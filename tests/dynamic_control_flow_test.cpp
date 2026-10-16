// Differential control-flow test: the same synthetic PowerPC programs are
// executed by the proven AOT code generator and the generic dynamic backend.

#include "cafe/dynamic.h"
#include "cafe/export.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "control_flow_cases.h"
#include "host/platform.h"
#include "title/presentation.h"

#include <bit>
#include <csetjmp>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::jmp_buf g_escape;
enum class Exit : int { returned = 0, trapped = 1, bad_return = 2, null_call = 3 };

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

void randomise(cafe::PPCContext& ctx, Rng& rng) {
    ctx = {};
    for (uint32_t& r : ctx.r) r = rng.u32();
    for (auto& f : ctx.f) {
        const int32_t a = static_cast<int32_t>(rng.u32());
        const int32_t b = static_cast<int32_t>(rng.u32());
        f.ps0 = std::ldexp(static_cast<double>(a), static_cast<int>(rng.below(30)) - 40);
        f.ps1 = std::ldexp(static_cast<double>(b), static_cast<int>(rng.below(30)) - 40);
    }
    for (auto& c : ctx.cr) {
        const uint32_t v = rng.below(16);
        c = {static_cast<uint8_t>((v >> 3) & 1),
             static_cast<uint8_t>((v >> 2) & 1),
             static_cast<uint8_t>((v >> 1) & 1),
             static_cast<uint8_t>(v & 1)};
    }
    for (uint32_t& g : ctx.gqr) g = rng.u32();
    ctx.lr = rng.u32();
    ctx.ctr = rng.u32();
    ctx.xer_so = static_cast<uint8_t>(rng.below(2));
    ctx.xer_ov = static_cast<uint8_t>(rng.below(2));
    ctx.xer_ca = static_cast<uint8_t>(rng.below(2));
    ctx.xer_bc = static_cast<uint8_t>(rng.below(0x80));
    ctx.fpscr = rng.u32();
    ctx.reserve_address = 0;
    ctx.reserve_value = 0;
    ctx.reserve_valid = 0;
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
#define FIELD(name, fmt)                                                       \
    do {                                                                       \
        if (expected.name != actual.name) {                                    \
            std::snprintf(line, sizeof line, #name " want " fmt " got " fmt,  \
                          expected.name, actual.name);                          \
            return line;                                                       \
        }                                                                      \
    } while (0)
    FIELD(lr, "%08X");
    FIELD(ctr, "%08X");
    FIELD(xer_so, "%u");
    FIELD(xer_ov, "%u");
    FIELD(xer_ca, "%u");
    FIELD(xer_bc, "%u");
    FIELD(fpscr, "%08X");
    FIELD(reserve_address, "%08X");
    FIELD(reserve_value, "%08X");
    FIELD(reserve_valid, "%u");
#undef FIELD
    return {};
}

Exit run_aot(const ControlFlowCase& test, cafe::PPCContext& ctx, uint8_t* memory) {
    const int escaped = setjmp(g_escape);
    if (escaped == 0) {
        test.function(ctx, memory);
        return Exit::returned;
    }
    return static_cast<Exit>(escaped);
}

Exit run_dynamic(const ControlFlowCase& test, cafe::PPCContext& ctx, uint8_t* memory) {
    const int escaped = setjmp(g_escape);
    if (escaped == 0) {
        cafe::dynamic::execute(ctx, memory, test.entry);
        return Exit::returned;
    }
    return static_cast<Exit>(escaped);
}

} // namespace

// Dependencies shared by generated AOT code and dynamic.cpp.
namespace cafe {
const Export* find_export(std::string_view, std::string_view) { return nullptr; }
const DataExport* find_data_export(std::string_view, std::string_view) { return nullptr; }
PPCFunc* find_registered_host_function(uint32_t) { return nullptr; }
std::string sha256_hex(const std::vector<uint8_t>&) { return "dynamic-control-test"; }

[[noreturn]] void fatal(const char* format, ...) {
    std::fprintf(stderr, "unexpected control-flow fatal: ");
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
cafe::PPCFunc* cafe_ppc_lookup(uint32_t address) {
    for (size_t i = 0; i < kGuestEntryCount; ++i) {
        if (kGuestEntries[i].address == address) return kGuestEntries[i].function;
    }
    std::fprintf(stderr, "lookup of unknown guest address %08X\n", address);
    std::abort();
}
[[noreturn]] void cafe_ppc_trap(cafe::PPCContext&, uint32_t) {
    std::longjmp(g_escape, static_cast<int>(Exit::trapped));
}
[[noreturn]] void cafe_ppc_bad_return(cafe::PPCContext&, uint32_t) {
    std::longjmp(g_escape, static_cast<int>(Exit::bad_return));
}
[[noreturn]] void cafe_ppc_null_call(cafe::PPCContext&, uint32_t) {
    std::longjmp(g_escape, static_cast<int>(Exit::null_call));
}
[[noreturn]] void cafe_ppc_illegal(cafe::PPCContext&, uint32_t a, uint32_t w,
                                    const char* reason) {
    std::fprintf(stderr, "illegal %08X at %08X: %s\n", w, a, reason);
    std::abort();
}
uint64_t cafe_ppc_timebase(void) { return 0; }
uint32_t cafe_ppc_runtime_option(uint32_t) { return 0; }
}

int main() {
    constexpr uint32_t kReturn = 0x00000100u;
    constexpr size_t kMemorySize = 0x00090000u;
    constexpr int kTrials = 32;

    std::vector<uint32_t> code(kCodeWords, kCodeWords + kCodeWordCount);
    cafe::dynamic::initialize_test_program(kCodeBase, code);

    std::vector<uint8_t> expected_memory(kMemorySize);
    std::vector<uint8_t> actual_memory(kMemorySize);

    size_t runs = 0;
    size_t failures = 0;
    for (size_t c = 0; c < kControlFlowCaseCount; ++c) {
        const ControlFlowCase& test = kControlFlowCases[c];
        Rng rng{0xC0DEF00D00000000ull ^ static_cast<uint64_t>(c)};

        for (int trial = 0; trial < kTrials; ++trial) {
            cafe::PPCContext expected{};
            randomise(expected, rng);
            if (test.r4_hi > test.r4_lo) {
                expected.r[4] = test.r4_lo + rng.below(test.r4_hi - test.r4_lo);
            }
            if (test.r5_hi > test.r5_lo) {
                expected.r[5] = test.r5_lo + rng.below(test.r5_hi - test.r5_lo);
            }
            if (test.r4_address) {
                expected.r[4] = 0x00048000u + (rng.below(0x4000) & ~31u);
                expected.r[8] = expected.r[4] + 64;
            }
            static constexpr uint32_t ctr_values[] = {0, 1, 2, 3, 7, 31};
            if (rng.below(2)) expected.ctr = ctr_values[rng.below(6)];
            expected.lr = kReturn;

            for (size_t i = 0; i < expected_memory.size(); i += 8) {
                const uint64_t value = rng.next();
                const size_t remain = expected_memory.size() - i;
                std::memcpy(expected_memory.data() + i, &value,
                            remain < sizeof(value) ? remain : sizeof(value));
            }
            actual_memory = expected_memory;
            cafe::PPCContext actual = expected;

            const Exit expected_exit = run_aot(test, expected, expected_memory.data());
            const Exit actual_exit = run_dynamic(test, actual, actual_memory.data());
            ++runs;

            std::string diff;
            if (expected_exit != actual_exit) {
                char line[96]{};
                std::snprintf(line, sizeof line, "exit want %d got %d",
                              static_cast<int>(expected_exit), static_cast<int>(actual_exit));
                diff = line;
            } else {
                diff = compare_context(expected, actual);
                if (diff.empty() && expected_memory != actual_memory) {
                    diff = "memory differs";
                }
            }

            if (!diff.empty()) {
                ++failures;
                std::fprintf(stderr,
                             "FAIL %-32s trial %d: %s\n",
                             test.name, trial, diff.c_str());
                if (failures >= 12) {
                    std::fprintf(stderr, "too many failures; stopping early\n");
                    return 1;
                }
            }
        }
    }

    std::printf("dynamic control flow: %zu programs, %zu runs, %zu failures\n",
                kControlFlowCaseCount, runs, failures);
    return failures == 0 ? 0 : 1;
}
