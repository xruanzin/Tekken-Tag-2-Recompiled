#include "cafe/dynamic.h"

#include "cafe/export.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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
thread_local auto t_last_progress = std::chrono::steady_clock::now();
bool g_full_instruction_trace = false;

void remember_instruction(uint32_t address, uint32_t word) {
    t_trace[t_trace_cursor] = {address, word};
    t_trace_cursor = (t_trace_cursor + 1) % kTraceEntries;
    t_trace_count = std::min(t_trace_count + 1, kTraceEntries);
}

struct Decoded {
    uint32_t address{};
    uint32_t word{};
    Op op{Op::invalid};
};

using BlockFn = uint32_t (*)(PPCContext*, uint8_t*);

struct Block {
    uint32_t start{};
    std::vector<const Decoded*> instructions;
    BlockFn native{};
};

constexpr uint32_t kPresentationHook = 0x0864107Cu;
constexpr size_t kMaxBlockInstructions = 64;

std::unordered_map<uint32_t, std::unique_ptr<Block>> g_blocks;
std::mutex g_block_mutex;

// Hot guest loops revisit the same basic blocks constantly. Avoid taking the
// global block mutex and hashing into g_blocks on every iteration by keeping a
// small direct-mapped cache per host thread. The epoch invalidates pointers
// whenever a new program is initialized.
struct HotBlockEntry {
    uint32_t address{};
    Block* block{};
};
constexpr size_t kHotBlockCacheSize = 4096;
thread_local std::array<HotBlockEntry, kHotBlockCacheSize> t_hot_blocks{};
thread_local uint64_t t_hot_block_epoch = 0;
std::atomic<uint64_t> g_hot_block_epoch{1};

std::filesystem::path g_block_cache_path;
std::unordered_set<uint32_t> g_known_block_starts;
std::vector<uint32_t> g_pending_block_starts;
bool g_cache_exit_registered = false;
constexpr size_t kBlockCacheFlushBatch = 128;

#ifdef _WIN32
constexpr size_t kJitArenaSize = 8u << 20;
uint8_t* g_jit_arena = nullptr;
size_t g_jit_offset = 0;
bool g_jit_enabled = true;
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
bool g_enabled = false;
std::vector<CodeSection> g_code;
std::unordered_map<uint32_t, uint32_t> g_rel24_targets;
std::unordered_map<uint32_t, ImportBinding> g_imports;

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
    const char* option = std::getenv("TTT2_DYNAMIC_TRAMPOLINES");
    g_jit_enabled = option && *option == '1';
    if (!g_jit_enabled) return;

    // These are dispatch trampolines, not translated PPC code. They are kept
    // only as an opt-in comparison path; direct calls to execute_block_cpp are
    // faster on the Windows Player because they avoid an extra indirect jump.
    g_jit_arena = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kJitArenaSize, MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE));
    if (!g_jit_arena) {
        g_jit_enabled = false;
        std::fprintf(stderr, "ttt2: dynamic host: x64 baseline JIT arena unavailable; using C++ blocks\n");
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

uint32_t direct_branch_target(uint32_t address, uint32_t word) {
    const auto relocated = g_rel24_targets.find(address);
    return relocated != g_rel24_targets.end() ? relocated->second
                                              : ppc::branch_target(word, address);
}

const Decoded* decoded_at(uint32_t address) {
    for (const auto& section : g_code) {
        if (address >= section.begin && address < section.end &&
            ((address - section.begin) & 3u) == 0) {
            const size_t index = (address - section.begin) / 4;
            if (index < section.code.size()) return &section.code[index];
        }
    }
    return nullptr;
}

void load_integer(PPCContext& ctx, uint8_t* base, uint32_t w, int width,
                  bool sign_extend, bool update, bool indexed, bool byte_reverse = false) {
    const uint32_t d = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t ea = indexed ? ea_x(ctx, ra, ppc::rb(w))
                                : ea_d(ctx, ra, ppc::simm(w));
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

void store_integer(PPCContext& ctx, uint8_t* base, uint32_t w, int width,
                   bool update, bool indexed, bool byte_reverse = false) {
    const uint32_t s = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t ea = indexed ? ea_x(ctx, ra, ppc::rb(w))
                                : ea_d(ctx, ra, ppc::simm(w));
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

void fload(PPCContext& ctx, uint8_t* base, uint32_t w, bool single,
           bool update, bool indexed) {
    const uint32_t d = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t ea = indexed ? ea_x(ctx, ra, ppc::rb(w))
                                : ea_d(ctx, ra, ppc::simm(w));
    if (single) {
        const double value = ppc::load_single(ppc::ld32(base, ea));
        ctx.f[d].ps0 = value;
        ctx.f[d].ps1 = value;
    } else {
        ctx.f[d].ps0 = ppc::from_bits(ppc::ld64(base, ea));
    }
    if (update) ctx.r[ra] = ea;
}

void fstore(PPCContext& ctx, uint8_t* base, uint32_t w, bool single,
            bool update, bool indexed, bool integer_word = false) {
    const uint32_t s = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t ea = indexed ? ea_x(ctx, ra, ppc::rb(w))
                                : ea_d(ctx, ra, ppc::simm(w));
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

uint32_t execute_one(PPCContext& ctx, uint8_t* base, const Decoded& ins) {
    const uint32_t a = ins.address;
    const uint32_t w = ins.word;
    const Op op = ins.op;
    if (g_full_instruction_trace || ends_block(op)) {
        remember_instruction(a, w);
    }
    const uint32_t d = ppc::rd(w);
    const uint32_t ra = ppc::ra(w);
    const uint32_t rb = ppc::rb(w);
    const uint32_t rc = ppc::frc(w);
    const int32_t simm = ppc::simm(w);
    const uint32_t uimm = ppc::uimm(w);
    uint32_t next = a + 4;

    if (ppc::has_record_form(op) && ppc::rc(w) && op >= Op::fadd) {
        cafe_ppc_illegal(ctx, a, w, "floating-point record form");
    }

    switch (op) {
    case Op::addi:
        ctx.r[d] = (ra ? ctx.r[ra] : 0u) + static_cast<uint32_t>(simm);
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

    case Op::lbz: load_integer(ctx, base, w, 1, false, false, false); break;
    case Op::lbzu: load_integer(ctx, base, w, 1, false, true, false); break;
    case Op::lbzx: load_integer(ctx, base, w, 1, false, false, true); break;
    case Op::lbzux: load_integer(ctx, base, w, 1, false, true, true); break;
    case Op::lhz: load_integer(ctx, base, w, 2, false, false, false); break;
    case Op::lhzu: load_integer(ctx, base, w, 2, false, true, false); break;
    case Op::lhzx: load_integer(ctx, base, w, 2, false, false, true); break;
    case Op::lhzux: load_integer(ctx, base, w, 2, false, true, true); break;
    case Op::lha: load_integer(ctx, base, w, 2, true, false, false); break;
    case Op::lhau: load_integer(ctx, base, w, 2, true, true, false); break;
    case Op::lhax: load_integer(ctx, base, w, 2, true, false, true); break;
    case Op::lhaux: load_integer(ctx, base, w, 2, true, true, true); break;
    case Op::lwz: load_integer(ctx, base, w, 4, false, false, false); break;
    case Op::lwzu: load_integer(ctx, base, w, 4, false, true, false); break;
    case Op::lwzx: load_integer(ctx, base, w, 4, false, false, true); break;
    case Op::lwzux: load_integer(ctx, base, w, 4, false, true, true); break;
    case Op::lhbrx: load_integer(ctx, base, w, 2, false, false, true, true); break;
    case Op::lwbrx: load_integer(ctx, base, w, 4, false, false, true, true); break;

    case Op::stb: store_integer(ctx, base, w, 1, false, false); break;
    case Op::stbu: store_integer(ctx, base, w, 1, true, false); break;
    case Op::stbx: store_integer(ctx, base, w, 1, false, true); break;
    case Op::stbux: store_integer(ctx, base, w, 1, true, true); break;
    case Op::sth: store_integer(ctx, base, w, 2, false, false); break;
    case Op::sthu: store_integer(ctx, base, w, 2, true, false); break;
    case Op::sthx: store_integer(ctx, base, w, 2, false, true); break;
    case Op::sthux: store_integer(ctx, base, w, 2, true, true); break;
    case Op::stw: store_integer(ctx, base, w, 4, false, false); break;
    case Op::stwu: store_integer(ctx, base, w, 4, true, false); break;
    case Op::stwx: store_integer(ctx, base, w, 4, false, true); break;
    case Op::stwux: store_integer(ctx, base, w, 4, true, true); break;
    case Op::sthbrx: store_integer(ctx, base, w, 2, false, true, true); break;
    case Op::stwbrx: store_integer(ctx, base, w, 4, false, true, true); break;

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
        next = direct_branch_target(a, w);
        break;
    case Op::bc: {
        if (ppc::lk(w)) ctx.lr = a + 4;
        if (branch_condition(ctx, d, ra)) next = direct_branch_target(a, w);
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

    case Op::lfs: fload(ctx, base, w, true, false, false); break;
    case Op::lfsu: fload(ctx, base, w, true, true, false); break;
    case Op::lfsx: fload(ctx, base, w, true, false, true); break;
    case Op::lfsux: fload(ctx, base, w, true, true, true); break;
    case Op::lfd: fload(ctx, base, w, false, false, false); break;
    case Op::lfdu: fload(ctx, base, w, false, true, false); break;
    case Op::lfdx: fload(ctx, base, w, false, false, true); break;
    case Op::lfdux: fload(ctx, base, w, false, true, true); break;
    case Op::stfs: fstore(ctx, base, w, true, false, false); break;
    case Op::stfsu: fstore(ctx, base, w, true, true, false); break;
    case Op::stfsx: fstore(ctx, base, w, true, false, true); break;
    case Op::stfsux: fstore(ctx, base, w, true, true, true); break;
    case Op::stfd: fstore(ctx, base, w, false, false, false); break;
    case Op::stfdu: fstore(ctx, base, w, false, true, false); break;
    case Op::stfdx: fstore(ctx, base, w, false, false, true); break;
    case Op::stfdux: fstore(ctx, base, w, false, true, true); break;
    case Op::stfiwx: fstore(ctx, base, w, false, false, true, true); break;

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

    if (a == 0x02EF885Cu && w == 0x38E00002u && cafe_ppc_runtime_option(1)) {
        ctx.r[7] = 0;
    }
    return next;
}

uint32_t execute_block_cpp(PPCContext* ctx, uint8_t* base, const Block* block) {
    uint32_t next = block->start;
    for (const Decoded* ins : block->instructions) {
        next = execute_one(*ctx, base, *ins);
        if (next != ins->address + 4 || ends_block(ins->op)) {
            return next;
        }
    }
    return next;
}

#ifdef _WIN32
BlockFn emit_block_thunk(Block* block) {
    if (!g_jit_enabled || !g_jit_arena) return nullptr;

    // Windows x64 ABI:
    //   RCX = PPCContext*, RDX = guest base, R8 = Block*.
    // The thunk supplies R8 then tail-jumps to execute_block_cpp, preserving
    // the caller's shadow space and return address.
    constexpr size_t kThunkSize = 22;
    if (g_jit_offset + 32 > kJitArenaSize) return nullptr;
    g_jit_offset = (g_jit_offset + 15) & ~size_t{15};
    uint8_t* code = g_jit_arena + g_jit_offset;
    uint8_t* p = code;

    *p++ = 0x49; *p++ = 0xB8; // mov r8, imm64
    const uintptr_t block_ptr = reinterpret_cast<uintptr_t>(block);
    std::memcpy(p, &block_ptr, sizeof(block_ptr)); p += sizeof(block_ptr);

    *p++ = 0x48; *p++ = 0xB8; // mov rax, imm64
    const uintptr_t helper = reinterpret_cast<uintptr_t>(&execute_block_cpp);
    std::memcpy(p, &helper, sizeof(helper)); p += sizeof(helper);

    *p++ = 0xFF; *p++ = 0xE0; // jmp rax
    static_assert(kThunkSize == 22);
    g_jit_offset += static_cast<size_t>(p - code);
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(p - code));
    return reinterpret_cast<BlockFn>(code);
}
#endif

Block* get_or_build_block(uint32_t start) {
    const uint64_t epoch = g_hot_block_epoch.load(std::memory_order_relaxed);
    if (t_hot_block_epoch != epoch) {
        t_hot_blocks.fill({});
        t_hot_block_epoch = epoch;
    }
    HotBlockEntry& hot = t_hot_blocks[(start >> 2) & (kHotBlockCacheSize - 1)];
    if (hot.block != nullptr && hot.address == start) {
        return hot.block;
    }

    {
        std::lock_guard lock(g_block_mutex);
        const auto found = g_blocks.find(start);
        if (found != g_blocks.end()) {
            hot = {start, found->second.get()};
            return found->second.get();
        }
    }

    auto block = std::make_unique<Block>();
    block->start = start;
    uint32_t pc = start;
    for (size_t count = 0; count < kMaxBlockInstructions; ++count) {
        // Make the native presentation hook an explicit block boundary so
        // normal execution always passes through run()'s hook interception.
        if (pc == kPresentationHook && pc != start) break;
        const Decoded* ins = decoded_at(pc);
        if (!ins) break;
        block->instructions.push_back(ins);
        if (ends_block(ins->op)) break;
        pc += 4;
    }
    if (block->instructions.empty()) return nullptr;

    std::lock_guard lock(g_block_mutex);
    if (const auto found = g_blocks.find(start); found != g_blocks.end()) {
        hot = {start, found->second.get()};
        return found->second.get();
    }
    Block* raw = block.get();
    g_blocks.emplace(start, std::move(block));
    hot = {start, raw};
    remember_block_locked(start);
#ifdef _WIN32
    raw->native = emit_block_thunk(raw);
#endif
    return raw;
}

} // namespace

void initialize(const std::vector<uint8_t>& rpx_bytes) {
    const auto started = std::chrono::steady_clock::now();
    rpx::Image image = rpx::load(rpx_bytes);

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
#ifdef _WIN32
        reset_jit_arena();
#endif
    }

    g_hot_block_epoch.fetch_add(1, std::memory_order_relaxed);
    const char* trace_option = std::getenv("TTT2_TRACE_GUEST");
    g_full_instruction_trace = trace_option && *trace_option == '1';

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
            decoded.code.push_back({address, word, ppc::decode(word)});
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
        g_imports.emplace(import.stub_address, std::move(binding));
    }

    const std::vector<uint32_t> warm_blocks = prepare_persistent_block_cache(rpx_bytes);
    size_t warmed = 0;
    for (const uint32_t address : warm_blocks) {
        if (decoded_at(address) && get_or_build_block(address)) ++warmed;
    }

    g_enabled = true;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    std::fprintf(stderr,
                 "ttt2: dynamic host: decoded %zu PPC instructions across %zu code sections in %.2f ms\n",
                 instruction_count, g_code.size(), ms);
#ifdef _WIN32
    std::fprintf(stderr, "ttt2: dynamic host: x64 dispatch trampolines %s\n",
                 g_jit_enabled && g_jit_arena ? "enabled" : "disabled (direct C++ blocks)");
#endif
    std::fprintf(stderr, "ttt2: dynamic host: warmed %zu cached code blocks\n", warmed);
}

bool enabled() {
    std::lock_guard lock(g_mutex);
    return g_enabled;
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
    while (pc != stop) {
        // The static port replaces this function to observe the system-info
        // object after the original guest body returns. Reproduce the same
        // hook without requiring any generated C++ symbol.
        if (!suppress_presentation_hook && pc == 0x0864107Cu) {
            const uint32_t out = ctx.r[3];
            const uint32_t return_address = ctx.lr;
            run(ctx, base, pc, return_address, true);
            title::system_info_created(ppc::ld32(base, out));
            pc = return_address;
            continue;
        }

        if (auto it = g_imports.find(pc); it != g_imports.end() && !it->second.is_data) {
            if (!it->second.function) {
                fatal("unimplemented dynamic import %s:%s at 0x%08X",
                      it->second.module.c_str(), it->second.name.c_str(), pc);
            }
            it->second.function(ctx, base);
            pc = ctx.lr;
            continue;
        }

        if (PPCFunc* host = find_registered_host_function(pc)) {
            host(ctx, base);
            pc = ctx.lr;
            continue;
        }

        Block* block = get_or_build_block(pc);
        if (!block) {
            fatal("dynamic guest execution reached non-code address 0x%08X", pc);
        }

        ++t_blocks_executed;
        t_instructions_executed += block->instructions.size();
        if ((t_blocks_executed & 0x3FFFFu) == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - t_last_progress >= std::chrono::seconds(2)) {
                char progress[192]{};
                std::snprintf(progress, sizeof(progress),
                              "dynamic progress pc=0x%08X blocks=%llu instructions=%llu",
                              pc,
                              static_cast<unsigned long long>(t_blocks_executed),
                              static_cast<unsigned long long>(t_instructions_executed));
                diagnostic_trace(progress);
                t_last_progress = now;
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
}

void execute(PPCContext& ctx, uint8_t* base, uint32_t entry) {
    const uint32_t return_address = ctx.lr;
    run(ctx, base, entry, return_address, false);
}

uint32_t execute_test_instruction(PPCContext& ctx, uint8_t* base,
                                  uint32_t address, uint32_t word) {
    const Decoded instruction{address, word, ppc::decode(word)};
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
        section.code.push_back({address, words[i], ppc::decode(words[i])});
    }
    g_code.push_back(std::move(section));
    g_hot_block_epoch.fetch_add(1, std::memory_order_relaxed);
    g_enabled = true;
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
