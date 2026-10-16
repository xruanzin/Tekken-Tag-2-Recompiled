#include "cafe/dynamic.h"
#include "cafe/generated.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "host/platform.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cafe {
namespace {

thread_local PPCContext* t_current = nullptr;

#ifdef _WIN32
std::filesystem::path diagnostic_directory() {
    std::wstring user(32768, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(L"TTT2_USER_DIR", user.data(), static_cast<DWORD>(user.size()));
    if (length != 0 && length < user.size()) {
        user.resize(length);
        return std::filesystem::path(user);
    }

    std::wstring cwd(32768, L'\0');
    const DWORD cwd_length = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
    if (cwd_length != 0 && cwd_length < cwd.size()) {
        cwd.resize(cwd_length);
        return std::filesystem::path(cwd);
    }
    return {};
}

void write_raw_trace_file(const wchar_t* filename, const char* category, const char* text,
                          bool truncate = false) {
    const std::filesystem::path directory = diagnostic_directory();
    if (directory.empty()) return;
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::filesystem::path path = directory / filename;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[4096]{};
    const int count = std::snprintf(
        line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] %-6s %s\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), category ? category : "TRACE",
        text ? text : "");
    if (count > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(std::min<int>(count, sizeof(line) - 1)),
                  &written, nullptr);
        FlushFileBuffers(file);
    }
    CloseHandle(file);
}

void write_raw_crash(EXCEPTION_POINTERS* exception, uintptr_t fault_address, uintptr_t pc) {
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    char message[2048]{};
#if defined(_M_X64) || defined(__x86_64__)
    const CONTEXT* context = exception->ContextRecord;
    std::snprintf(
        message, sizeof(message),
        "exception=0x%08lX exception_address=%p fault_address=%p rip=%p rsp=%p rbp=%p "
        "rax=%p rbx=%p rcx=%p rdx=%p",
        static_cast<unsigned long>(code),
        exception->ExceptionRecord->ExceptionAddress,
        reinterpret_cast<void*>(fault_address),
        reinterpret_cast<void*>(pc),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rsp)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rbp)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rax)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rbx)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rcx)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(context->Rdx)));
#else
    std::snprintf(message, sizeof(message),
                  "exception=0x%08lX exception_address=%p fault_address=%p pc=%p",
                  static_cast<unsigned long>(code),
                  exception->ExceptionRecord->ExceptionAddress,
                  reinterpret_cast<void*>(fault_address),
                  reinterpret_cast<void*>(pc));
#endif
    write_raw_trace_file(L"crash_trace.log", "CRASH", message, true);
    write_raw_trace_file(L"boot_trace.log", "CRASH", message);

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(pc), &memory, sizeof(memory)) != 0 &&
        memory.AllocationBase != nullptr) {
        wchar_t module_path[MAX_PATH]{};
        const HMODULE module = static_cast<HMODULE>(memory.AllocationBase);
        const DWORD module_length = GetModuleFileNameW(module, module_path, MAX_PATH);
        if (module_length != 0) {
            char module_utf8[2048]{};
            WideCharToMultiByte(CP_UTF8, 0, module_path, -1, module_utf8,
                                static_cast<int>(sizeof(module_utf8)), nullptr, nullptr);
            char where[3072]{};
            std::snprintf(where, sizeof(where), "module=%s+0x%llX", module_utf8,
                          static_cast<unsigned long long>(
                              pc - reinterpret_cast<uintptr_t>(memory.AllocationBase)));
            write_raw_trace_file(L"crash_trace.log", "CRASH", where);
            write_raw_trace_file(L"boot_trace.log", "CRASH", where);
        }
    }
}
#endif

void print_backtrace(const PPCContext& ctx) {
    std::fprintf(stderr, "guest backtrace (LR, then saved return addresses):\n");
    std::fprintf(stderr, "  0x%08X\n", ctx.lr);
    uint32_t frame = ctx.r[1];
    for (int depth = 0; depth < 48; ++depth) {
        if (!guest_memory_committed(frame, 8)) break;
        const uint32_t back = ppc::ld32(guest_base(), frame);
        if (back == 0 || back <= frame || !guest_memory_committed(back, 8)) break;
        std::fprintf(stderr, "  0x%08X\n", ppc::ld32(guest_base(), back + 4));
        frame = back;
    }
}

void report_fault(uintptr_t host_address, uintptr_t pc, unsigned long code) {
    const auto base = reinterpret_cast<uintptr_t>(guest_base());
    if (base != 0 && host_address >= base && host_address - base < (uint64_t{1} << 32) + 0x10000) {
        const uint64_t guest = host_address - base;
        std::fprintf(stderr, "\nguest memory fault: access to unmapped guest address 0x%08llX\n",
                     static_cast<unsigned long long>(guest));
        if (t_current != nullptr && guest < t_current->r[1] && t_current->r[1] - guest <= (4u << 20)) {
            std::fprintf(stderr, "(0x%llX bytes below the stack pointer: a stack overflow)\n",
                         static_cast<unsigned long long>(t_current->r[1] - guest));
        }
    } else {
        std::fprintf(stderr, "\nhost fault 0x%08lX at %p\n", code,
                     reinterpret_cast<void*>(host_address));
    }
#ifdef _WIN32
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "host code: %s+0x%llx\n",
                 host::path_string(host::executable_path()).c_str(),
                 static_cast<unsigned long long>(pc - module));
#else
    Dl_info where{};
    if (dladdr(reinterpret_cast<void*>(pc), &where) && where.dli_fname != nullptr) {
        std::fprintf(stderr, "host code: %s+0x%llx\n", where.dli_fname,
                     static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(where.dli_fbase)));
    }
#endif
    if (t_current != nullptr) print_guest_state(*t_current);
    dynamic::print_recent_trace();
}

#ifdef _WIN32
LONG WINAPI fault_handler(EXCEPTION_POINTERS* exception) {
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    uintptr_t address = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        exception->ExceptionRecord->NumberParameters >= 2) {
        address = static_cast<uintptr_t>(exception->ExceptionRecord->ExceptionInformation[1]);
    }
#if defined(_M_X64) || defined(__x86_64__)
    const uintptr_t pc = static_cast<uintptr_t>(exception->ContextRecord->Rip);
#else
    const uintptr_t pc = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
#endif
    write_raw_crash(exception, address, pc);
    report_fault(address, pc, code);
    std::fflush(stderr);
    std::_Exit(70);
}
#else
void fault_handler(int signal, siginfo_t* info, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    const auto pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    report_fault(reinterpret_cast<uintptr_t>(info->si_addr), pc, static_cast<unsigned long>(signal));
    std::_Exit(128 + signal);
}
#endif

} // namespace

PPCContext* current_context() { return t_current; }
void set_current_context(PPCContext* ctx) { t_current = ctx; }

void begin_diagnostic_trace() {
#ifdef _WIN32
    write_raw_trace_file(L"boot_trace.log", "BOOT", "trace started", true);
    // Clear the previous crash file so a stale crash cannot be mistaken for
    // the current run.
    const std::filesystem::path directory = diagnostic_directory();
    if (!directory.empty()) {
        DeleteFileW((directory / L"crash_trace.log").c_str());
    }
#else
    std::fprintf(stderr, "ttt2: diagnostic trace started\n");
    std::fflush(stderr);
#endif
}

void diagnostic_trace(const char* stage) {
#ifdef _WIN32
    write_raw_trace_file(L"boot_trace.log", "BOOT", stage);
#else
    std::fprintf(stderr, "ttt2: boot trace: %s\n", stage ? stage : "");
    std::fflush(stderr);
#endif
}

void print_guest_state(const PPCContext& ctx) {
    for (int i = 0; i < 32; i += 4) {
        std::fprintf(stderr, "  r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i,
                     ctx.r[i], i + 1, ctx.r[i + 1], i + 2, ctx.r[i + 2], i + 3, ctx.r[i + 3]);
    }
    std::fprintf(stderr, "  lr %08X  ctr %08X  cr %08X  xer %08X\n", ctx.lr, ctx.ctr,
                 ppc::mfcr(ctx), ppc::mfxer(ctx));
    print_backtrace(ctx);
}

void fatal(const char* format, ...) {
    std::fflush(stdout);
    std::fprintf(stderr, "\nttt2: ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    if (t_current != nullptr) print_guest_state(*t_current);
    dynamic::print_recent_trace();
    std::fflush(stderr);
    std::_Exit(70);
}

void install_fault_handler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(fault_handler);
    diagnostic_trace("fault handler installed");
#else
    static char alternate_stack[64 * 1024];
    stack_t stack{};
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof alternate_stack;
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
#endif
}

} // namespace cafe

extern "C" {

void cafe_ppc_trap(cafe::PPCContext& ctx, uint32_t address) {
    cafe::set_current_context(&ctx);
    cafe::fatal("guest trap instruction at 0x%08X (an assertion in game code)", address);
}

void cafe_ppc_illegal(cafe::PPCContext& ctx, uint32_t address, uint32_t word,
                      const char* reason) {
    cafe::set_current_context(&ctx);
    cafe::fatal("cannot execute instruction 0x%08X at 0x%08X: %s", word, address, reason);
}

void cafe_ppc_null_call(cafe::PPCContext& ctx, uint32_t address) {
    cafe::set_current_context(&ctx);
    cafe::fatal("call through an unresolved weak symbol (address 0) at 0x%08X", address);
}

void cafe_ppc_bad_return(cafe::PPCContext& ctx, uint32_t expected) {
    cafe::set_current_context(&ctx);
    cafe::fatal("a guest call returned with LR=0x%08X instead of its return address "
                "0x%08X: the callee returned somewhere other than its caller "
                "(longjmp-style control flow the port does not model yet)",
                ctx.lr, expected);
}

uint64_t cafe_ppc_timebase(void) {
    static const auto start = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    constexpr uint64_t kHz = 62156250;
    const auto t = static_cast<uint64_t>(ns);
    return (t / 1000000000u) * kHz + (t % 1000000000u) * kHz / 1000000000u;
}

void cafe_unimplemented_import(cafe::PPCContext& ctx, const char* module, const char* name) {
    cafe::set_current_context(&ctx);
    cafe::fatal("the game called %s:%s, which the runtime does not implement yet", module, name);
}

} // extern "C"
