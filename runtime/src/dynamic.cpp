#include "cafe/dynamic.h"

#include "cafe/export.h"
#include "cafe/layout.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "ppc.h"
#include "rpx.h"
#include "title/presentation.h"
#include "host/platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cafe::dynamic {
namespace {

using ppc::Op;

struct TraceEntry {
    uint32_t address{};
    uint32_t word{};
};
constexpr size_t kTraceEntries = 64;
thread_local std::array<TraceEntry, kTraceEntries> t_trace{};
thread_local size_t t_trace_cursor = 0;
thread_local size_t t_trace_count = 0;
thread_local uint64_t t_blocks_executed = 0;
thread_local uint64_t t_instructions_executed = 0;
thread_local uint64_t t_import_calls = 0;
thread_local uint64_t t_host_calls = 0;
thread_local uint64_t t_run_calls = 0;
thread_local auto t_last_progress = std::chrono::steady_clock::now();
bool g_full_instruction_trace = false;
bool g_dynamic_boot_trace = false;
bool g_keep_recent_trace = false;

void trace_dynamic_state(const char* kind, uint32_t pc, uint32_t stop,
                         const PPCContext& ctx) {
    char line[224]{};
    std::snprintf(line, sizeof(line),
                  "dynamic %s pc=0x%08X stop=0x%08X lr=0x%08X sp=0x%08X ctr=0x%08X "
                  "blocks=%llu instructions=%llu imports=%llu hosts=%llu",
                  kind, pc, stop, ctx.lr, ctx.r[1], ctx.ctr,
                  static_cast<unsigned long long>(t_blocks_executed),
                  static_cast<unsigned long long>(t_instructions_executed),
                  static_cast<unsigned long long>(t_import_calls),
                  static_cast<unsigned long long>(t_host_calls));
    diagnostic_trace(line);
}

void remember_instruction(uint32_t address, uint32_t word) {
    t_trace[t_trace_cursor] = {address, word};
    t_trace_cursor = (t_trace_cursor + 1) % kTraceEntries;
    t_trace_count = std::min(t_trace_count + 1, kTraceEntries);
}

struct Decoded {
    uint32_t address{};
    uint32_t word{};
    Op op{Op::invalid};
    uint8_t d{};
    uint8_t ra{};
    uint8_t rb{};
    uint8_t rc{};
    int16_t simm{};
    uint16_t uimm{};
    uint32_t direct_target{};
};

using BlockFn = uint32_t (*)(PPCContext*, uint8_t*);

struct Block {
    uint32_t start{};
    const Decoded* first{};
    uint16_t count{};
    BlockFn native{};
};

constexpr uint32_t kPresentationHook = 0x0864107Cu;
constexpr size_t kMaxBlockInstructions = 64;

std::unordered_map<uint32_t, std::unique_ptr<Block>> g_blocks;
std::shared_mutex g_block_mutex;

// Hot guest loops revisit the same basic blocks constantly. Avoid taking the
// global block mutex and hashing into g_blocks on every iteration by keeping a
// small direct-mapped cache per host thread. The epoch invalidates pointers
// whenever a new program is initialized.
struct HotBlockEntry {
    uint32_t address{};
    Block* block{};
};
constexpr size_t kHotBlockCacheSize = 32768;
thread_local std::array<HotBlockEntry, kHotBlockCacheSize> t_hot_blocks{};
thread_local uint64_t t_hot_block_epoch = 0;
std::atomic<uint64_t> g_hot_block_epoch{1};

std::filesystem::path g_block_cache_path;
std::unordered_set<uint32_t> g_known_block_starts;
std::vector<uint32_t> g_pending_block_starts;
bool g_cache_exit_registered = false;
constexpr size_t kBlockCacheFlushBatch = 8192;

#ifdef _WIN32
constexpr size_t kJitArenaSize = 64u << 20;
uint8_t* g_jit_arena = nullptr;
size_t g_jit_offset = 0;
bool g_jit_enabled = true;
size_t g_jit_blocks = 0;
size_t g_jit_direct_instructions = 0;
size_t g_jit_native_tails = 0;
#endif

struct CodeSection {
    uint32_t begin{};
    uint32_t end{};
    std::vector<Decoded> code;
};

struct ImportBinding {
    std::string module;
    std::string name;
    PPCFunc* function{};
    bool is_data{};
    uint32_t data_address{};
};

std::mutex g_mutex;
std::atomic<bool> g_enabled{false};
std::vector<CodeSection> g_code;
std::unordered_map<uint32_t, uint32_t> g_rel24_targets;
std::unordered_map<uint32_t, ImportBinding> g_imports;
uint32_t g_import_min = UINT32_MAX;
uint32_t g_import_max = 0;

void flush_block_cache_locked() {
    if (g_block_cache_path.empty() || g_pending_block_starts.empty()) return;
    std::ofstream output(g_block_cache_path, std::ios::binary | std::ios::app);
    if (!output) return;
    output.write(reinterpret_cast<const char*>(g_pending_block_starts.data()),
                 static_cast<std::streamsize>(g_pending_block_starts.size() * sizeof(uint32_t)));
    if (output) g_pending_block_starts.clear();
}

void flush_block_cache_at_exit() {
    std::lock_guard lock(g_block_mutex);
    flush_block_cache_locked();
}

std::vector<uint32_t> prepare_persistent_block_cache(const std::vector<uint8_t>& rpx_bytes) {
    std::vector<uint32_t> warm;
    std::error_code ec;
    std::filesystem::path directory;
    if (const char* custom = std::getenv("TTT2_CODE_CACHE_DIR"); custom && *custom) {
        directory = custom;
    } else {
        directory = host::data_directory() / "ttt2" / "code-cache";
    }
    std::filesystem::create_directories(directory, ec);
    if (ec) return warm;

    g_block_cache_path = directory / (sha256_hex(rpx_bytes) + ".blocks-v1");
    constexpr char kMagic[8] = {'T','T','T','2','B','L','K','1'};

    std::ifstream input(g_block_cache_path, std::ios::binary);
    if (input) {
        char magic[8]{};
        input.read(magic, sizeof(magic));
        if (input && std::memcmp(magic, kMagic, sizeof(kMagic)) == 0) {
            uint32_t address = 0;
            while (input.read(reinterpret_cast<char*>(&address), sizeof(address))) {
                if (g_known_block_starts.insert(address).second) warm.push_back(address);
            }
        } else {
            input.close();
            std::filesystem::remove(g_block_cache_path, ec);
        }
    }

    if (!std::filesystem::exists(g_block_cache_path, ec)) {
        std::ofstream output(g_block_cache_path, std::ios::binary | std::ios::trunc);
        if (output) output.write(kMagic, sizeof(kMagic));
    }

    if (!g_cache_exit_registered) {
        std::atexit(flush_block_cache_at_exit);
        g_cache_exit_registered = true;
    }
    return warm;
}

void remember_block_locked(uint32_t start) {
    if (g_block_cache_path.empty()) return;
    if (!g_known_block_starts.insert(start).second) return;
    g_pending_block_starts.push_back(start);
    if (g_pending_block_starts.size() >= kBlockCacheFlushBatch) {
        flush_block_cache_locked();
    }
}

bool ends_block(Op op) {
    switch (op) {
    case Op::b:
    case Op::bc:
    case Op::bclr:
    case Op::bcctr:
    case Op::tw:
    case Op::twi:
    case Op::sc:
    case Op::rfi:
        return true;
    default:
        return false;
    }
}

#ifdef _WIN32
void reset_jit_arena() {
    if (g_jit_arena) {
        VirtualFree(g_jit_arena, 0, MEM_RELEASE);
        g_jit_arena = nullptr;
    }
    g_jit_offset = 0;
    g_jit_blocks = 0;
    g_jit_direct_instructions = 0;
    g_jit_native_tails = 0;
    const char* option = std::getenv("TTT2_DYNAMIC_JIT");
    g_jit_enabled = option == nullptr || *option == '\0' || *option != '0';
    if (!g_jit_enabled) return;

    g_jit_arena = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kJitArenaSize, MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE));
    if (!g_jit_arena) {
        g_jit_enabled = false;
        std::fprintf(stderr, "ttt2: dynamic host: x64 tier-1 JIT arena unavailable; using C++ blocks\n");
    }
}
#endif

uint32_t rotate_mask(uint32_t mb, uint32_t me) {
    const uint32_t begin = 0xFFFFFFFFu >> mb;
    const uint32_t end = me < 31 ? 0xFFFFFFFFu >> (me + 1) : 0;
    const uint32_t m = begin ^ end;
    return mb <= me ? m : ~m;
}

void record(PPCContext& ctx, uint32_t value) {
    ppc::cmp_signed(ctx.cr[0], static_cast<int32_t>(value), 0, ctx.xer_so);
}

uint8_t get_cr_bit(const PPCContext& ctx, uint32_t bit) {
    const CRField& f = ctx.cr[(bit >> 2) & 7];
    switch (bit & 3) {
    case 0: return f.lt;
    case 1: return f.gt;
    case 2: return f.eq;
    default: return f.so;
    }
}

void set_cr_bit(PPCContext& ctx, uint32_t bit, uint8_t value) {
    CRField& f = ctx.cr[(bit >> 2) & 7];
    value = value ? 1 : 0;
    switch (bit & 3) {
    case 0: f.lt = value; break;
    case 1: f.gt = value; break;
    case 2: f.eq = value; break;
    default: f.so = value; break;
    }
}

uint32_t ea_d(const PPCContext& ctx, uint32_t ra, int32_t displacement) {
    return (ra ? ctx.r[ra] : 0u) + static_cast<uint32_t>(displacement);
}

uint32_t ea_x(const PPCContext& ctx, uint32_t ra, uint32_t rb) {
    return (ra ? ctx.r[ra] : 0u) + ctx.r[rb];
}

void add_family(PPCContext& ctx, uint32_t w, uint32_t x, uint32_t y,
                uint32_t carry, bool sets_ca) {
    const uint32_t d = ppc::rd(w);
    const uint64_t t = uint64_t{x} + y + carry;
    const uint32_t result = static_cast<uint32_t>(t);
    ctx.r[d] = result;
    if (sets_ca) ctx.xer_ca = static_cast<uint8_t>(t >> 32);
    if (ppc::oe(w)) {
        ctx.xer_ov = static_cast<uint8_t>((((x ^ result) & (y ^ result)) >> 31) != 0);
        ctx.xer_so |= ctx.xer_ov;
    }
    if (ppc::rc(w)) record(ctx, result);
}

bool branch_condition(PPCContext& ctx, uint32_t bo, uint32_t bi) {
    bool take = true;
    if (!(bo & 4)) {
        --ctx.ctr;
        take &= (bo & 2) ? ctx.ctr == 0 : ctx.ctr != 0;
    }
    if (!(bo & 0x10)) {
        const bool bit = get_cr_bit(ctx, bi) != 0;
        take &= (bo & 8) ? bit : !bit;
    }
    return take;
}

Decoded decode_instruction(uint32_t address, uint32_t word) {
    const Op op = ppc::decode(word);
    uint32_t target = 0;
    if (op == Op::b || op == Op::bc) {
        const auto relocated = g_rel24_targets.find(address);
        target = relocated != g_rel24_targets.end() ? relocated->second
                                                    : ppc::branch_target(word, address);
    }
    return {address,
            word,
            op,
            static_cast<uint8_t>(ppc::rd(word)),
            static_cast<uint8_t>(ppc::ra(word)),
            static_cast<uint8_t>(ppc::rb(word)),
            static_cast<uint8_t>(ppc::frc(word)),
            static_cast<int16_t>(ppc::simm(word)),
            static_cast<uint16_t>(ppc::uimm(word)),
            target};
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
void load_integer(PPCContext& ctx, uint8_t* base, const Decoded& ins, int width,
                  bool sign_extend, bool update, bool indexed, bool byte_reverse = false) {
    const uint32_t d = ins.d;
    const uint32_t ra = ins.ra;
    const uint32_t ea = indexed ? ea_x(ctx, ra, ins.rb)
                                : ea_d(ctx, ra, ins.simm);
    uint32_t value = 0;
    if (width == 1) {
        value = ppc::ld8(base, ea);
    } else if (width == 2) {
        const uint16_t raw = byte_reverse ? ppc::ld16_le(base, ea) : ppc::ld16(base, ea);
        value = sign_extend ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)))
                            : raw;
    } else {
        value = byte_reverse ? ppc::ld32_le(base, ea) : ppc::ld32(base, ea);
    }
    ctx.r[d] = value;
    if (update) ctx.r[ra] = ea;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
void store_integer(PPCContext& ctx, uint8_t* base, const Decoded& ins, int width,
                   bool update, bool indexed, bool byte_reverse = false) {
    const uint32_t s = ins.d;
    const uint32_t ra = ins.ra;
    const uint32_t ea = indexed ? ea_x(ctx, ra, ins.rb)
                                : ea_d(ctx, ra, ins.simm);
    if (width == 1) {
        ppc::st8(base, ea, static_cast<uint8_t>(ctx.r[s]));
    } else if (width == 2) {
        if (byte_reverse) ppc::st16_le(base, ea, static_cast<uint16_t>(ctx.r[s]));
        else ppc::st16(base, ea, static_cast<uint16_t>(ctx.r[s]));
    } else {
        if (byte_reverse) ppc::st32_le(base, ea, ctx.r[s]);
        else ppc::st32(base, ea, ctx.r[s]);
    }
    if (update) ctx.r[ra] = ea;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
void fload(PPCContext& ctx, uint8_t* base, const Decoded& ins, bool single,
           bool update, bool indexed) {
    const uint32_t d = ins.d;
    const uint32_t ra = ins.ra;
    const uint32_t ea = indexed ? ea_x(ctx, ra, ins.rb)
                                : ea_d(ctx, ra, ins.simm);
    if (single) {
        const double value = ppc::load_single(ppc::ld32(base, ea));
        ctx.f[d].ps0 = value;
        ctx.f[d].ps1 = value;
    } else {
        ctx.f[d].ps0 = ppc::from_bits(ppc::ld64(base, ea));
    }
    if (update) ctx.r[ra] = ea;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
void fstore(PPCContext& ctx, uint8_t* base, const Decoded& ins, bool single,
            bool update, bool indexed, bool integer_word = false) {
    const uint32_t s = ins.d;
    const uint32_t ra = ins.ra;
    const uint32_t ea = indexed ? ea_x(ctx, ra, ins.rb)
                                : ea_d(ctx, ra, ins.simm);
    if (integer_word) {
        ppc::st32(base, ea, static_cast<uint32_t>(ppc::bits(ctx.f[s].ps0)));
    } else if (single) {
        ppc::st32(base, ea, ppc::store_single(ctx.f[s].ps0));
    } else {
        ppc::st64(base, ea, ppc::bits(ctx.f[s].ps0));
    }
    if (update) ctx.r[ra] = ea;
}

void psq(PPCContext& ctx, uint8_t* base, uint32_t w, bool load, bool update,
         bool indexed) {
    const uint32_t fr = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t ea = indexed ? ea_x(ctx, ra, ppc::rb(w))
                                : ea_d(ctx, ra, ppc::psq_d(w));
    const bool wbit = indexed ? ppc::psqx_w(w) : ppc::psq_w(w);
    const uint32_t gqr = indexed ? ppc::psqx_i(w) : ppc::psq_i(w);
    if (load) {
        if (wbit) ppc::psq_load<true>(ctx, base, fr, ea, ctx.gqr[gqr]);
        else ppc::psq_load<false>(ctx, base, fr, ea, ctx.gqr[gqr]);
    } else {
        if (wbit) ppc::psq_store<true>(ctx, base, fr, ea, ctx.gqr[gqr]);
        else ppc::psq_store<false>(ctx, base, fr, ea, ctx.gqr[gqr]);
    }
    if (update) ctx.r[ra] = ea;
}

void pair_write(PPCContext& ctx, uint32_t d, double p0, double p1) {
    ctx.f[d].ps0 = p0;
    ctx.f[d].ps1 = p1;
}

void fill_write(PPCContext& ctx, uint32_t d, double value) {
    ctx.f[d].ps0 = value;
    ctx.f[d].ps1 = value;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
uint32_t execute_one(PPCContext& ctx, uint8_t* base, const Decoded& ins) {
    const uint32_t a = ins.address;
    const uint32_t w = ins.word;
    const Op op = ins.op;
    const uint32_t d = ins.d;
    const uint32_t ra = ins.ra;
    const uint32_t rb = ins.rb;
    const uint32_t rc = ins.rc;
    const int32_t simm = ins.simm;
    const uint32_t uimm = ins.uimm;
    uint32_t next = a + 4;

    if ((w & 1u) != 0 && op >= Op::fadd && ppc::has_record_form(op)) {
        cafe_ppc_illegal(ctx, a, w, "floating-point record form");
    }

    switch (op) {
    case Op::addi:
        ctx.r[d] = (ra ? ctx.r[ra] : 0u) + static_cast<uint32_t>(simm);
        if (a == 0x02EF885Cu && cafe_ppc_runtime_option(1)) {
            ctx.r[7] = 0;
        }
        break;
    case Op::addis:
        ctx.r[d] = (ra ? ctx.r[ra] : 0u) + (static_cast<uint32_t>(simm) << 16);
        break;
    case Op::addic:
    case Op::addic_rc: {
        const uint32_t x = ctx.r[ra];
        ctx.r[d] = x + static_cast<uint32_t>(simm);
        ctx.xer_ca = ctx.r[d] < x;
        if (op == Op::addic_rc) record(ctx, ctx.r[d]);
        break;
    }
    case Op::subfic: {
        const uint64_t t = uint64_t{~ctx.r[ra]} + static_cast<uint32_t>(simm) + 1;
        ctx.r[d] = static_cast<uint32_t>(t);
        ctx.xer_ca = static_cast<uint8_t>(t >> 32);
        break;
    }
    case Op::mulli:
        ctx.r[d] = ctx.r[ra] * static_cast<uint32_t>(simm);
        break;
    case Op::add: add_family(ctx, w, ctx.r[ra], ctx.r[rb], 0, false); break;
    case Op::addc: add_family(ctx, w, ctx.r[ra], ctx.r[rb], 0, true); break;
    case Op::adde: add_family(ctx, w, ctx.r[ra], ctx.r[rb], ctx.xer_ca, true); break;
    case Op::addze: add_family(ctx, w, ctx.r[ra], 0u, ctx.xer_ca, true); break;
    case Op::addme: add_family(ctx, w, ctx.r[ra], 0xFFFFFFFFu, ctx.xer_ca, true); break;
    case Op::subf: add_family(ctx, w, ~ctx.r[ra], ctx.r[rb], 1, false); break;
    case Op::subfc: add_family(ctx, w, ~ctx.r[ra], ctx.r[rb], 1, true); break;
    case Op::subfe: add_family(ctx, w, ~ctx.r[ra], ctx.r[rb], ctx.xer_ca, true); break;
    case Op::subfze: add_family(ctx, w, ~ctx.r[ra], 0u, ctx.xer_ca, true); break;
    case Op::subfme: add_family(ctx, w, ~ctx.r[ra], 0xFFFFFFFFu, ctx.xer_ca, true); break;
    case Op::neg: add_family(ctx, w, ~ctx.r[ra], 0u, 1, false); break;

    case Op::mullw: {
        if (ppc::oe(w)) {
            const int64_t p = int64_t{static_cast<int32_t>(ctx.r[ra])} *
                              static_cast<int32_t>(ctx.r[rb]);
            ctx.r[d] = static_cast<uint32_t>(p);
            ctx.xer_ov = p != static_cast<int32_t>(p);
            ctx.xer_so |= ctx.xer_ov;
        } else {
            ctx.r[d] = ctx.r[ra] * ctx.r[rb];
        }
        if (ppc::rc(w)) record(ctx, ctx.r[d]);
        break;
    }
    case Op::mulhw:
        ctx.r[d] = static_cast<uint32_t>((int64_t{static_cast<int32_t>(ctx.r[ra])} *
                                          static_cast<int32_t>(ctx.r[rb])) >> 32);
        if (ppc::rc(w)) record(ctx, ctx.r[d]);
        break;
    case Op::mulhwu:
        ctx.r[d] = static_cast<uint32_t>((uint64_t{ctx.r[ra]} * ctx.r[rb]) >> 32);
        if (ppc::rc(w)) record(ctx, ctx.r[d]);
        break;
    case Op::divw:
    case Op::divwu: {
        const uint32_t x = ctx.r[ra], y = ctx.r[rb];
        if (ppc::oe(w)) {
            ctx.xer_ov = op == Op::divw ? ppc::divw_overflows(x, y) : y == 0;
            ctx.xer_so |= ctx.xer_ov;
        }
        ctx.r[d] = op == Op::divw ? ppc::divw(x, y) : ppc::divwu(x, y);
        if (ppc::rc(w)) record(ctx, ctx.r[d]);
        break;
    }

    case Op::cmp:
        ppc::cmp_signed(ctx.cr[ppc::crfd(w)], static_cast<int32_t>(ctx.r[ra]),
                        static_cast<int32_t>(ctx.r[rb]), ctx.xer_so);
        break;
    case Op::cmpl:
        ppc::cmp_unsigned(ctx.cr[ppc::crfd(w)], ctx.r[ra], ctx.r[rb], ctx.xer_so);
        break;
    case Op::cmpi:
        ppc::cmp_signed(ctx.cr[ppc::crfd(w)], static_cast<int32_t>(ctx.r[ra]),
                        simm, ctx.xer_so);
        break;
    case Op::cmpli:
        ppc::cmp_unsigned(ctx.cr[ppc::crfd(w)], ctx.r[ra], uimm, ctx.xer_so);
        break;

    case Op::and_: ctx.r[ra] = ctx.r[d] & ctx.r[rb]; if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::andc: ctx.r[ra] = ctx.r[d] & ~ctx.r[rb]; if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::or_: ctx.r[ra] = ctx.r[d] | ctx.r[rb]; if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::orc: ctx.r[ra] = ctx.r[d] | ~ctx.r[rb]; if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::xor_: ctx.r[ra] = ctx.r[d] ^ ctx.r[rb]; if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::nand: ctx.r[ra] = ~(ctx.r[d] & ctx.r[rb]); if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::nor: ctx.r[ra] = ~(ctx.r[d] | ctx.r[rb]); if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::eqv: ctx.r[ra] = ~(ctx.r[d] ^ ctx.r[rb]); if (ppc::rc(w)) record(ctx, ctx.r[ra]); break;
    case Op::andi_rc: ctx.r[ra] = ctx.r[d] & uimm; record(ctx, ctx.r[ra]); break;
    case Op::andis_rc: ctx.r[ra] = ctx.r[d] & (uimm << 16); record(ctx, ctx.r[ra]); break;
    case Op::ori: ctx.r[ra] = ctx.r[d] | uimm; break;
    case Op::oris: ctx.r[ra] = ctx.r[d] | (uimm << 16); break;
    case Op::xori: ctx.r[ra] = ctx.r[d] ^ uimm; break;
    case Op::xoris: ctx.r[ra] = ctx.r[d] ^ (uimm << 16); break;
    case Op::extsb:
        ctx.r[ra] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ctx.r[d])));
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    case Op::extsh:
        ctx.r[ra] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ctx.r[d])));
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    case Op::cntlzw:
        ctx.r[ra] = static_cast<uint32_t>(std::countl_zero(ctx.r[d]));
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;

    case Op::rlwinm: {
        const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
        ctx.r[ra] = ppc::rotl(ctx.r[d], rb) & m;
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    }
    case Op::rlwnm: {
        const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
        ctx.r[ra] = ppc::rotl(ctx.r[d], ctx.r[rb]) & m;
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    }
    case Op::rlwimi: {
        const uint32_t m = rotate_mask(ppc::mb(w), ppc::me(w));
        ctx.r[ra] = (ppc::rotl(ctx.r[d], rb) & m) | (ctx.r[ra] & ~m);
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    }
    case Op::slw:
        ctx.r[ra] = (ctx.r[rb] & 0x20) ? 0u : ctx.r[d] << (ctx.r[rb] & 31);
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    case Op::srw:
        ctx.r[ra] = (ctx.r[rb] & 0x20) ? 0u : ctx.r[d] >> (ctx.r[rb] & 31);
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    case Op::sraw:
        ctx.r[ra] = ppc::sraw(ctx.xer_ca, ctx.r[d], ctx.r[rb] & 0x3F);
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;
    case Op::srawi:
        ctx.r[ra] = ppc::sraw(ctx.xer_ca, ctx.r[d], rb);
        if (ppc::rc(w)) record(ctx, ctx.r[ra]);
        break;

    case Op::lbz: load_integer(ctx, base, ins, 1, false, false, false); break;
    case Op::lbzu: load_integer(ctx, base, ins, 1, false, true, false); break;
    case Op::lbzx: load_integer(ctx, base, ins, 1, false, false, true); break;
    case Op::lbzux: load_integer(ctx, base, ins, 1, false, true, true); break;
    case Op::lhz: load_integer(ctx, base, ins, 2, false, false, false); break;
    case Op::lhzu: load_integer(ctx, base, ins, 2, false, true, false); break;
    case Op::lhzx: load_integer(ctx, base, ins, 2, false, false, true); break;
    case Op::lhzux: load_integer(ctx, base, ins, 2, false, true, true); break;
    case Op::lha: load_integer(ctx, base, ins, 2, true, false, false); break;
    case Op::lhau: load_integer(ctx, base, ins, 2, true, true, false); break;
    case Op::lhax: load_integer(ctx, base, ins, 2, true, false, true); break;
    case Op::lhaux: load_integer(ctx, base, ins, 2, true, true, true); break;
    case Op::lwz: load_integer(ctx, base, ins, 4, false, false, false); break;
    case Op::lwzu: load_integer(ctx, base, ins, 4, false, true, false); break;
    case Op::lwzx: load_integer(ctx, base, ins, 4, false, false, true); break;
    case Op::lwzux: load_integer(ctx, base, ins, 4, false, true, true); break;
    case Op::lhbrx: load_integer(ctx, base, ins, 2, false, false, true, true); break;
    case Op::lwbrx: load_integer(ctx, base, ins, 4, false, false, true, true); break;

    case Op::stb: store_integer(ctx, base, ins, 1, false, false); break;
    case Op::stbu: store_integer(ctx, base, ins, 1, true, false); break;
    case Op::stbx: store_integer(ctx, base, ins, 1, false, true); break;
    case Op::stbux: store_integer(ctx, base, ins, 1, true, true); break;
    case Op::sth: store_integer(ctx, base, ins, 2, false, false); break;
    case Op::sthu: store_integer(ctx, base, ins, 2, true, false); break;
    case Op::sthx: store_integer(ctx, base, ins, 2, false, true); break;
    case Op::sthux: store_integer(ctx, base, ins, 2, true, true); break;
    case Op::stw: store_integer(ctx, base, ins, 4, false, false); break;
    case Op::stwu: store_integer(ctx, base, ins, 4, true, false); break;
    case Op::stwx: store_integer(ctx, base, ins, 4, false, true); break;
    case Op::stwux: store_integer(ctx, base, ins, 4, true, true); break;
    case Op::sthbrx: store_integer(ctx, base, ins, 2, false, true, true); break;
    case Op::stwbrx: store_integer(ctx, base, ins, 4, false, true, true); break;

    case Op::lmw:
        ppc::lmw(ctx, base, d, ea_d(ctx, ra, simm));
        break;
    case Op::stmw:
        ppc::stmw(ctx, base, d, ea_d(ctx, ra, simm));
        break;
    case Op::lswi:
        ppc::lsw(ctx, base, d, ra ? ctx.r[ra] : 0u, rb ? rb : 32);
        break;
    case Op::stswi:
        ppc::stsw(ctx, base, d, ra ? ctx.r[ra] : 0u, rb ? rb : 32);
        break;
    case Op::lswx:
        ppc::lsw(ctx, base, d, ea_x(ctx, ra, rb), ctx.xer_bc);
        break;
    case Op::stswx:
        ppc::stsw(ctx, base, d, ea_x(ctx, ra, rb), ctx.xer_bc);
        break;
    case Op::lwarx:
        ctx.r[d] = ppc::lwarx(ctx, base, ea_x(ctx, ra, rb));
        break;
    case Op::stwcx: {
        const bool stored = ppc::stwcx(ctx, base, ea_x(ctx, ra, rb), ctx.r[d]);
        ctx.cr[0] = {0, 0, static_cast<uint8_t>(stored), ctx.xer_so};
        break;
    }

    case Op::b:
        if (ppc::lk(w)) ctx.lr = a + 4;
        next = ins.direct_target;
        break;
    case Op::bc: {
        if (ppc::lk(w)) ctx.lr = a + 4;
        if (branch_condition(ctx, d, ra)) next = ins.direct_target;
        break;
    }
    case Op::bclr: {
        const uint32_t target = ctx.lr;
        if (ppc::lk(w)) ctx.lr = a + 4;
        if (branch_condition(ctx, d, ra)) next = target;
        break;
    }
    case Op::bcctr: {
        const uint32_t target = ctx.ctr;
        if (!(d & 4)) cafe_ppc_illegal(ctx, a, w, "bcctr with CTR decrement");
        if (ppc::lk(w)) ctx.lr = a + 4;
        if (branch_condition(ctx, d, ra)) next = target;
        break;
    }
    case Op::tw:
    case Op::twi: {
        const uint32_t x = ctx.r[ra];
        const uint32_t y = op == Op::tw ? ctx.r[rb] : static_cast<uint32_t>(simm);
        bool trap = false;
        if (d & 16) trap |= static_cast<int32_t>(x) < static_cast<int32_t>(y);
        if (d & 8) trap |= static_cast<int32_t>(x) > static_cast<int32_t>(y);
        if (d & 4) trap |= x == y;
        if (d & 2) trap |= x < y;
        if (d & 1) trap |= x > y;
        if (trap) cafe_ppc_trap(ctx, a);
        break;
    }

    case Op::mcrf:
        ctx.cr[ppc::crfd(w)] = ctx.cr[ppc::crfs(w)];
        break;
    case Op::crand: set_cr_bit(ctx, d, get_cr_bit(ctx, ra) & get_cr_bit(ctx, rb)); break;
    case Op::crandc: set_cr_bit(ctx, d, get_cr_bit(ctx, ra) & !get_cr_bit(ctx, rb)); break;
    case Op::creqv: set_cr_bit(ctx, d, !(get_cr_bit(ctx, ra) ^ get_cr_bit(ctx, rb))); break;
    case Op::crnand: set_cr_bit(ctx, d, !(get_cr_bit(ctx, ra) & get_cr_bit(ctx, rb))); break;
    case Op::crnor: set_cr_bit(ctx, d, !(get_cr_bit(ctx, ra) | get_cr_bit(ctx, rb))); break;
    case Op::cror: set_cr_bit(ctx, d, get_cr_bit(ctx, ra) | get_cr_bit(ctx, rb)); break;
    case Op::crorc: set_cr_bit(ctx, d, get_cr_bit(ctx, ra) | !get_cr_bit(ctx, rb)); break;
    case Op::crxor: set_cr_bit(ctx, d, get_cr_bit(ctx, ra) ^ get_cr_bit(ctx, rb)); break;
    case Op::mfcr:
        ctx.r[d] = ppc::mfcr(ctx);
        break;
    case Op::mtcrf:
        ppc::mtcrf(ctx, ppc::crm(w), ctx.r[d]);
        break;
    case Op::mcrxr:
        ctx.cr[ppc::crfd(w)] = {ctx.xer_so, ctx.xer_ov, ctx.xer_ca, 0};
        ctx.xer_so = ctx.xer_ov = ctx.xer_ca = 0;
        break;

    case Op::mfspr: {
        const uint32_t spr = ppc::spr(w);
        if (spr == 1) ctx.r[d] = ppc::mfxer(ctx);
        else if (spr == 8) ctx.r[d] = ctx.lr;
        else if (spr == 9) ctx.r[d] = ctx.ctr;
        else if (spr >= 896 && spr < 904) ctx.r[d] = ctx.gqr[spr - 896];
        else if (spr >= 912 && spr < 920) ctx.r[d] = ctx.gqr[spr - 912];
        else if (spr == 268) ctx.r[d] = static_cast<uint32_t>(cafe_ppc_timebase());
        else if (spr == 269) ctx.r[d] = static_cast<uint32_t>(cafe_ppc_timebase() >> 32);
        else cafe_ppc_illegal(ctx, a, w, "mfspr of an unsupported register");
        break;
    }
    case Op::mtspr: {
        const uint32_t spr = ppc::spr(w);
        if (spr == 1) ppc::mtxer(ctx, ctx.r[d]);
        else if (spr == 8) ctx.lr = ctx.r[d];
        else if (spr == 9) ctx.ctr = ctx.r[d];
        else if (spr >= 896 && spr < 904) ctx.gqr[spr - 896] = ctx.r[d];
        else if (spr >= 912 && spr < 920) ctx.gqr[spr - 912] = ctx.r[d];
        else cafe_ppc_illegal(ctx, a, w, "mtspr of an unsupported register");
        break;
    }
    case Op::mftb: {
        const uint32_t tbr = ppc::spr(w);
        if (tbr == 268) ctx.r[d] = static_cast<uint32_t>(cafe_ppc_timebase());
        else if (tbr == 269) ctx.r[d] = static_cast<uint32_t>(cafe_ppc_timebase() >> 32);
        else cafe_ppc_illegal(ctx, a, w, "mftb of an unknown time-base register");
        break;
    }

    case Op::sync:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        break;
    case Op::isync:
    case Op::eieio:
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        break;
    case Op::dcbf:
    case Op::dcbi:
    case Op::dcbst:
    case Op::dcbt:
    case Op::dcbtst:
    case Op::icbi:
        break;
    case Op::dcbz:
    case Op::dcbz_l:
        ppc::dcbz(base, ea_x(ctx, ra, rb));
        break;

    case Op::lfs: fload(ctx, base, ins, true, false, false); break;
    case Op::lfsu: fload(ctx, base, ins, true, true, false); break;
    case Op::lfsx: fload(ctx, base, ins, true, false, true); break;
    case Op::lfsux: fload(ctx, base, ins, true, true, true); break;
    case Op::lfd: fload(ctx, base, ins, false, false, false); break;
    case Op::lfdu: fload(ctx, base, ins, false, true, false); break;
    case Op::lfdx: fload(ctx, base, ins, false, false, true); break;
    case Op::lfdux: fload(ctx, base, ins, false, true, true); break;
    case Op::stfs: fstore(ctx, base, ins, true, false, false); break;
    case Op::stfsu: fstore(ctx, base, ins, true, true, false); break;
    case Op::stfsx: fstore(ctx, base, ins, true, false, true); break;
    case Op::stfsux: fstore(ctx, base, ins, true, true, true); break;
    case Op::stfd: fstore(ctx, base, ins, false, false, false); break;
    case Op::stfdu: fstore(ctx, base, ins, false, true, false); break;
    case Op::stfdx: fstore(ctx, base, ins, false, false, true); break;
    case Op::stfdux: fstore(ctx, base, ins, false, true, true); break;
    case Op::stfiwx: fstore(ctx, base, ins, false, false, true, true); break;

    case Op::fadd: ctx.f[d].ps0 = ppc::add(ctx.f[ra].ps0, ctx.f[rb].ps0); break;
    case Op::fsub: ctx.f[d].ps0 = ppc::sub(ctx.f[ra].ps0, ctx.f[rb].ps0); break;
    case Op::fmul: ctx.f[d].ps0 = ppc::mul(ctx.f[ra].ps0, ctx.f[rc].ps0); break;
    case Op::fdiv: ctx.f[d].ps0 = ppc::div(ctx.f[ra].ps0, ctx.f[rb].ps0); break;
    case Op::fadds:
        fill_write(ctx, d, ppc::round_single(ppc::add(ctx.f[ra].ps0, ctx.f[rb].ps0)));
        break;
    case Op::fsubs:
        fill_write(ctx, d, ppc::round_single(ppc::sub(ctx.f[ra].ps0, ctx.f[rb].ps0)));
        break;
    case Op::fmuls:
        fill_write(ctx, d, ppc::round_single(ppc::mul(ctx.f[ra].ps0, ppc::round_25bit(ctx.f[rc].ps0))));
        break;
    case Op::fdivs:
        fill_write(ctx, d, ppc::round_single(ppc::div(ctx.f[ra].ps0, ctx.f[rb].ps0)));
        break;
    case Op::fmadd:
        ctx.f[d].ps0 = ppc::madd<false, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0);
        break;
    case Op::fmsub:
        ctx.f[d].ps0 = ppc::madd<false, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0);
        break;
    case Op::fnmadd:
        ctx.f[d].ps0 = ppc::negate_unless_nan(ppc::madd<false, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0));
        break;
    case Op::fnmsub:
        ctx.f[d].ps0 = ppc::negate_unless_nan(ppc::madd<false, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0));
        break;
    case Op::fmadds:
        fill_write(ctx, d, ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0)));
        break;
    case Op::fmsubs:
        fill_write(ctx, d, ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0)));
        break;
    case Op::fnmadds:
        fill_write(ctx, d, ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0))));
        break;
    case Op::fnmsubs:
        fill_write(ctx, d, ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0))));
        break;
    case Op::fsqrt:
        ctx.f[d].ps0 = std::sqrt(ctx.f[rb].ps0);
        break;
    case Op::fsqrts:
        fill_write(ctx, d, ppc::round_single(std::sqrt(ctx.f[rb].ps0)));
        break;
    case Op::fres:
        fill_write(ctx, d, ppc::res(ctx.f[rb].ps0));
        break;
    case Op::frsqrte:
        ctx.f[d].ps0 = ppc::rsqrte(ctx.f[rb].ps0);
        break;
    case Op::fsel:
        ctx.f[d].ps0 = ppc::fsel(ctx.f[ra].ps0, ctx.f[rb].ps0, ctx.f[rc].ps0);
        break;
    case Op::frsp:
        fill_write(ctx, d, ppc::round_single(ctx.f[rb].ps0));
        break;
    case Op::fctiw:
        ctx.f[d].ps0 = ppc::fcti(ctx.f[rb].ps0, false);
        break;
    case Op::fctiwz:
        ctx.f[d].ps0 = ppc::fcti(ctx.f[rb].ps0, true);
        break;
    case Op::fmr:
        ctx.f[d].ps0 = ctx.f[rb].ps0;
        break;
    case Op::fneg:
        ctx.f[d].ps0 = ppc::from_bits(ppc::bits(ctx.f[rb].ps0) ^ ppc::kSign);
        break;
    case Op::fabs:
        ctx.f[d].ps0 = ppc::from_bits(ppc::bits(ctx.f[rb].ps0) & ~ppc::kSign);
        break;
    case Op::fnabs:
        ctx.f[d].ps0 = ppc::from_bits(ppc::bits(ctx.f[rb].ps0) | ppc::kSign);
        break;
    case Op::fcmpu:
    case Op::fcmpo:
        ppc::fcmp(ctx.cr[ppc::crfd(w)], ctx.f[ra].ps0, ctx.f[rb].ps0);
        break;

    case Op::psq_l: psq(ctx, base, w, true, false, false); break;
    case Op::psq_lu: psq(ctx, base, w, true, true, false); break;
    case Op::psq_lx: psq(ctx, base, w, true, false, true); break;
    case Op::psq_lux: psq(ctx, base, w, true, true, true); break;
    case Op::psq_st: psq(ctx, base, w, false, false, false); break;
    case Op::psq_stu: psq(ctx, base, w, false, true, false); break;
    case Op::psq_stx: psq(ctx, base, w, false, false, true); break;
    case Op::psq_stux: psq(ctx, base, w, false, true, true); break;

    case Op::ps_add:
        pair_write(ctx, d,
                   ppc::round_single(ppc::add(ctx.f[ra].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::add(ctx.f[ra].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_sub:
        pair_write(ctx, d,
                   ppc::round_single(ppc::sub(ctx.f[ra].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::sub(ctx.f[ra].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_div:
        pair_write(ctx, d,
                   ppc::round_single(ppc::div(ctx.f[ra].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::div(ctx.f[ra].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_mul:
        pair_write(ctx, d,
                   ppc::round_single(ppc::mul(ctx.f[ra].ps0, ppc::round_25bit(ctx.f[rc].ps0))),
                   ppc::round_single(ppc::mul(ctx.f[ra].ps1, ppc::round_25bit(ctx.f[rc].ps1))));
        break;
    case Op::ps_madd:
        pair_write(ctx, d,
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps1, ctx.f[rc].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_msub:
        pair_write(ctx, d,
                   ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps1, ctx.f[rc].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_nmadd:
        pair_write(ctx, d,
                   ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0))),
                   ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps1, ctx.f[rc].ps1, ctx.f[rb].ps1))));
        break;
    case Op::ps_nmsub:
        pair_write(ctx, d,
                   ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0))),
                   ppc::negate_unless_nan(ppc::round_single(ppc::madd<true, true>(ctx.f[ra].ps1, ctx.f[rc].ps1, ctx.f[rb].ps1))));
        break;
    case Op::ps_madds0:
        pair_write(ctx, d,
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps0, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps1, ctx.f[rc].ps0, ctx.f[rb].ps1)));
        break;
    case Op::ps_madds1:
        pair_write(ctx, d,
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps0, ctx.f[rc].ps1, ctx.f[rb].ps0)),
                   ppc::round_single(ppc::madd<true, false>(ctx.f[ra].ps1, ctx.f[rc].ps1, ctx.f[rb].ps1)));
        break;
    case Op::ps_muls0:
        pair_write(ctx, d,
                   ppc::round_single(ppc::mul(ctx.f[ra].ps0, ppc::round_25bit(ctx.f[rc].ps0))),
                   ppc::round_single(ppc::mul(ctx.f[ra].ps1, ppc::round_25bit(ctx.f[rc].ps0))));
        break;
    case Op::ps_muls1:
        pair_write(ctx, d,
                   ppc::round_single(ppc::mul(ctx.f[ra].ps0, ppc::round_25bit(ctx.f[rc].ps1))),
                   ppc::round_single(ppc::mul(ctx.f[ra].ps1, ppc::round_25bit(ctx.f[rc].ps1))));
        break;
    case Op::ps_sum0:
        pair_write(ctx, d,
                   ppc::round_single(ppc::add(ctx.f[ra].ps0, ctx.f[rb].ps1)),
                   ppc::round_single(ctx.f[rc].ps1));
        break;
    case Op::ps_sum1:
        pair_write(ctx, d,
                   ppc::round_single(ctx.f[rc].ps0),
                   ppc::round_single(ppc::add(ctx.f[ra].ps0, ctx.f[rb].ps1)));
        break;
    case Op::ps_sel:
        pair_write(ctx, d,
                   ppc::fsel(ctx.f[ra].ps0, ctx.f[rb].ps0, ctx.f[rc].ps0),
                   ppc::fsel(ctx.f[ra].ps1, ctx.f[rb].ps1, ctx.f[rc].ps1));
        break;
    case Op::ps_res:
        pair_write(ctx, d, ppc::res(ctx.f[rb].ps0), ppc::res(ctx.f[rb].ps1));
        break;
    case Op::ps_rsqrte:
        pair_write(ctx, d,
                   ppc::round_single(ppc::rsqrte(ctx.f[rb].ps0)),
                   ppc::round_single(ppc::rsqrte(ctx.f[rb].ps1)));
        break;
    case Op::ps_mr:
        pair_write(ctx, d, ctx.f[rb].ps0, ctx.f[rb].ps1);
        break;
    case Op::ps_neg:
        pair_write(ctx, d,
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps0) ^ ppc::kSign),
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps1) ^ ppc::kSign));
        break;
    case Op::ps_abs:
        pair_write(ctx, d,
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps0) & ~ppc::kSign),
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps1) & ~ppc::kSign));
        break;
    case Op::ps_nabs:
        pair_write(ctx, d,
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps0) | ppc::kSign),
                   ppc::from_bits(ppc::bits(ctx.f[rb].ps1) | ppc::kSign));
        break;
    case Op::ps_merge00: pair_write(ctx, d, ctx.f[ra].ps0, ctx.f[rb].ps0); break;
    case Op::ps_merge01: pair_write(ctx, d, ctx.f[ra].ps0, ctx.f[rb].ps1); break;
    case Op::ps_merge10: pair_write(ctx, d, ctx.f[ra].ps1, ctx.f[rb].ps0); break;
    case Op::ps_merge11: pair_write(ctx, d, ctx.f[ra].ps1, ctx.f[rb].ps1); break;
    case Op::ps_cmpu0:
    case Op::ps_cmpo0:
        ppc::fcmp(ctx.cr[ppc::crfd(w)], ctx.f[ra].ps0, ctx.f[rb].ps0);
        break;
    case Op::ps_cmpu1:
    case Op::ps_cmpo1:
        ppc::fcmp(ctx.cr[ppc::crfd(w)], ctx.f[ra].ps1, ctx.f[rb].ps1);
        break;

    case Op::sc:
    case Op::rfi:
    case Op::tdi:
    case Op::mfmsr:
    case Op::mtmsr:
    case Op::mfsr:
    case Op::mtsr:
    case Op::mfsrin:
    case Op::mtsrin:
    case Op::tlbie:
    case Op::tlbsync:
    case Op::eciwx:
    case Op::ecowx:
    case Op::mffs:
    case Op::mtfsf:
    case Op::mtfsfi:
    case Op::mtfsb0:
    case Op::mtfsb1:
    case Op::mcrfs:
        cafe_ppc_illegal(ctx, a, w, "instruction not supported in user code");
        break;
    case Op::invalid:
    case Op::count:
        cafe_ppc_illegal(ctx, a, w, "undecodable instruction");
        break;
    }

    return next;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
uint32_t execute_block_cpp(PPCContext* ctx, uint8_t* base, const Block* block) {
    const size_t count = block->count;
    if (count == 0 || block->first == nullptr) return block->start;

    const Decoded* first = block->first;
    const Decoded* tail = first + (count - 1);

    // Diagnostics are opt-in. Keep all trace bookkeeping out of the normal
    // gameplay path: the dynamic runtime executes billions of basic blocks.
    if (g_full_instruction_trace) {
        for (size_t i = 0; i + 1 < count; ++i) {
            const Decoded* ins = first + i;
            remember_instruction(ins->address, ins->word);
            (void)execute_one(*ctx, base, *ins);
        }
        remember_instruction(tail->address, tail->word);
        return execute_one(*ctx, base, *tail);
    }
    if (g_keep_recent_trace) {
        remember_instruction(tail->address, tail->word);
    }

    // Blocks reference one contiguous decoded span. Avoid a vector lookup and
    // an extra pointer indirection for every guest instruction.
    for (size_t i = 0; i + 1 < count; ++i) {
        (void)execute_one(*ctx, base, first[i]);
    }
    return execute_one(*ctx, base, *tail);
}


uint32_t execute_block_suffix(PPCContext* ctx, uint8_t* base, const Block* block, uint32_t start) {
    const size_t count = block->count;
    if (count == 0 || block->first == nullptr || start >= count) return block->start;
    const Decoded* first = block->first;
    for (size_t i = start; i + 1 < count; ++i) {
        (void)execute_one(*ctx, base, first[i]);
    }
    return execute_one(*ctx, base, first[count - 1]);
}

#ifdef _WIN32
struct X64Emitter {
    uint8_t* p;

    void b(uint8_t v) { *p++ = v; }
    void d(uint32_t v) { std::memcpy(p, &v, sizeof(v)); p += sizeof(v); }
    void q(uint64_t v) { std::memcpy(p, &v, sizeof(v)); p += sizeof(v); }

    static uint32_t r_off(uint32_t r) {
        return static_cast<uint32_t>(offsetof(PPCContext, r) + r * sizeof(uint32_t));
    }
    static uint32_t cr_off(uint32_t f, uint32_t member = 0) {
        return static_cast<uint32_t>(offsetof(PPCContext, cr) + f * sizeof(CRField) + member);
    }

    void load_eax_ctx(uint32_t off) { b(0x41); b(0x8B); b(0x83); d(off); }
    void store_eax_ctx(uint32_t off) { b(0x41); b(0x89); b(0x83); d(off); }
    void load_r9d_ctx(uint32_t off) { b(0x45); b(0x8B); b(0x8B); d(off); }
    void store_r9d_ctx(uint32_t off) { b(0x45); b(0x89); b(0x8B); d(off); }
    void load_r10d_ctx(uint32_t off) { b(0x45); b(0x8B); b(0x93); d(off); }
    void mov_eax_r10d() { b(0x44); b(0x89); b(0xD0); }
    void loadzx_eax_ctx8(uint32_t off) { b(0x41); b(0x0F); b(0xB6); b(0x83); d(off); }
    uint8_t* jcc32(uint8_t cc) {
        b(0x0F); b(cc);
        uint8_t* displacement = p;
        d(0);
        return displacement;
    }
    void patch_rel32(uint8_t* displacement, uint8_t* target) {
        const intptr_t delta = target - (displacement + 4);
        const int32_t rel = static_cast<int32_t>(delta);
        std::memcpy(displacement, &rel, sizeof(rel));
    }

    void load_eax_r(uint32_t r) { load_eax_ctx(r_off(r)); }
    void store_eax_r(uint32_t r) { store_eax_ctx(r_off(r)); }
    void load_r9d_r(uint32_t r) { load_r9d_ctx(r_off(r)); }
    void store_r9d_r(uint32_t r) { store_r9d_ctx(r_off(r)); }

    void mov_eax_imm(uint32_t v) { b(0xB8); d(v); }
    void store_imm_ctx(uint32_t off, uint32_t v) {
        b(0x41); b(0xC7); b(0x83); d(off); d(v);
    }
    void ret() { b(0xC3); }
    void add_eax_imm(uint32_t v) { b(0x05); d(v); }
    void and_eax_imm(uint32_t v) { b(0x25); d(v); }
    void or_eax_imm(uint32_t v) { b(0x0D); d(v); }
    void xor_eax_imm(uint32_t v) { b(0x35); d(v); }

    void add_eax_ctx(uint32_t off) { b(0x41); b(0x03); b(0x83); d(off); }
    void sub_eax_ctx(uint32_t off) { b(0x41); b(0x2B); b(0x83); d(off); }
    void and_eax_ctx(uint32_t off) { b(0x41); b(0x23); b(0x83); d(off); }
    void or_eax_ctx(uint32_t off) { b(0x41); b(0x0B); b(0x83); d(off); }
    void xor_eax_ctx(uint32_t off) { b(0x41); b(0x33); b(0x83); d(off); }

    void setcc_ctx(uint8_t opcode, uint32_t off) {
        b(0x41); b(0x0F); b(opcode); b(0x83); d(off);
    }
    void copy_xer_so(uint32_t cr_field) {
        b(0x41); b(0x8A); b(0x83); d(static_cast<uint32_t>(offsetof(PPCContext, xer_so)));
        b(0x41); b(0x88); b(0x83); d(cr_off(cr_field, 3));
    }
    void record_eax() {
        b(0x85); b(0xC0); // test eax,eax
        setcc_ctx(0x9C, cr_off(0, 0)); // setl
        setcc_ctx(0x9F, cr_off(0, 1)); // setg
        setcc_ctx(0x94, cr_off(0, 2)); // sete
        copy_xer_so(0);
    }
    void compare_eax(bool is_signed, uint32_t cr_field) {
        setcc_ctx(is_signed ? 0x9C : 0x92, cr_off(cr_field, 0));
        setcc_ctx(is_signed ? 0x9F : 0x97, cr_off(cr_field, 1));
        setcc_ctx(0x94, cr_off(cr_field, 2));
        copy_xer_so(cr_field);
    }

    void ea(const Decoded& ins, bool indexed) {
        if (indexed) {
            if (ins.ra == 0) load_eax_r(ins.rb);
            else {
                load_eax_r(ins.ra);
                add_eax_ctx(r_off(ins.rb));
            }
        } else {
            if (ins.ra == 0) mov_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)));
            else {
                load_eax_r(ins.ra);
                add_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)));
            }
        }
    }

    void guest_load_r9d(int width, bool sign_extend, bool byte_reverse) {
        if (width == 1) {
            b(0x44); b(0x0F); b(0xB6); b(0x0C); b(0x02);
        } else if (width == 2) {
            b(0x44); b(0x0F); b(0xB7); b(0x0C); b(0x02);
            if (!byte_reverse) { b(0x66); b(0x41); b(0xC1); b(0xC9); b(0x08); }
            if (sign_extend) { b(0x45); b(0x0F); b(0xBF); b(0xC9); }
        } else {
            b(0x44); b(0x8B); b(0x0C); b(0x02);
            if (!byte_reverse) { b(0x41); b(0x0F); b(0xC9); }
        }
    }
    void guest_store_r9d(int width, bool byte_reverse) {
        if (width == 1) {
            b(0x44); b(0x88); b(0x0C); b(0x02);
        } else if (width == 2) {
            if (!byte_reverse) { b(0x66); b(0x41); b(0xC1); b(0xC9); b(0x08); }
            b(0x66); b(0x44); b(0x89); b(0x0C); b(0x02);
        } else {
            if (!byte_reverse) { b(0x41); b(0x0F); b(0xC9); }
            b(0x44); b(0x89); b(0x0C); b(0x02);
        }
    }

    bool load_integer_jit(const Decoded& ins, int width, bool sign_extend,
                          bool update, bool indexed, bool byte_reverse = false) {
        ea(ins, indexed);
        guest_load_r9d(width, sign_extend, byte_reverse);
        store_r9d_r(ins.d);
        if (update) store_eax_r(ins.ra);
        return true;
    }
    bool store_integer_jit(const Decoded& ins, int width, bool update,
                           bool indexed, bool byte_reverse = false) {
        ea(ins, indexed);
        load_r9d_r(ins.d);
        guest_store_r9d(width, byte_reverse);
        if (update) store_eax_r(ins.ra);
        return true;
    }

    bool emit(const Decoded& ins) {
        const uint32_t w = ins.word;
        const bool rec = ppc::rc(w);
        const bool oe = ppc::oe(w);
        switch (ins.op) {
        case Op::addi:
            if (ins.ra) { load_eax_r(ins.ra); add_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm))); }
            else mov_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)));
            store_eax_r(ins.d);
            return true;
        case Op::addis:
            if (ins.ra) { load_eax_r(ins.ra); add_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)) << 16); }
            else mov_eax_imm(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)) << 16);
            store_eax_r(ins.d);
            return true;
        case Op::mulli:
            b(0x41); b(0x69); b(0x83); d(r_off(ins.ra));
            d(static_cast<uint32_t>(static_cast<int32_t>(ins.simm)));
            store_eax_r(ins.d);
            return true;
        case Op::add:
            if (oe) return false;
            load_eax_r(ins.ra); add_eax_ctx(r_off(ins.rb)); store_eax_r(ins.d);
            if (rec) record_eax();
            return true;
        case Op::subf:
            if (oe) return false;
            load_eax_r(ins.rb); sub_eax_ctx(r_off(ins.ra)); store_eax_r(ins.d);
            if (rec) record_eax();
            return true;
        case Op::neg:
            if (oe) return false;
            load_eax_r(ins.ra); b(0xF7); b(0xD8); store_eax_r(ins.d);
            if (rec) record_eax();
            return true;
        case Op::mullw:
            if (oe) return false;
            load_eax_r(ins.ra);
            b(0x41); b(0x0F); b(0xAF); b(0x83); d(r_off(ins.rb));
            store_eax_r(ins.d);
            if (rec) record_eax();
            return true;

        case Op::cmp:
        case Op::cmpl:
            load_eax_r(ins.ra);
            b(0x41); b(0x3B); b(0x83); d(r_off(ins.rb));
            compare_eax(ins.op == Op::cmp, ppc::crfd(w));
            return true;
        case Op::cmpi:
        case Op::cmpli:
            load_eax_r(ins.ra); b(0x3D);
            d(ins.op == Op::cmpi ? static_cast<uint32_t>(static_cast<int32_t>(ins.simm)) : ins.uimm);
            compare_eax(ins.op == Op::cmpi, ppc::crfd(w));
            return true;

        case Op::and_:
            load_eax_r(ins.d); and_eax_ctx(r_off(ins.rb)); store_eax_r(ins.ra);
            if (rec) record_eax(); return true;
        case Op::or_:
            load_eax_r(ins.d); or_eax_ctx(r_off(ins.rb)); store_eax_r(ins.ra);
            if (rec) record_eax(); return true;
        case Op::xor_:
            load_eax_r(ins.d); xor_eax_ctx(r_off(ins.rb)); store_eax_r(ins.ra);
            if (rec) record_eax(); return true;
        case Op::andc:
        case Op::orc:
            load_eax_r(ins.d); load_r9d_r(ins.rb);
            b(0x41); b(0xF7); b(0xD1); // not r9d
            b(0x44); b(ins.op == Op::andc ? 0x21 : 0x09); b(0xC8);
            store_eax_r(ins.ra); if (rec) record_eax(); return true;
        case Op::nand:
        case Op::nor:
        case Op::eqv:
            load_eax_r(ins.d);
            if (ins.op == Op::nand) and_eax_ctx(r_off(ins.rb));
            else if (ins.op == Op::nor) or_eax_ctx(r_off(ins.rb));
            else xor_eax_ctx(r_off(ins.rb));
            b(0xF7); b(0xD0); // not eax
            store_eax_r(ins.ra); if (rec) record_eax(); return true;
        case Op::andi_rc:
            load_eax_r(ins.d); and_eax_imm(ins.uimm); store_eax_r(ins.ra); record_eax(); return true;
        case Op::andis_rc:
            load_eax_r(ins.d); and_eax_imm(ins.uimm << 16); store_eax_r(ins.ra); record_eax(); return true;
        case Op::ori:
            load_eax_r(ins.d); or_eax_imm(ins.uimm); store_eax_r(ins.ra); return true;
        case Op::oris:
            load_eax_r(ins.d); or_eax_imm(ins.uimm << 16); store_eax_r(ins.ra); return true;
        case Op::xori:
            load_eax_r(ins.d); xor_eax_imm(ins.uimm); store_eax_r(ins.ra); return true;
        case Op::xoris:
            load_eax_r(ins.d); xor_eax_imm(ins.uimm << 16); store_eax_r(ins.ra); return true;
        case Op::extsb:
            load_eax_r(ins.d); b(0x0F); b(0xBE); b(0xC0); store_eax_r(ins.ra);
            if (rec) record_eax(); return true;
        case Op::extsh:
            load_eax_r(ins.d); b(0x0F); b(0xBF); b(0xC0); store_eax_r(ins.ra);
            if (rec) record_eax(); return true;

        case Op::rlwinm: {
            load_eax_r(ins.d);
            b(0xC1); b(0xC0); b(static_cast<uint8_t>(ins.rb & 31));
            and_eax_imm(rotate_mask(ppc::mb(w), ppc::me(w)));
            store_eax_r(ins.ra); if (rec) record_eax(); return true;
        }
        case Op::rlwimi: {
            const uint32_t mask = rotate_mask(ppc::mb(w), ppc::me(w));
            load_eax_r(ins.d); load_r9d_r(ins.ra);
            b(0xC1); b(0xC0); b(static_cast<uint8_t>(ins.rb & 31));
            and_eax_imm(mask);
            b(0x41); b(0x81); b(0xE1); d(~mask); // and r9d,~mask
            b(0x44); b(0x09); b(0xC8);           // or eax,r9d
            store_eax_r(ins.ra); if (rec) record_eax(); return true;
        }

        case Op::lbz: return load_integer_jit(ins, 1, false, false, false);
        case Op::lbzu: return load_integer_jit(ins, 1, false, true, false);
        case Op::lbzx: return load_integer_jit(ins, 1, false, false, true);
        case Op::lbzux: return load_integer_jit(ins, 1, false, true, true);
        case Op::lhz: return load_integer_jit(ins, 2, false, false, false);
        case Op::lhzu: return load_integer_jit(ins, 2, false, true, false);
        case Op::lhzx: return load_integer_jit(ins, 2, false, false, true);
        case Op::lhzux: return load_integer_jit(ins, 2, false, true, true);
        case Op::lha: return load_integer_jit(ins, 2, true, false, false);
        case Op::lhau: return load_integer_jit(ins, 2, true, true, false);
        case Op::lhax: return load_integer_jit(ins, 2, true, false, true);
        case Op::lhaux: return load_integer_jit(ins, 2, true, true, true);
        case Op::lwz: return load_integer_jit(ins, 4, false, false, false);
        case Op::lwzu: return load_integer_jit(ins, 4, false, true, false);
        case Op::lwzx: return load_integer_jit(ins, 4, false, false, true);
        case Op::lwzux: return load_integer_jit(ins, 4, false, true, true);
        case Op::lhbrx: return load_integer_jit(ins, 2, false, false, true, true);
        case Op::lwbrx: return load_integer_jit(ins, 4, false, false, true, true);

        case Op::stb: return store_integer_jit(ins, 1, false, false);
        case Op::stbu: return store_integer_jit(ins, 1, true, false);
        case Op::stbx: return store_integer_jit(ins, 1, false, true);
        case Op::stbux: return store_integer_jit(ins, 1, true, true);
        case Op::sth: return store_integer_jit(ins, 2, false, false);
        case Op::sthu: return store_integer_jit(ins, 2, true, false);
        case Op::sthx: return store_integer_jit(ins, 2, false, true);
        case Op::sthux: return store_integer_jit(ins, 2, true, true);
        case Op::stw: return store_integer_jit(ins, 4, false, false);
        case Op::stwu: return store_integer_jit(ins, 4, true, false);
        case Op::stwx: return store_integer_jit(ins, 4, false, true);
        case Op::stwux: return store_integer_jit(ins, 4, true, true);
        case Op::sthbrx: return store_integer_jit(ins, 2, false, true, true);
        case Op::stwbrx: return store_integer_jit(ins, 4, false, true, true);

        case Op::mfspr: {
            const uint32_t spr = ppc::spr(w);
            if (spr == 8) load_eax_ctx(static_cast<uint32_t>(offsetof(PPCContext, lr)));
            else if (spr == 9) load_eax_ctx(static_cast<uint32_t>(offsetof(PPCContext, ctr)));
            else return false;
            store_eax_r(ins.d); return true;
        }
        case Op::mtspr: {
            const uint32_t spr = ppc::spr(w);
            if (spr != 8 && spr != 9) return false;
            load_eax_r(ins.d);
            store_eax_ctx(static_cast<uint32_t>(spr == 8 ? offsetof(PPCContext, lr) : offsetof(PPCContext, ctr)));
            return true;
        }

        case Op::dcbf:
        case Op::dcbi:
        case Op::dcbst:
        case Op::dcbt:
        case Op::dcbtst:
        case Op::icbi:
        case Op::isync:
        case Op::eieio:
            return true;
        case Op::sync:
            b(0x0F); b(0xAE); b(0xF0); // mfence
            return true;
        default:
            return false;
        }
    }

    bool tail(const Decoded& ins) {
        const uint32_t lr_off = static_cast<uint32_t>(offsetof(PPCContext, lr));
        const uint32_t ctr_off = static_cast<uint32_t>(offsetof(PPCContext, ctr));
        const bool link = ppc::lk(ins.word);
        if (ins.op == Op::b) {
            if (link) store_imm_ctx(lr_off, ins.address + 4);
            mov_eax_imm(ins.direct_target);
            ret();
            return true;
        }
        if (ins.op != Op::bc && ins.op != Op::bclr && ins.op != Op::bcctr) return false;

        const uint32_t bo = ins.d;
        const uint32_t bi = ins.ra;
        if (ins.op == Op::bcctr && !(bo & 4)) return false; // architecturally illegal

        // bclr with LK branches to the old LR, so preserve it before updating
        // LR. Conditional bc/bcctr have immediate/CTR targets and need no save.
        if (ins.op == Op::bclr) load_r10d_ctx(lr_off);
        if (link) store_imm_ctx(lr_off, ins.address + 4);

        uint8_t* false_jumps[2]{};
        uint32_t false_count = 0;

        if (!(bo & 4)) {
            load_eax_ctx(ctr_off);
            b(0x83); b(0xE8); b(0x01); // sub eax,1
            store_eax_ctx(ctr_off);
            b(0x85); b(0xC0);          // test eax,eax
            // BO[3] selects CTR==0 vs CTR!=0.
            false_jumps[false_count++] = jcc32((bo & 2) ? 0x85 : 0x84); // jne / je
        }

        if (!(bo & 0x10)) {
            const uint32_t bit_off = cr_off((bi >> 2) & 7u, bi & 3u);
            loadzx_eax_ctx8(bit_off);
            b(0x85); b(0xC0); // test eax,eax
            // BO[1] selects CR[BI]==1 vs ==0.
            false_jumps[false_count++] = jcc32((bo & 8) ? 0x84 : 0x85); // je / jne
        }

        if (ins.op == Op::bc) {
            mov_eax_imm(ins.direct_target);
        } else if (ins.op == Op::bclr) {
            mov_eax_r10d();
        } else {
            load_eax_ctx(ctr_off);
        }
        ret();

        if (false_count != 0) {
            uint8_t* not_taken = p;
            mov_eax_imm(ins.address + 4);
            ret();
            for (uint32_t i = 0; i < false_count; ++i) patch_rel32(false_jumps[i], not_taken);
        }
        return true;
    }

    void suffix(Block* block, uint32_t start) {
        // Restore first argument even if future fast ops use ECX/CL.
        b(0x4C); b(0x89); b(0xD9); // mov rcx,r11
        b(0x49); b(0xB8); q(reinterpret_cast<uintptr_t>(block)); // r8
        b(0x41); b(0xB9); d(start); // r9d
        b(0x48); b(0xB8); q(reinterpret_cast<uintptr_t>(&execute_block_suffix));
        b(0x48); b(0x83); b(0xEC); b(0x28);
        b(0xFF); b(0xD0);
        b(0x48); b(0x83); b(0xC4); b(0x28);
        b(0xC3);
    }
};

BlockFn emit_block_thunk(Block* block) {
    if (!g_jit_enabled || !g_jit_arena || g_keep_recent_trace ||
        block == nullptr || block->first == nullptr || block->count == 0) {
        return nullptr;
    }

    g_jit_offset = (g_jit_offset + 15) & ~size_t{15};
    constexpr size_t kMaxBlockCode = 4096;
    if (g_jit_offset + kMaxBlockCode > kJitArenaSize) return nullptr;

    uint8_t* code = g_jit_arena + g_jit_offset;
    X64Emitter e{code};
    e.b(0x49); e.b(0x89); e.b(0xCB); // mov r11,rcx

    size_t direct = 0;
    const size_t tail = block->count - 1;
    size_t suffix_start = tail;
    for (size_t i = 0; i < tail; ++i) {
        if (!e.emit(block->first[i])) {
            suffix_start = i;
            break;
        }
        ++direct;
    }
    bool native_tail = false;
    if (suffix_start == tail) {
        native_tail = e.tail(block->first[tail]);
    }
    if (!native_tail) {
        // A helper call costs more than interpreting one trivial instruction.
        if (direct < 2) return nullptr;
        e.suffix(block, static_cast<uint32_t>(suffix_start));
    }

    const size_t bytes = static_cast<size_t>(e.p - code);
    if (bytes > kMaxBlockCode) return nullptr;

    g_jit_offset += bytes;
    ++g_jit_blocks;
    g_jit_direct_instructions += direct;
    if (native_tail) ++g_jit_native_tails;
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(bytes));
    return reinterpret_cast<BlockFn>(code);
}
#endif

void refresh_hot_block_cache_epoch() {
    const uint64_t epoch = g_hot_block_epoch.load(std::memory_order_relaxed);
    if (t_hot_block_epoch != epoch) {
        t_hot_blocks.fill({});
        t_hot_block_epoch = epoch;
    }
}

Block* get_or_build_block_slow(uint32_t start) {
    {
        std::shared_lock lock(g_block_mutex);
        const auto found = g_blocks.find(start);
        if (found != g_blocks.end()) {
            return found->second.get();
        }
    }

    auto block = std::make_unique<Block>();
    block->start = start;

    // Locate the containing code section once. Decoded instructions inside a
    // section are contiguous, so a block only needs first+count.
    const CodeSection* owner = nullptr;
    size_t first_index = 0;
    for (const auto& section : g_code) {
        if (start >= section.begin && start < section.end &&
            ((start - section.begin) & 3u) == 0) {
            owner = &section;
            first_index = (start - section.begin) / 4;
            break;
        }
    }
    if (owner == nullptr || first_index >= owner->code.size()) return nullptr;

    block->first = owner->code.data() + first_index;
    const size_t available = owner->code.size() - first_index;
    const size_t limit = std::min(kMaxBlockInstructions, available);
    for (size_t count = 0; count < limit; ++count) {
        const Decoded* ins = block->first + count;
        // Make the native presentation hook an explicit block boundary so
        // normal execution always passes through run()'s hook interception.
        if (ins->address == kPresentationHook && count != 0) break;
        ++block->count;
        if (ends_block(ins->op)) break;
    }
    if (block->count == 0) return nullptr;

    std::lock_guard lock(g_block_mutex);
    if (const auto found = g_blocks.find(start); found != g_blocks.end()) {
        return found->second.get();
    }
    Block* raw = block.get();
    g_blocks.emplace(start, std::move(block));
    remember_block_locked(start);
#ifdef _WIN32
    raw->native = emit_block_thunk(raw);
#endif
    return raw;
}

#if defined(__clang__)
__attribute__((always_inline)) inline
#endif
Block* get_or_build_block(uint32_t start) {
    HotBlockEntry& hot = t_hot_blocks[(start >> 2) & (kHotBlockCacheSize - 1)];
    if (hot.block != nullptr && hot.address == start) {
        return hot.block;
    }
    Block* block = get_or_build_block_slow(start);
    if (block != nullptr) {
        hot = {start, block};
    }
    return block;
}

} // namespace

void initialize(const std::vector<uint8_t>& rpx_bytes) {
    const auto started = std::chrono::steady_clock::now();
    rpx::Image image = rpx::load(rpx_bytes);

    std::lock_guard lock(g_mutex);
    g_code.clear();
    g_rel24_targets.clear();
    g_imports.clear();
    g_import_min = UINT32_MAX;
    g_import_max = 0;
    {
        std::lock_guard block_lock(g_block_mutex);
        g_blocks.clear();
        g_known_block_starts.clear();
        g_pending_block_starts.clear();
        g_block_cache_path.clear();
        g_blocks.reserve(262144);
        g_known_block_starts.reserve(262144);
        g_pending_block_starts.reserve(kBlockCacheFlushBatch);
#ifdef _WIN32
        reset_jit_arena();
#endif
    }

    g_hot_block_epoch.fetch_add(1, std::memory_order_relaxed);
    const char* trace_option = std::getenv("TTT2_TRACE_GUEST");
    g_full_instruction_trace = trace_option && *trace_option == '1';
    const char* boot_trace_option = std::getenv("TTT2_TRACE_DYNAMIC");
    g_dynamic_boot_trace = boot_trace_option && *boot_trace_option == '1';
    g_keep_recent_trace = g_full_instruction_trace || g_dynamic_boot_trace;
#ifdef _WIN32
    if (g_keep_recent_trace) g_jit_enabled = false;
#endif

    g_rel24_targets.insert(image.rel24_targets.begin(), image.rel24_targets.end());

    size_t instruction_count = 0;
    for (const auto& section : image.sections) {
        if (!section.allocated() || !section.executable() || section.data.empty() ||
            section.address == 0 || section.data.size() < 4) {
            continue;
        }
        CodeSection decoded;
        decoded.begin = section.address;
        decoded.end = section.address + static_cast<uint32_t>(section.data.size() & ~size_t{3});
        const size_t words = (decoded.end - decoded.begin) / 4;
        decoded.code.reserve(words);
        for (size_t i = 0; i < words; ++i) {
            const uint32_t address = decoded.begin + static_cast<uint32_t>(i * 4);
            const uint32_t word = rpx::read_be32(section.data.data() + i * 4);
            decoded.code.push_back(decode_instruction(address, word));
        }
        instruction_count += words;
        g_code.push_back(std::move(decoded));
    }

    for (const auto& import : image.imports) {
        ImportBinding binding;
        binding.module = import.module;
        binding.name = import.name;
        binding.is_data = import.is_data;
        binding.data_address = import.address;
        if (!import.is_data) {
            if (const Export* exported = find_export(import.module, import.name)) {
                binding.function = exported->function;
            }
        }
        g_import_min = std::min(g_import_min, import.stub_address);
        g_import_max = std::max(g_import_max, import.stub_address);
        g_imports.emplace(import.stub_address, std::move(binding));
    }

    const std::vector<uint32_t> warm_blocks = prepare_persistent_block_cache(rpx_bytes);
    refresh_hot_block_cache_epoch();
    size_t warmed = 0;
    for (const uint32_t address : warm_blocks) {
        if (get_or_build_block(address)) ++warmed;
    }

    g_enabled.store(true, std::memory_order_release);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    std::fprintf(stderr,
                 "ttt2: dynamic host: decoded %zu PPC instructions across %zu code sections in %.2f ms\n",
                 instruction_count, g_code.size(), ms);
#ifdef _WIN32
    std::fprintf(stderr,
                 "ttt2: dynamic host: x64 tier-1 JIT %s (%zu warm blocks, %zu direct PPC instructions, %zu native tails)\n",
                 g_jit_enabled && g_jit_arena ? "enabled" : "disabled",
                 g_jit_blocks, g_jit_direct_instructions, g_jit_native_tails);
    {
        char jit_info[192]{};
        std::snprintf(jit_info, sizeof(jit_info),
                      "DYNAMIC jit=%s warm_blocks=%zu direct_instructions=%zu native_tails=%zu",
                      g_jit_enabled && g_jit_arena ? "enabled" : "disabled",
                      g_jit_blocks, g_jit_direct_instructions, g_jit_native_tails);
        cafe::diagnostic_trace(jit_info);
    }
#endif
    std::fprintf(stderr, "ttt2: dynamic host: warmed %zu cached code blocks\n", warmed);
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void initialize_data_imports() {
    std::lock_guard lock(g_mutex);
    for (const auto& [stub, binding] : g_imports) {
        (void)stub;
        if (!binding.is_data) continue;
        if (const DataExport* data = find_data_export(binding.module, binding.name)) {
            data->initialize(binding.data_address);
        } else {
            std::fprintf(stderr, "ttt2: warning: data import %s:%s is not provided\n",
                         binding.module.c_str(), binding.name.c_str());
        }
    }
}

void run(PPCContext& ctx, uint8_t* base, uint32_t entry, uint32_t stop,
         bool suppress_presentation_hook) {
    uint32_t pc = entry;
    refresh_hot_block_cache_epoch();
    if (g_dynamic_boot_trace) {
        ++t_run_calls;
        if (t_run_calls <= 32) {
            trace_dynamic_state("run-enter", pc, stop, ctx);
        }
    }

    // Game code for this title lives below the synthetic host-thunk window.
    // Stay on a tight block-to-block path while PC remains normal guest code;
    // only return to the slow dispatcher for imports, host thunks, the native
    // presentation hook or the requested stop address.
    constexpr uint32_t kFastGuestLimit = layout::kHostThunkBase;

    for (;;) {
        while (pc != stop && pc < kFastGuestLimit &&
               (suppress_presentation_hook || pc != kPresentationHook)) {
            Block* block = get_or_build_block(pc);
            if (!block) {
                fatal("dynamic guest execution reached non-code address 0x%08X", pc);
            }

            if (g_dynamic_boot_trace) {
                ++t_blocks_executed;
                t_instructions_executed += block->count;
                if (t_blocks_executed <= 96) {
                    char marker[224]{};
                    std::snprintf(marker, sizeof(marker),
                                  "dynamic block #%llu pc=0x%08X size=%zu lr=0x%08X sp=0x%08X",
                                  static_cast<unsigned long long>(t_blocks_executed), pc,
                                  static_cast<size_t>(block->count), ctx.lr, ctx.r[1]);
                    diagnostic_trace(marker);
                }
                if (t_blocks_executed == 1 || (t_blocks_executed & 0x3FFFu) == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (t_blocks_executed == 1 ||
                        now - t_last_progress >= std::chrono::seconds(2)) {
                        trace_dynamic_state("progress", pc, stop, ctx);
                        t_last_progress = now;
                    }
                }
            }

#ifdef _WIN32
            if (block->native) {
                pc = block->native(&ctx, base);
            } else
#endif
            {
                pc = execute_block_cpp(&ctx, base, block);
            }
        }

        if (pc == stop) break;

        // The static port replaces this function to observe the system-info
        // object after the original guest body returns. Reproduce the same
        // hook without requiring any generated C++ symbol.
        if (!suppress_presentation_hook && pc == kPresentationHook) {
            const uint32_t out = ctx.r[3];
            const uint32_t return_address = ctx.lr;
            run(ctx, base, pc, return_address, true);
            title::system_info_created(ppc::ld32(base, out));
            pc = return_address;
            continue;
        }

        if (pc >= g_import_min && pc <= g_import_max) {
            if (auto it = g_imports.find(pc); it != g_imports.end() && !it->second.is_data) {
                if (g_dynamic_boot_trace) {
                    ++t_import_calls;
                    if (t_import_calls <= 64) {
                        char marker[256]{};
                        std::snprintf(marker, sizeof(marker),
                                      "dynamic import #%llu pc=0x%08X %s:%s lr=0x%08X",
                                      static_cast<unsigned long long>(t_import_calls), pc,
                                      it->second.module.c_str(), it->second.name.c_str(), ctx.lr);
                        diagnostic_trace(marker);
                    }
                }
                if (!it->second.function) {
                    fatal("unimplemented dynamic import %s:%s at 0x%08X",
                          it->second.module.c_str(), it->second.name.c_str(), pc);
                }
                it->second.function(ctx, base);
                pc = ctx.lr;
                continue;
            }
        }

        if (PPCFunc* host = find_registered_host_function(pc)) {
            if (g_dynamic_boot_trace) {
                ++t_host_calls;
                if (t_host_calls <= 32) {
                    char marker[224]{};
                    const char* name = host_function_name(pc);
                    std::snprintf(marker, sizeof(marker),
                                  "dynamic host #%llu pc=0x%08X name=%s lr=0x%08X",
                                  static_cast<unsigned long long>(t_host_calls), pc,
                                  name ? name : "(unknown)", ctx.lr);
                    diagnostic_trace(marker);
                }
            }
            host(ctx, base);
            pc = ctx.lr;
            continue;
        }

        fatal("dynamic guest execution reached unsupported dispatcher address 0x%08X", pc);
    }

    if (g_dynamic_boot_trace && t_run_calls <= 32) {
        trace_dynamic_state("run-exit", pc, stop, ctx);
    }
}

void execute(PPCContext& ctx, uint8_t* base, uint32_t entry) {
    const uint32_t return_address = ctx.lr;
    run(ctx, base, entry, return_address, false);
}

uint32_t execute_test_instruction(PPCContext& ctx, uint8_t* base,
                                  uint32_t address, uint32_t word) {
    const Decoded instruction = decode_instruction(address, word);
    return execute_one(ctx, base, instruction);
}

void initialize_test_program(uint32_t base_address,
                             const std::vector<uint32_t>& words) {
    std::lock_guard lock(g_mutex);
    g_code.clear();
    g_rel24_targets.clear();
    g_imports.clear();
    {
        std::lock_guard block_lock(g_block_mutex);
        g_blocks.clear();
        g_known_block_starts.clear();
        g_pending_block_starts.clear();
        g_block_cache_path.clear();
        g_blocks.reserve(262144);
        g_known_block_starts.reserve(262144);
        g_pending_block_starts.reserve(kBlockCacheFlushBatch);
#ifdef _WIN32
        reset_jit_arena();
#endif
    }

    CodeSection section;
    section.begin = base_address;
    section.end = base_address + static_cast<uint32_t>(words.size() * 4);
    section.code.reserve(words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t address = base_address + static_cast<uint32_t>(i * 4);
        section.code.push_back(decode_instruction(address, words[i]));
    }
    g_code.push_back(std::move(section));
    g_hot_block_epoch.fetch_add(1, std::memory_order_relaxed);
    g_enabled.store(true, std::memory_order_release);
}

void print_recent_trace() {
    if (t_trace_count == 0) return;
    std::fprintf(stderr, "recent guest instructions (oldest -> newest):\n");
    const size_t first = (t_trace_cursor + kTraceEntries - t_trace_count) % kTraceEntries;
    for (size_t i = 0; i < t_trace_count; ++i) {
        const TraceEntry& e = t_trace[(first + i) % kTraceEntries];
        const Op op = ppc::decode(e.word);
        std::fprintf(stderr, "  0x%08X  %08X  %s\n",
                     e.address, e.word, ppc::op_name(op));
    }
}

} // namespace cafe::dynamic
