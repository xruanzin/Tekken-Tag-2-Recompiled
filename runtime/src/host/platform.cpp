#include "host/platform.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace cafe::host {
namespace fs = std::filesystem;

fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return fs::current_path();
    buffer.resize(length);
    return fs::path(buffer);
#else
    std::error_code ec;
    fs::path path = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : path;
#endif
}

fs::path home_directory() {
#ifdef _WIN32
    if (const char* value = std::getenv("USERPROFILE"); value && *value) return fs::path(value);
    if (const char* drive = std::getenv("HOMEDRIVE"); drive && *drive) {
        if (const char* path = std::getenv("HOMEPATH"); path && *path) return fs::path(std::string(drive) + path);
    }
    return fs::current_path();
#else
    if (const char* value = std::getenv("HOME"); value && *value) return fs::path(value);
    return fs::current_path();
#endif
}

static fs::path env_or(const char* variable, const fs::path& fallback) {
    if (const char* value = std::getenv(variable); value && *value) return fs::path(value);
    return fallback;
}

fs::path config_directory() {
#ifdef _WIN32
    return env_or("LOCALAPPDATA", home_directory() / "AppData/Local");
#else
    return env_or("XDG_CONFIG_HOME", home_directory() / ".config");
#endif
}

fs::path data_directory() {
    if (const char* value = std::getenv("TTT2_USER_DIR"); value && *value) return fs::path(value);
#ifdef _WIN32
    return env_or("LOCALAPPDATA", home_directory() / "AppData/Local");
#else
    return env_or("XDG_DATA_HOME", home_directory() / ".local/share");
#endif
}

fs::path cache_directory() {
    if (const char* value = std::getenv("TTT2_USER_DIR"); value && *value) return fs::path(value);
#ifdef _WIN32
    return env_or("LOCALAPPDATA", home_directory() / "AppData/Local");
#else
    return env_or("XDG_CACHE_HOME", home_directory() / ".cache");
#endif
}

std::string path_string(const fs::path& path) {
#ifdef _WIN32
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return path.string();
#endif
}

bool is_terminal(FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

int64_t local_utc_offset_seconds(std::time_t when) {
#ifdef _WIN32
    std::tm local{};
    std::tm utc{};
    if (localtime_s(&local, &when) != 0 || gmtime_s(&utc, &when) != 0) return 0;
    // mktime interprets a broken-down time as local time. Give the UTC
    // broken-down value the same DST state as the local value, so their
    // difference is the effective UTC offset for this instant.
    utc.tm_isdst = local.tm_isdst;
    const std::time_t local_value = std::mktime(&local);
    const std::time_t utc_as_local = std::mktime(&utc);
    if (local_value == static_cast<std::time_t>(-1) || utc_as_local == static_cast<std::time_t>(-1)) return 0;
    return static_cast<int64_t>(std::difftime(local_value, utc_as_local));
#else
    std::tm local{};
    if (localtime_r(&when, &local) == nullptr) return 0;
    return static_cast<int64_t>(local.tm_gmtoff);
#endif
}

uint64_t system_memory_total_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return static_cast<uint64_t>(status.ullTotalPhys);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
}

uint64_t system_memory_available_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return static_cast<uint64_t>(status.ullAvailPhys);
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
}

void set_thread_name(std::string_view name) {
#ifdef _WIN32
    if (name.empty()) return;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (needed <= 0) return;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), needed);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
    std::string short_name(name.substr(0, 15));
    pthread_setname_np(pthread_self(), short_name.c_str());
#endif
}

void set_thread_high_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

void set_thread_low_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void set_guest_core_hint(int core) {
#ifdef _WIN32
    if (core < 0) return;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const DWORD logical = std::max<DWORD>(1, info.dwNumberOfProcessors);

    // On common SMT systems (including Ryzen 5 4600G) adjacent logical
    // processors are sibling threads of one physical core. Prefer separate
    // physical cores for Cafe core 0/1/2 without hard-pinning every guest
    // thread, so Windows can still migrate under load.
    DWORD ideal = static_cast<DWORD>(core);
    if (logical >= 6) ideal = static_cast<DWORD>(core * 2);
    ideal %= logical;
    SetThreadIdealProcessor(GetCurrentThread(), ideal);
#else
    (void)core;
#endif
}


} // namespace cafe::host
