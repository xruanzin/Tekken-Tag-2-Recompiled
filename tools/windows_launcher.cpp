#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path module_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(buffer);
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
    return text;
}

bool valid_game(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return fs::is_regular_file(path / L"code" / L"Tekken.rpx", ec);
    }
    return fs::is_regular_file(path, ec) && lower(path.extension().wstring()) == L".wua";
}

std::optional<fs::path> pick_folder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Selecione a pasta extraida de Tekken Tag Tournament 2");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                result = fs::path(value);
                CoTaskMemFree(value);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<fs::path> pick_wua() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Wii U archive (.wua)", L"*.wua"},
        {L"Todos os arquivos", L"*.*"},
    };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"Selecione o arquivo .wua de Tekken Tag Tournament 2");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                result = fs::path(value);
                CoTaskMemFree(value);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<fs::path> choose_game() {
    const int choice = MessageBoxW(
        nullptr,
        L"Escolha a origem do jogo.\n\n"
        L"SIM: pasta extraida (code/content/meta)\n"
        L"NAO: arquivo .wua\n"
        L"CANCELAR: sair",
        L"TTT2 Launcher",
        MB_ICONQUESTION | MB_YESNOCANCEL | MB_SETFOREGROUND);
    if (choice == IDYES) return pick_folder();
    if (choice == IDNO) return pick_wua();
    return std::nullopt;
}

fs::path game_from_config(const fs::path& config) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD n = GetPrivateProfileStringW(L"Launcher", L"GamePath", L"", buffer.data(),
                                             static_cast<DWORD>(buffer.size()), config.c_str());
    if (n == 0) return {};
    return fs::path(std::wstring(buffer.data(), n));
}

bool save_game(const fs::path& config, const fs::path& game) {
    return WritePrivateProfileStringW(L"Launcher", L"GamePath", game.c_str(), config.c_str()) != FALSE;
}

fs::path find_game_executable(const fs::path& root) {
    const fs::path built = root / L"build" / L"windows" / L"ttt2.exe";
    const fs::path beside = root / L"ttt2.exe";
    std::error_code ec;
    if (fs::is_regular_file(built, ec)) return built;
    if (fs::is_regular_file(beside, ec)) return beside;
    return {};
}

bool launch(const fs::path& executable, const fs::path& game) {
    std::wstring command = L"\"" + executable.wstring() + L"\" \"" + game.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working = executable.parent_path().wstring();
    const BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, working.c_str(), &startup, &process);
    if (!ok) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const fs::path self = module_path();
    const fs::path root = self.empty() ? fs::current_path() : self.parent_path();
    const fs::path config = root / L"TTT2 Launcher.ini";
    const fs::path executable = find_game_executable(root);

    if (executable.empty()) {
        MessageBoxW(nullptr,
                    L"Nao encontrei build\\windows\\ttt2.exe.\n"
                    L"Compile o projeto primeiro com build-windows.ps1.",
                    L"TTT2 Launcher", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    fs::path game = game_from_config(config);
    const bool choose_again = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (choose_again || !valid_game(game)) {
        const auto chosen = choose_game();
        if (!chosen) {
            CoUninitialize();
            return 0;
        }
        game = *chosen;
        if (!valid_game(game)) {
            MessageBoxW(nullptr,
                        L"A origem selecionada nao contem code\\Tekken.rpx e tambem nao e um arquivo .wua valido.",
                        L"TTT2 Launcher", MB_ICONERROR | MB_OK);
            CoUninitialize();
            return 2;
        }
        save_game(config, game);
    }

    if (!launch(executable, game)) {
        const DWORD error = GetLastError();
        std::wstring message = L"Nao foi possivel iniciar ttt2.exe.\nErro do Windows: " + std::to_wstring(error);
        MessageBoxW(nullptr, message.c_str(), L"TTT2 Launcher", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 3;
    }

    CoUninitialize();
    return 0;
}
#endif
