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
    FILE* redirected = nullptr;
    bool stdout_redirected = false;
    if (_wfreopen_s(&redirected, file.c_str(), L"w", stderr) == 0 && redirected) {
        // CREATE_NO_WINDOW/GUI launches can leave stdout without a valid CRT
        // descriptor. Calling _dup2 with that negative descriptor invokes the
        // MSVC invalid-parameter handler, which terminates the process with
        // 0xC0000409 before the first log line is written.
        const int stderr_fd = _fileno(stderr);
        const int stdout_fd = _fileno(stdout);
        if (stderr_fd >= 0 && stdout_fd >= 0) {
            stdout_redirected = (_dup2(stderr_fd, stdout_fd) == 0);
        } else {
            // Give printf-style diagnostics somewhere to go even when the
            // launcher created the process without console handles.
            FILE* redirected_stdout = nullptr;
            stdout_redirected =
                (_wfreopen_s(&redirected_stdout, file.c_str(), L"a", stdout) == 0 &&
                 redirected_stdout != nullptr);
        }
    }
#else
    bool stdout_redirected = false;
    if (std::freopen(file.c_str(), "w", stderr)) {
        stdout_redirected = (::dup2(fileno(stderr), fileno(stdout)) >= 0);
    }
#endif
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    if (stdout_redirected) std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
    redirect_runtime_log(game);
    install_fault_handler();
    vfs::open_game(game);
    if (const char* save = std::getenv("TTT2_SAVE_DIR")) {
        vfs::set_save_root(save);
    } else {
        vfs::set_save_root(host::data_directory() / "ttt2" / "save");
    }
    const auto rpx = vfs::read_whole("/vol/code/Tekken.rpx");
    if (!rpx) fatal("the game source has no code/Tekken.rpx");

    if (verify_only) {
        const std::string digest = sha256_hex(*rpx);
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
        return 0;
    }

    reserve_guest_memory();
    init_system_heap();
    const LoadedImage image = load_image(*rpx);
    if (cafe_program_info.compiled_entries == 0) {
        dynamic::initialize(*rpx);
    }
    build_dispatch_table();
    os::init_heaps(image.data_end);
    if (dynamic::enabled()) {
        dynamic::initialize_data_imports();
    } else {
        init_data_imports();
    }
    title::prepare_presentation();
    if (dynamic::enabled()) {
        std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, dynamic zero-install execution enabled\n",
                     image.entry_point);
    } else {
        std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                     image.entry_point, cafe_program_info.compiled_entries,
                     cafe_program_info.total_entries);
    }

    // argv[0] is the executable name, as the Cafe loader passes it.
    const uint32_t guest_argv = system_alloc(8);
    *reinterpret_cast<uint32_t*>(guest_pointer(guest_argv)) = __builtin_bswap32(guest_strdup("Tekken.rpx"));
    const auto run = [&] {
        return os::run_main_thread(image.entry_point, 1, guest_argv, image.stack_size, image.sda_base,
                                   image.sda2_base);
    };
    // The window's event loop needs the process's main thread; the title
    // runs beside it. TTT2_GPU=null runs headless.
    const char* gpu = std::getenv("TTT2_GPU");
    const bool headless = (gpu && std::strcmp(gpu, "null") == 0) || !host::open_window("Tekken Tag Tournament 2");
    if (!headless) {
        std::vector<uint8_t> icon;
        uint32_t width = 0, height = 0;
        if (const auto tga = vfs::read_whole("/vol/meta/iconTex.tga"); tga && install::decode_tga(*tga, icon, width, height)) {
            host::set_window_icon(icon, width, height);
        }
    }
    host::open_audio();
    if (headless) return run();
    std::atomic<bool> finished{false};
    int result = 0;
    std::thread title([&] {
        result = run();
        finished = true;
    });
    if (!host::run_event_loop([&] { return finished.load(); })) std::_Exit(0); // window closed
    title.join();
    return result;
}
