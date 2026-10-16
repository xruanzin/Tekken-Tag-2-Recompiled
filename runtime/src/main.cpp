#include "cafe/dynamic.h"
#include "cafe/export.h"
#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"
#include "cafe/vfs.h"

#include "host/audio.h"
#include "host/window.h"
#include "host/platform.h"
#include "install.h"
#include "rpx.h"
#include "title/presentation.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace cafe::os {
void init_heaps(uint32_t mem2_begin);
int run_main_thread(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size,
                    uint32_t sda_base, uint32_t sda2_base);
} // namespace cafe::os

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [GAME]\n"
                 "       %s --verify GAME\n"
                 "       %s --install [GAME [FOLDER]] [--no-launcher]\n"
                 "       %s --update [FOLDER]\n"
                 "  GAME is the title's .wua archive, or a folder with code/,\n"
                 "  content/ and meta/; without it the installed game runs.\n"
                 "  --install copies the game, this executable and its icon into\n"
                 "  FOLDER (chosen in a dialog when not given); --update copies\n"
                 "  this executable into the install after a rebuild. Saves go\n"
                 "  to $TTT2_SAVE_DIR (or %%TTT2_SAVE_DIR%% on Windows).\n",
                 argv0, argv0, argv0, argv0);
}

// Objects the game imports as data are created by the library that owns
// them; anything the runtime does not provide yet stays zeroed.
void init_data_imports() {
    for (size_t i = 0; i < cafe_guest_import_count; ++i) {
        const cafe::GuestImport& import = cafe_guest_imports[i];
        if (!import.is_data) continue;
        if (const cafe::DataExport* data = cafe::find_data_export(import.module, import.name)) {
            data->initialize(import.address);
        } else {
            std::fprintf(stderr, "ttt2: warning: data import %s:%s is not provided\n", import.module,
                         import.name);
        }
    }
}

uint32_t guest_strdup(const char* text) {
    const uint32_t address = cafe::system_alloc(static_cast<uint32_t>(std::strlen(text) + 1));
    std::memcpy(cafe::guest_pointer(address), text, std::strlen(text) + 1);
    return address;
}

std::filesystem::path log_directory_for_game(const std::filesystem::path& game) {
    if (const char* user = std::getenv("TTT2_USER_DIR"); user && *user) {
        return std::filesystem::path(user);
    }
    std::error_code ec;
    if (std::filesystem::is_directory(game, ec)) return game / "logs";
    if (game.has_parent_path()) return game.parent_path() / "logs";
    return std::filesystem::current_path(ec) / "logs";
}

void redirect_runtime_log(const std::filesystem::path& game) {
    std::filesystem::path directory = log_directory_for_game(game);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        ec.clear();
        directory = cafe::host::data_directory() / "ttt2" / "logs";
        std::filesystem::create_directories(directory, ec);
    }
    if (ec) return;

    const std::filesystem::path file =
        directory / (std::getenv("TTT2_USER_DIR") ? "last_run.log" : "ttt2-last-run.txt");
#ifdef _WIN32
    // Do not use freopen/_fileno on the GUI-subsystem streams here. When the
    // launcher starts the runtime with CREATE_NO_WINDOW, the MSVC CRT may
    // expose stdout/stderr as invalid pseudo streams; touching them through
    // _wfreopen_s/_fileno can invoke the invalid-parameter fast-fail path
    // (0xC0000409) before a single normal log line is written.
    //
    // Instead create a real Win32 file handle, import it into the CRT, then
    // duplicate that known-valid descriptor directly onto fd 1 and fd 2.
    diagnostic_trace("redirect_runtime_log: CreateFileW");
    HANDLE raw = CreateFileW(file.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        diagnostic_trace("redirect_runtime_log: CreateFileW failed");
        return;
    }

    diagnostic_trace("redirect_runtime_log: _open_osfhandle");
    const int log_fd = _open_osfhandle(reinterpret_cast<intptr_t>(raw), _O_WRONLY | _O_TEXT);
    if (log_fd < 0) {
        CloseHandle(raw); // ownership was not transferred
        diagnostic_trace("redirect_runtime_log: _open_osfhandle failed");
        return;
    }

    diagnostic_trace("redirect_runtime_log: _dup2 stderr");
    const int stderr_result = _dup2(log_fd, 2);
    diagnostic_trace("redirect_runtime_log: _dup2 stdout");
    const int stdout_result = _dup2(log_fd, 1);

    // _open_osfhandle transferred ownership of raw to log_fd. The duplicated
    // descriptors keep their own CRT references alive after this close.
    _close(log_fd);

    if (stderr_result != 0) {
        diagnostic_trace("redirect_runtime_log: stderr dup failed");
        return;
    }

    const intptr_t stderr_handle = _get_osfhandle(2);
    if (stderr_handle != -1) {
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(stderr_handle));
    }
    if (stdout_result == 0) {
        const intptr_t stdout_handle = _get_osfhandle(1);
        if (stdout_handle != -1) {
            SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(stdout_handle));
        }
    }

    diagnostic_trace("redirect_runtime_log: stdio descriptors ready");
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    if (stdout_result == 0) std::setvbuf(stdout, nullptr, _IOLBF, 0);
#else
    bool stdout_redirected = false;
    if (std::freopen(file.c_str(), "w", stderr)) {
        stdout_redirected = (::dup2(fileno(stderr), fileno(stdout)) >= 0);
    }
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    if (stdout_redirected) std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    std::fprintf(stderr, "ttt2: runtime log redirected to %s\n", file.string().c_str());
    std::fflush(stderr);
}

} // namespace

int main(int argc, char** argv) {
    using namespace cafe;
    if (argc >= 2 && (std::strcmp(argv[1], "--install") == 0 || std::strcmp(argv[1], "--update") == 0)) {
        return install::run(std::vector<std::string>(argv + 1, argv + argc));
    }
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }

    const bool verify_only = argc == 3 && std::strcmp(argv[1], "--verify") == 0;
    std::filesystem::path game;
    if (verify_only) {
        game = argv[2];
    } else if (argc == 2) {
        game = argv[1];
    } else if (const auto installed = install::installed_game(); argc == 1 && installed) {
        game = *installed;
    } else {
        usage(argv[0]);
        return 2;
    }
    begin_diagnostic_trace();
    diagnostic_trace(verify_only ? "mode=verify" : "mode=run");
    install_fault_handler();

    diagnostic_trace("redirect_runtime_log: begin");
    redirect_runtime_log(game);
    diagnostic_trace("redirect_runtime_log: complete");

    diagnostic_trace("vfs::open_game: begin");
    vfs::open_game(game);
    diagnostic_trace("vfs::open_game: complete");
    diagnostic_trace("save root: begin");
    if (const char* save = std::getenv("TTT2_SAVE_DIR")) {
        vfs::set_save_root(save);
    } else {
        vfs::set_save_root(host::data_directory() / "ttt2" / "save");
    }
    diagnostic_trace("save root: complete");

    diagnostic_trace("read /vol/code/Tekken.rpx: begin");
    const auto rpx = vfs::read_whole("/vol/code/Tekken.rpx");
    if (!rpx) fatal("the game source has no code/Tekken.rpx");
    {
        char marker[128]{};
        std::snprintf(marker, sizeof(marker), "read Tekken.rpx: complete (%zu bytes)", rpx->size());
        diagnostic_trace(marker);
    }

    if (verify_only) {
        diagnostic_trace("verify: sha256 begin");
        const std::string digest = sha256_hex(*rpx);
        diagnostic_trace("verify: sha256 complete");
        if (digest != cafe_program_info.rpx_sha256) {
            std::fprintf(stderr,
                         "ttt2: unsupported game executable\n"
                         "ttt2: expected EU v16 Tekken.rpx SHA-256 %s\n"
                         "ttt2: found                            %s\n",
                         cafe_program_info.rpx_sha256, digest.c_str());
            return 3;
        }
        try {
            const cafe::rpx::Image parsed = cafe::rpx::load(*rpx);
            std::fprintf(stderr,
                         "ttt2: recognized supported EU v16 game: entry 0x%08X, %zu imports\n",
                         parsed.entry_point, parsed.imports.size());
            if (cafe_program_info.compiled_entries != 0) {
                std::fprintf(stderr,
                             "ttt2: static AOT runtime: %zu/%zu guest entries compiled\n",
                             cafe_program_info.compiled_entries,
                             cafe_program_info.total_entries);
            } else {
                std::fprintf(stderr,
                             "ttt2: experimental generic runtime detected; player releases require "
                             "the static AOT runtime\n");
                return 5;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "ttt2: invalid Tekken.rpx: %s\n", error.what());
            return 4;
        }
        diagnostic_trace("verify: success");
        return 0;
    }

    diagnostic_trace("reserve_guest_memory: begin");
    reserve_guest_memory();
    diagnostic_trace("reserve_guest_memory: complete");

    diagnostic_trace("init_system_heap: begin");
    init_system_heap();
    diagnostic_trace("init_system_heap: complete");

    diagnostic_trace("load_image: begin");
    const LoadedImage image = load_image(*rpx);
    {
        char marker[192]{};
        std::snprintf(marker, sizeof(marker),
                      "load_image: complete entry=0x%08X stack=0x%X data_end=0x%08X",
                      image.entry_point, image.stack_size, image.data_end);
        diagnostic_trace(marker);
    }
    if (cafe_program_info.compiled_entries == 0) {
        diagnostic_trace("dynamic::initialize: begin");
        dynamic::initialize(*rpx);
        diagnostic_trace("dynamic::initialize: complete");
    }

    diagnostic_trace("build_dispatch_table: begin");
    build_dispatch_table();
    diagnostic_trace("build_dispatch_table: complete");

    diagnostic_trace("os::init_heaps: begin");
    os::init_heaps(image.data_end);
    diagnostic_trace("os::init_heaps: complete");

    diagnostic_trace("data imports: begin");
    if (dynamic::enabled()) {
        dynamic::initialize_data_imports();
    } else {
        init_data_imports();
    }
    diagnostic_trace("data imports: complete");

    diagnostic_trace("title::prepare_presentation: begin");
    title::prepare_presentation();
    diagnostic_trace("title::prepare_presentation: complete");
    if (dynamic::enabled()) {
        std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, dynamic zero-install execution enabled\n",
                     image.entry_point);
    } else {
        std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                     image.entry_point, cafe_program_info.compiled_entries,
                     cafe_program_info.total_entries);
    }

    // argv[0] is the executable name, as the Cafe loader passes it.
    diagnostic_trace("guest argv: begin");
    const uint32_t guest_argv = system_alloc(8);
    *reinterpret_cast<uint32_t*>(guest_pointer(guest_argv)) = __builtin_bswap32(guest_strdup("Tekken.rpx"));
    diagnostic_trace("guest argv: complete");

    const auto run = [&] {
        diagnostic_trace("os::run_main_thread: entered");
        const int code =
            os::run_main_thread(image.entry_point, 1, guest_argv, image.stack_size,
                                image.sda_base, image.sda2_base);
        char marker[96]{};
        std::snprintf(marker, sizeof(marker), "os::run_main_thread: returned %d", code);
        diagnostic_trace(marker);
        return code;
    };
    // The window's event loop needs the process's main thread; the title
    // runs beside it. TTT2_GPU=null runs headless.
    const char* gpu = std::getenv("TTT2_GPU");
    diagnostic_trace("host::open_window: begin");
    const bool headless =
        (gpu && std::strcmp(gpu, "null") == 0) ||
        !host::open_window("Tekken Tag Tournament 2");
    diagnostic_trace(headless ? "host::open_window: headless/failed" :
                                "host::open_window: complete");
    if (!headless) {
        std::vector<uint8_t> icon;
        uint32_t width = 0, height = 0;
        if (const auto tga = vfs::read_whole("/vol/meta/iconTex.tga"); tga && install::decode_tga(*tga, icon, width, height)) {
            host::set_window_icon(icon, width, height);
        }
    }
    diagnostic_trace("host::open_audio: begin");
    host::open_audio();
    diagnostic_trace("host::open_audio: complete");

    if (headless) {
        diagnostic_trace("headless: starting guest directly");
        return run();
    }

    std::atomic<bool> finished{false};
    int result = 0;
    diagnostic_trace("title thread: create");
    std::thread title([&] {
        diagnostic_trace("title thread: entered");
        result = run();
        finished = true;
        diagnostic_trace("title thread: finished");
    });

    diagnostic_trace("host::run_event_loop: begin");
    if (!host::run_event_loop([&] { return finished.load(); })) {
        diagnostic_trace("host::run_event_loop: window closed");
        std::_Exit(0);
    }
    diagnostic_trace("host::run_event_loop: complete");
    title.join();
    diagnostic_trace("runtime: clean exit");
    return result;
}
