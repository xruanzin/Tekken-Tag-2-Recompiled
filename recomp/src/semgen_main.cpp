// cafe-semgen: emit single-instruction functions used by differential
// semantics tests. It can sample a real RPX or generate architecture-level
// representative encodings without any game data.

#include "codegen.h"
#include "ppc.h"
#include "program.h"
#include "rpx.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace cafe;

namespace {

bool testable(uint32_t w) {
    using ppc::Op;
    const Op op = ppc::decode(w);
    // Record forms of FP instructions depend on FPSCR fields this runtime
    // intentionally does not model.
    if (ppc::has_record_form(op) && ppc::rc(w) && op >= Op::fadd) return false;

    switch (op) {
    case Op::invalid: case Op::count:
    case Op::b: case Op::bc: case Op::bclr: case Op::bcctr:
    case Op::sc: case Op::rfi: case Op::tw: case Op::twi: case Op::tdi:
    case Op::mftb: case Op::mfmsr: case Op::mtmsr: case Op::mfsr: case Op::mtsr:
    case Op::mfsrin: case Op::mtsrin: case Op::tlbie: case Op::tlbsync:
    case Op::eciwx: case Op::ecowx: case Op::mffs: case Op::mtfsf: case Op::mtfsfi:
    case Op::mtfsb0: case Op::mtfsb1: case Op::mcrfs:
    case Op::lwarx: case Op::stwcx:
    case Op::sync: case Op::isync: case Op::eieio:
    case Op::dcbf: case Op::dcbi: case Op::dcbst: case Op::dcbt: case Op::dcbtst:
    case Op::icbi:
        return false;
    case Op::mfspr: case Op::mtspr: {
        const uint32_t spr = ppc::spr(w);
        return spr == 1 || spr == 8 || spr == 9 || (spr >= 896 && spr < 904) ||
               (spr >= 912 && spr < 920);
    }
    default:
        return true;
    }
}

bool memory_op(ppc::Op op) {
    using ppc::Op;
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

std::string variant(uint32_t w) {
    const ppc::Op op = ppc::decode(w);
    std::string name = ppc::op_name(op);
    if (ppc::has_overflow_form(op) && ppc::oe(w)) name += 'o';
    if (ppc::has_record_form(op) && ppc::rc(w)) name += '.';
    return name;
}

uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15;
    x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

using Samples = std::map<std::string, std::set<std::pair<uint32_t, uint32_t>>>;

void keep(Samples& samples, uint32_t w, size_t per_variant) {
    if (!testable(w)) return;
    auto& set = samples[variant(w)];
    set.insert({mix(w), w});
    if (set.size() > per_variant) set.erase(std::prev(set.end()));
}

std::vector<uint32_t> flatten(const Samples& samples) {
    std::vector<uint32_t> words;
    for (const auto& [name, set] : samples) {
        (void)name;
        for (const auto& [hash, w] : set) {
            (void)hash;
            words.push_back(w);
        }
    }
    return words;
}

std::vector<uint32_t> sample_game(const std::filesystem::path& path,
                                  size_t per_variant) {
    const recomp::Program game = recomp::Program::analyze(rpx::load(path));
    Samples samples;
    for (const auto& function : game.functions) {
        for (uint32_t a = function.address;
             a < function.address + function.size; a += 4) {
            keep(samples, game.word(a), per_variant);
        }
    }
    return flatten(samples);
}

// Builds deterministic representative encodings without using an RPX.
// Primary opcode + low 11 bits cover every decoder selector; bits 11..25
// are then varied to exercise registers and operands while preserving the
// selected operation.
std::vector<uint32_t> sample_generic(size_t per_variant) {
    Samples samples;
    for (uint32_t primary = 0; primary < 64; ++primary) {
        for (uint32_t low = 0; low < 2048; ++low) {
            const uint32_t selector = (primary << 26) | low;
            const ppc::Op op = ppc::decode(selector);
            if (op == ppc::Op::invalid || op == ppc::Op::count) continue;

            for (uint32_t sample = 0; sample < 8; ++sample) {
                uint32_t w = (primary << 26) |
                             (mix(selector ^ (sample * 0x9E3779B9u)) & 0x03FFF800u) |
                             low;
                if (ppc::decode(w) != op) continue;

                // D-form memory with rA=0 can address the very top of the
                // 32-bit space for negative displacements. Keep generic
                // differential tests in their compact low-memory sandbox.
                const bool indexed = ppc::opcd(w) == 31 ||
                                     (ppc::opcd(w) == 4 &&
                                      (op == ppc::Op::psq_lx || op == ppc::Op::psq_lux ||
                                       op == ppc::Op::psq_stx || op == ppc::Op::psq_stux ||
                                       op == ppc::Op::dcbz_l));
                if (memory_op(op) && !indexed && ppc::ra(w) == 0) {
                    w |= (1u << 16); // rA = 1
                    if (ppc::decode(w) != op) continue;
                }
                keep(samples, w, per_variant);
            }
        }
    }
    return flatten(samples);
}

void emit_cases(const std::vector<uint32_t>& tested,
                const std::filesystem::path& output) {
    std::vector<uint32_t> words;
    words.reserve(tested.size() * 2);
    for (const uint32_t w : tested) {
        words.push_back(w);
        words.push_back(0x4E800020u); // blr
    }

    constexpr uint32_t kBase = 0x01000000;
    const recomp::Program program = recomp::Program::synthetic(kBase, words, 2);
    std::string out =
        "// Generated by cafe-semgen.\n#include \"cafe/ppc_ops.h\"\n\n"
        "namespace ppc = ::cafe::ppc;\n\n";
    std::set<std::string> referenced;
    const recomp::EmitOptions options{true};
    for (const auto& [address, entry] : program.entries) {
        out += recomp::emit_entry(program, entry, options, referenced);
    }
    out += "struct SemanticsTest { uint32_t word; ::cafe::PPCFunc* function; };\n"
           "extern const SemanticsTest kSemanticsTests[] = {\n";
    for (size_t i = 0; i < tested.size(); ++i) {
        out += std::format("\t{{0x{:08X}u, {}_orig}},\n", tested[i],
                           recomp::function_name(kBase + static_cast<uint32_t>(i) * 8));
    }
    out += std::format("}};\nextern const size_t kSemanticsTestCount = {};\n",
                       tested.size());
    std::ofstream(output, std::ios::binary) << out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--generic") {
            if (argc != 3 && argc != 4) {
                std::fprintf(stderr,
                             "usage: %s --generic OUTPUT.cpp [per-variant]\n",
                             argv[0]);
                return 2;
            }
            const size_t per_variant = argc == 4 ? std::stoul(argv[3]) : 6;
            const std::vector<uint32_t> tested = sample_generic(per_variant);
            emit_cases(tested, argv[2]);
            std::printf("generic semantics: %zu encodings\n", tested.size());
            return tested.empty() ? 1 : 0;
        }

        if (argc != 3 && argc != 4) {
            std::fprintf(stderr,
                         "usage: %s Tekken.rpx OUTPUT.cpp [per-variant]\n"
                         "       %s --generic OUTPUT.cpp [per-variant]\n",
                         argv[0], argv[0]);
            return 2;
        }
        const size_t per_variant = argc == 4 ? std::stoul(argv[3]) : 48;
        const std::vector<uint32_t> tested = sample_game(argv[1], per_variant);
        emit_cases(tested, argv[2]);
        std::printf("game semantics: %zu encodings\n", tested.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
