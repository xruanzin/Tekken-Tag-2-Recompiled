// Compares the generic runtime's one-instruction PowerPC executor with the
// project's proven ahead-of-time code generator. No game data is required.

#include "cafe/dynamic.h"
#include "cafe/export.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "host/platform.h"
#include "ppc.h"
#include "title/presentation.h"

#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

struct SemanticsTest {
    uint32_t word;
    cafe::PPCFunc* function;
};
extern const SemanticsTest kSemanticsTests[];
extern const size_t kSemanticsTestCount;

namespace {

struct Rng {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return static_cast<uint32_t>(next()); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

bool memory_op(cafe::ppc::Op op) {
    using cafe::ppc::Op;
    switch (op) {
    case Op::lbz: case Op::lbzu: case Op::lbzx: case Op::lbzux:
    case Op::lhz: case Op::lhzu: case Op::lhzx: case Op::lhzux:
    case Op::lha: case Op::lhau: case Op::lhax: case Op::lhaux:
    case Op::lwz: case Op::lwzu: case Op::lwzx: case Op::lwzux:
    case Op::stb: case Op::stbu: case Op::stbx: case Op::stbux:
    case Op::sth: case Op::sthu: case Op::sthx: case Op::sthux:
    case Op::stw: case Op::stwu: case Op::stwx: case Op::stwux:
    case Op::lhbrx: case Op::lwbrx: case Op::sthbrx: case Op::stwbrx:
    case Op::lmw: case Op::stmw: case Op::lswi: case Op::lswx:
    case Op::stswi: case Op::stswx:
    case Op::lfs: case Op::lfsu: case Op::lfsx: case Op::lfsux:
    case Op::lfd: case Op::lfdu: case Op::lfdx: case Op::lfdux:
    case Op::stfs: case Op::stfsu: case Op::stfsx: case Op::stfsux:
    case Op::stfd: case Op::stfdu: case Op::stfdx: case Op::stfdux:
    case Op::stfiwx:
    case Op::psq_l: case Op::psq_lu: case Op::psq_lx: case Op::psq_lux:
    case Op::psq_st: case Op::psq_stu: case Op::psq_stx: case Op::psq_stux:
    case Op::dcbz: case Op::dcbz_l:
        return true;
    default:
        return false;
    }
}

bool indexed_memory(cafe::ppc::Op op, uint32_t w) {
    if (cafe::ppc::opcd(w) == 31) return true;
    using cafe::ppc::Op;
    return op == Op::psq_lx || op == Op::psq_lux ||
           op == Op::psq_stx || op == Op::psq_stux || op == Op::dcbz_l;
}

void randomise(cafe::PPCContext& ctx, Rng& rng) {
    ctx = {};
    for (uint32_t& r : ctx.r) r = rng.u32();

    static constexpr double special[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5,
        INFINITY, -INFINITY
    };
    for (auto& f : ctx.f) {
        const auto value = [&]() {
            if (rng.below(5) == 0) return special[rng.below(static_cast<uint32_t>(std::size(special)))];
            const int32_t n = static_cast<int32_t>(rng.u32());
            return std::ldexp(static_cast<double>(n), static_cast<int>(rng.below(40)) - 51);
        };
        f.ps0 = value();
        f.ps1 = value();
    }

    for (auto& field : ctx.cr) {
        const uint32_t v = rng.below(16);
        field = {static_cast<uint8_t>((v >> 3) & 1),
                 static_cast<uint8_t>((v >> 2) & 1),
                 static_cast<uint8_t>((v >> 1) & 1),
                 static_cast<uint8_t>(v & 1)};
    }

    static constexpr uint32_t type[] = {0, 4, 5, 6, 7};
    for (uint32_t& gqr : ctx.gqr) {
        gqr = type[rng.below(5)] | (rng.below(64) << 8) |
              (type[rng.below(5)] << 16) | (rng.below(64) << 24);
    }
    ctx.lr = rng.u32();
    ctx.ctr = rng.u32();
    ctx.xer_so = static_cast<uint8_t>(rng.below(2));
    ctx.xer_ov = static_cast<uint8_t>(rng.below(2));
    ctx.xer_ca = static_cast<uint8_t>(rng.below(2));
    ctx.xer_bc = static_cast<uint8_t>(rng.below(0x80));
    ctx.fpscr = rng.u32();
    ctx.reserve_address = rng.u32();
    ctx.reserve_value = rng.u32();
    ctx.reserve_valid = static_cast<uint8_t>(rng.below(2));
}

void prepare_memory_address(cafe::PPCContext& ctx, uint32_t w) {
    const cafe::ppc::Op op = cafe::ppc::decode(w);
    if (!memory_op(op)) return;

    const uint32_t ra = cafe::ppc::ra(w);
    const uint32_t rb = cafe::ppc::rb(w);
    if (!indexed_memory(op, w)) {
        if (ra != 0) ctx.r[ra] = 0x00030000u;
        return;
    }

    if (ra != 0 && rb == ra) {
        ctx.r[ra] = 0x00018000u;
    } else {
        if (ra != 0) ctx.r[ra] = 0x00030000u;
        ctx.r[rb] = ra == 0 ? 0x00030000u : 0x00000080u;
    }
}

std::string compare_context(const cafe::PPCContext& expected,
                            const cafe::PPCContext& actual) {
    char line[192]{};
    for (int i = 0; i < 32; ++i) {
        if (expected.r[i] != actual.r[i]) {
            std::snprintf(line, sizeof line, "r%d want %08X got %08X",
                          i, expected.r[i], actual.r[i]);
            return line;
        }
        if (std::bit_cast<uint64_t>(expected.f[i].ps0) !=
            std::bit_cast<uint64_t>(actual.f[i].ps0)) {
            std::snprintf(line, sizeof line, "f%d.ps0 differs", i);
            return line;
        }
        if (std::bit_cast<uint64_t>(expected.f[i].ps1) !=
            std::bit_cast<uint64_t>(actual.f[i].ps1)) {
            std::snprintf(line, sizeof line, "f%d.ps1 differs", i);
            return line;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (std::memcmp(&expected.cr[i], &actual.cr[i], sizeof(cafe::CRField)) != 0) {
            std::snprintf(line, sizeof line, "cr%d differs", i);
            return line;
        }
        if (expected.gqr[i] != actual.gqr[i]) {
            std::snprintf(line, sizeof line, "gqr%d want %08X got %08X",
                          i, expected.gqr[i], actual.gqr[i]);
            return line;
        }
    }
#define CHECK_FIELD(field, fmt)                                                \
    do {                                                                       \
        if (expected.field != actual.field) {                                  \
            std::snprintf(line, sizeof line, #field " want " fmt " got " fmt, \
                          expected.field, actual.field);                        \
            return line;                                                       \
        }                                                                      \
    } while (0)
    CHECK_FIELD(lr, "%08X");
    CHECK_FIELD(ctr, "%08X");
    CHECK_FIELD(xer_so, "%u");
    CHECK_FIELD(xer_ov, "%u");
    CHECK_FIELD(xer_ca, "%u");
    CHECK_FIELD(xer_bc, "%u");
    CHECK_FIELD(fpscr, "%08X");
    CHECK_FIELD(reserve_address, "%08X");
    CHECK_FIELD(reserve_value, "%08X");
    CHECK_FIELD(reserve_valid, "%u");
#undef CHECK_FIELD
    return {};
}

} // namespace

// dynamic.cpp dependencies not needed by one-instruction semantics.
namespace cafe {
const Export* find_export(std::string_view, std::string_view) { return nullptr; }
const DataExport* find_data_export(std::string_view, std::string_view) { return nullptr; }
PPCFunc* find_registered_host_function(uint32_t) { return nullptr; }
const char* host_function_name(uint32_t) { return nullptr; }
std::string sha256_hex(const std::vector<uint8_t>&) { return "dynamic-test"; }
void diagnostic_trace(const char*) {}

[[noreturn]] void fatal(const char* format, ...) {
    std::fprintf(stderr, "unexpected dynamic-test fatal: ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::abort();
}
} // namespace cafe

namespace cafe::host {
std::filesystem::path data_directory() {
    return std::filesystem::temp_directory_path();
}
} // namespace cafe::host

namespace cafe::title {
void system_info_created(uint32_t) {}
} // namespace cafe::title

extern "C" {
cafe::PPCFunc* cafe_ppc_lookup(uint32_t) { return nullptr; }
[[noreturn]] void cafe_ppc_trap(cafe::PPCContext&, uint32_t) { std::abort(); }
[[noreturn]] void cafe_ppc_illegal(cafe::PPCContext&, uint32_t, uint32_t, const char*) {
    std::abort();
}
[[noreturn]] void cafe_ppc_null_call(cafe::PPCContext&, uint32_t) { std::abort(); }
[[noreturn]] void cafe_ppc_bad_return(cafe::PPCContext&, uint32_t) { std::abort(); }
uint64_t cafe_ppc_timebase(void) { return 0; }
uint32_t cafe_ppc_runtime_option(uint32_t) { return 0; }
}

int main() {
    constexpr size_t kMemorySize = 0x80000;
    constexpr int kTrials = 8;
    constexpr uint32_t kAddress = 0x01000000u;

    std::vector<uint8_t> expected_memory(kMemorySize);
    std::vector<uint8_t> actual_memory(kMemorySize);

    size_t runs = 0;
    for (size_t index = 0; index < kSemanticsTestCount; ++index) {
        const SemanticsTest& test = kSemanticsTests[index];
        Rng rng{0xD19A000000000000ull ^ test.word};

        for (int trial = 0; trial < kTrials; ++trial) {
            cafe::PPCContext expected{};
            randomise(expected, rng);
            prepare_memory_address(expected, test.word);
            cafe::PPCContext actual = expected;

            for (size_t i = 0; i < expected_memory.size(); i += 8) {
                const uint64_t value = rng.next();
                std::memcpy(expected_memory.data() + i, &value, sizeof value);
            }
            actual_memory = expected_memory;

            test.function(expected, expected_memory.data());
            const uint32_t next =
                cafe::dynamic::execute_test_instruction(actual, actual_memory.data(),
                                                        kAddress, test.word);
            if (next != kAddress + 4) {
                std::fprintf(stderr, "FAIL %08X: dynamic next PC %08X\n",
                             test.word, next);
                return 1;
            }

            const std::string diff = compare_context(expected, actual);
            if (!diff.empty()) {
                std::fprintf(stderr, "FAIL %08X %-16s: %s\n", test.word,
                             cafe::ppc::op_name(cafe::ppc::decode(test.word)),
                             diff.c_str());
                return 1;
            }
            if (expected_memory != actual_memory) {
                std::fprintf(stderr, "FAIL %08X %-16s: memory differs\n",
                             test.word,
                             cafe::ppc::op_name(cafe::ppc::decode(test.word)));
                return 1;
            }
            ++runs;
        }
    }

    std::printf("dynamic semantics: %zu encodings, %zu matching runs\n",
                kSemanticsTestCount, runs);
    return kSemanticsTestCount == 0 ? 1 : 0;
}
