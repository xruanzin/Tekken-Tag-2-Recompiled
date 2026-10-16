#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr COLORREF kBg = RGB(5, 7, 11);
constexpr COLORREF kSidebar = RGB(8, 10, 15);
constexpr COLORREF kPanel = RGB(14, 17, 23);
constexpr COLORREF kPanel2 = RGB(24, 28, 36);
constexpr COLORREF kText = RGB(244, 247, 250);
constexpr COLORREF kMuted = RGB(154, 166, 182);
constexpr COLORREF kRed = RGB(205, 24, 42);
constexpr COLORREF kRedDark = RGB(108, 9, 22);
constexpr COLORREF kBlue = RGB(43, 147, 232);
constexpr COLORREF kBlueDark = RGB(16, 69, 120);
constexpr COLORREF kIce = RGB(166, 217, 255);
constexpr COLORREF kSilver = RGB(195, 203, 216);
constexpr COLORREF kBorder = RGB(61, 72, 88);

constexpr int kSidebarWidth = 220;
constexpr int kContentX = 244;
constexpr int kContentY = 102;
constexpr int kContentW = 820;
constexpr int kContentH = 525;

enum Page { kHome, kDisplay, kGraphics, kGame, kLanguage, kPageCount };

enum ControlId {
    ID_NAV_HOME = 100,
    ID_NAV_DISPLAY,
    ID_NAV_GRAPHICS,
    ID_NAV_GAME,
    ID_NAV_LANGUAGE,
    ID_GAME_PATH,
    ID_BROWSE,
    ID_PRESET,
    ID_PLAY,
    ID_SAVE,
    ID_RESET,
    ID_DISPLAY_MODE,
    ID_OUTPUT_RES,
    ID_FPS,
    ID_FILL_SCREEN,
    ID_RENDER_SCALE,
    ID_AA,
    ID_UPSCALER,
    ID_ANISO,
    ID_SHARPEN,
    ID_ASYNC_SHADERS,
    ID_GAME_LANGUAGE,
    ID_CONTROLLER,
    ID_AUDIO,
    ID_VOLUME,
    ID_UI_LANGUAGE,
};

enum TextKey {
    T_SUBTITLE,
    T_TAGLINE,
    T_HOME,
    T_DISPLAY,
    T_GRAPHICS,
    T_GAME,
    T_LANGUAGE,
    T_HOME_TITLE,
    T_HOME_DESC,
    T_GAME_SOURCE,
    T_BROWSE,
    T_PRESET,
    T_PLAY,
    T_SAVE,
    T_RESET,
    T_DISPLAY_TITLE,
    T_DISPLAY_MODE,
    T_OUTPUT_RES,
    T_FRAME_RATE,
    T_FILL_SCREEN,
    T_GRAPHICS_TITLE,
    T_INTERNAL_RES,
    T_AA,
    T_UPSCALER,
    T_ANISO,
    T_SHARPNESS,
    T_UPSCALE_NOTE,
    T_GAME_TITLE,
    T_GAME_LANGUAGE,
    T_CONTROLLER,
    T_AUDIO,
    T_VOLUME,
    T_LANGUAGE_TITLE,
    T_LAUNCHER_LANGUAGE,
    T_LANGUAGE_DESC,
    T_BAD_GAME,
    T_UNSUPPORTED_UPSCALER,
    T_COUNT
};

using LangTable = std::array<const wchar_t*, T_COUNT>;

const std::array<LangTable, 7> kLanguages = {{
    // Português (Brasil)
    {L"PORT NATIVO PARA WINDOWS", L"GET READY FOR THE NEXT BATTLE",
     L"Início", L"Tela", L"Gráficos", L"Jogo", L"Idioma",
     L"Tekken Tag Tournament 2", L"Configure o port e entre direto na luta.",
     L"Origem do jogo", L"Procurar", L"Perfil rápido", L"JOGAR", L"Salvar", L"Restaurar padrões",
     L"Tela e apresentação", L"Modo de exibição", L"Resolução de saída", L"FPS de apresentação",
     L"Preencher a tela", L"Qualidade gráfica", L"Resolução interna", L"Antialiasing",
     L"Upscaling / filtro", L"Filtro anisotrópico", L"Nitidez",
     L"FSR 1 EASU está integrado ao Vulkan. DLSS e XeSS ficam bloqueados até existirem backends específicos.",
     L"Jogo e controles", L"Idioma do jogo", L"Ícones do controle", L"Áudio", L"Volume",
     L"Idioma do launcher", L"Idioma da interface", L"A alteração da interface é aplicada imediatamente e salva no launcher.",
     L"Selecione uma pasta com code\\Tekken.rpx ou um arquivo .wua válido.",
     L"Esse upscaler ainda não está integrado ao renderer deste port."},
    // English
    {L"NATIVE WINDOWS PORT", L"GET READY FOR THE NEXT BATTLE",
     L"Home", L"Display", L"Graphics", L"Game", L"Language",
     L"Tekken Tag Tournament 2", L"Configure the port and jump straight into battle.",
     L"Game source", L"Browse", L"Quick preset", L"PLAY", L"Save", L"Restore defaults",
     L"Display and presentation", L"Display mode", L"Output resolution", L"Presentation FPS",
     L"Fill the screen", L"Graphics quality", L"Internal resolution", L"Anti-aliasing",
     L"Upscaling / filter", L"Anisotropic filtering", L"Sharpness",
     L"FSR 1 EASU is integrated into Vulkan. DLSS and XeSS remain locked until dedicated backends exist.",
     L"Game and controls", L"Game language", L"Controller icons", L"Audio", L"Volume",
     L"Launcher language", L"Interface language", L"Interface language changes are applied immediately and saved.",
     L"Select a folder containing code\\Tekken.rpx or a valid .wua file.",
     L"This upscaler is not integrated into this port's renderer yet."},
    // Español
    {L"PORT NATIVO PARA WINDOWS", L"GET READY FOR THE NEXT BATTLE",
     L"Inicio", L"Pantalla", L"Gráficos", L"Juego", L"Idioma",
     L"Tekken Tag Tournament 2", L"Configura el port y entra directamente al combate.",
     L"Origen del juego", L"Buscar", L"Perfil rápido", L"JUGAR", L"Guardar", L"Restablecer",
     L"Pantalla y presentación", L"Modo de pantalla", L"Resolución de salida", L"FPS de presentación",
     L"Llenar la pantalla", L"Calidad gráfica", L"Resolución interna", L"Antialiasing",
     L"Escalado / filtro", L"Filtrado anisotrópico", L"Nitidez",
     L"FSR 1 EASU está integrado en Vulkan. DLSS y XeSS permanecen bloqueados hasta tener backends dedicados.",
     L"Juego y controles", L"Idioma del juego", L"Iconos del mando", L"Audio", L"Volumen",
     L"Idioma del launcher", L"Idioma de la interfaz", L"El idioma se aplica inmediatamente y se guarda.",
     L"Selecciona una carpeta con code\\Tekken.rpx o un archivo .wua válido.",
     L"Este escalador todavía no está integrado en el renderer."},
    // Français
    {L"PORT NATIF WINDOWS", L"GET READY FOR THE NEXT BATTLE",
     L"Accueil", L"Affichage", L"Graphismes", L"Jeu", L"Langue",
     L"Tekken Tag Tournament 2", L"Configurez le port et entrez directement dans le combat.",
     L"Source du jeu", L"Parcourir", L"Préréglage", L"JOUER", L"Enregistrer", L"Réinitialiser",
     L"Affichage et présentation", L"Mode d'affichage", L"Résolution de sortie", L"FPS de présentation",
     L"Remplir l'écran", L"Qualité graphique", L"Résolution interne", L"Anticrénelage",
     L"Mise à l'échelle / filtre", L"Filtrage anisotrope", L"Netteté",
     L"FSR 1 EASU est intégré à Vulkan. DLSS et XeSS restent verrouillés sans backends dédiés.",
     L"Jeu et contrôles", L"Langue du jeu", L"Icônes de manette", L"Audio", L"Volume",
     L"Langue du launcher", L"Langue de l'interface", L"Le changement de langue est immédiat et enregistré.",
     L"Sélectionnez un dossier contenant code\\Tekken.rpx ou un fichier .wua valide.",
     L"Cet upscaler n'est pas encore intégré au renderer."},
    // Deutsch
    {L"NATIVER WINDOWS-PORT", L"GET READY FOR THE NEXT BATTLE",
     L"Start", L"Anzeige", L"Grafik", L"Spiel", L"Sprache",
     L"Tekken Tag Tournament 2", L"Port konfigurieren und direkt in den Kampf starten.",
     L"Spielquelle", L"Durchsuchen", L"Schnellprofil", L"SPIELEN", L"Speichern", L"Standardwerte",
     L"Anzeige und Präsentation", L"Anzeigemodus", L"Ausgabeauflösung", L"Präsentations-FPS",
     L"Bildschirm ausfüllen", L"Grafikqualität", L"Interne Auflösung", L"Kantenglättung",
     L"Upscaling / Filter", L"Anisotrope Filterung", L"Schärfe",
     L"FSR 1 EASU ist in Vulkan integriert. DLSS und XeSS bleiben ohne eigene Backends gesperrt.",
     L"Spiel und Steuerung", L"Spielsprache", L"Controller-Symbole", L"Audio", L"Lautstärke",
     L"Launcher-Sprache", L"Sprache der Oberfläche", L"Die Sprache wird sofort angewendet und gespeichert.",
     L"Wähle einen Ordner mit code\\Tekken.rpx oder eine gültige .wua-Datei.",
     L"Dieser Upscaler ist noch nicht in den Renderer integriert."},
    // Italiano
    {L"PORT NATIVO WINDOWS", L"GET READY FOR THE NEXT BATTLE",
     L"Home", L"Schermo", L"Grafica", L"Gioco", L"Lingua",
     L"Tekken Tag Tournament 2", L"Configura il port ed entra subito nel combattimento.",
     L"Origine del gioco", L"Sfoglia", L"Profilo rapido", L"GIOCA", L"Salva", L"Ripristina",
     L"Schermo e presentazione", L"Modalità schermo", L"Risoluzione di uscita", L"FPS di presentazione",
     L"Riempi lo schermo", L"Qualità grafica", L"Risoluzione interna", L"Antialiasing",
     L"Upscaling / filtro", L"Filtro anisotropico", L"Nitidezza",
     L"FSR 1 EASU è integrato in Vulkan. DLSS e XeSS restano bloccati finché non esistono backend dedicati.",
     L"Gioco e controlli", L"Lingua del gioco", L"Icone controller", L"Audio", L"Volume",
     L"Lingua del launcher", L"Lingua interfaccia", L"La lingua viene applicata subito e salvata.",
     L"Seleziona una cartella con code\\Tekken.rpx o un file .wua valido.",
     L"Questo upscaler non è ancora integrato nel renderer."},
    // 日本語
    {L"WINDOWS ネイティブポート", L"GET READY FOR THE NEXT BATTLE",
     L"ホーム", L"画面", L"グラフィック", L"ゲーム", L"言語",
     L"Tekken Tag Tournament 2", L"ポートを設定してすぐにバトルへ。",
     L"ゲームの場所", L"参照", L"クイック設定", L"プレイ", L"保存", L"初期設定",
     L"画面と表示", L"表示モード", L"出力解像度", L"表示 FPS",
     L"画面いっぱいに表示", L"グラフィック品質", L"内部解像度", L"アンチエイリアス",
     L"アップスケール / フィルター", L"異方性フィルタリング", L"シャープネス",
     L"FSR 1 EASU は Vulkan に統合済みです。DLSS / XeSS は専用バックエンド実装まで無効です。",
     L"ゲームとコントローラー", L"ゲーム言語", L"ボタン表示", L"オーディオ", L"音量",
     L"ランチャー言語", L"インターフェイス言語", L"言語変更はすぐに適用され保存されます。",
     L"code\\Tekken.rpx を含むフォルダー、または有効な .wua を選択してください。",
     L"このアップスケーラーはまだレンダラーに統合されていません。"}
}};

struct Config {
    fs::path game;
    int ui_language = 0;
    int preset = 2;          // custom, performance, balanced, quality, ultra
    int display_mode = 0;    // windowed/fullscreen
    int output_res = 0;      // auto + presets
    int fps = 1;             // auto,60,120,180,240
    bool fill_screen = false;
    int render_scale = 1;    // auto,1x..4x
    int aa = 1;              // off/fxaa
    int upscaler = 3;        // linear/nearest/sharp/FSR1/(locked...)
    int aniso = 2;           // default/2/4/8/16
    int sharpness = 25;      // 0..100
    int game_language = 0;   // list index
    int controller = 0;      // auto/xbox/playstation/nintendo
    bool audio = true;
    int volume = 100;
    bool reduce_input_lag = true;
    bool async_shaders = true;
};

HINSTANCE g_instance = nullptr;
ULONG_PTR g_gdiplus_token = 0;
std::unique_ptr<Gdiplus::Image> g_theme_image;
IStream* g_theme_stream = nullptr;
HWND g_main = nullptr;
HWND g_home_art = nullptr;
std::array<HWND, kPageCount> g_pages{};
std::array<HWND, kPageCount> g_nav{};
Page g_page = kHome;
Config g_cfg;
fs::path g_root, g_ini, g_exe;

HFONT g_font = nullptr, g_small = nullptr, g_title = nullptr, g_nav_font = nullptr;
HBRUSH g_bg_brush = nullptr, g_panel_brush = nullptr, g_edit_brush = nullptr;

HWND g_game_edit = nullptr, g_preset = nullptr;
HWND g_display_mode = nullptr, g_output_res = nullptr, g_fps = nullptr, g_fill = nullptr;
HWND g_render_scale = nullptr, g_aa = nullptr, g_upscaler = nullptr, g_aniso = nullptr;
HWND g_sharpen = nullptr, g_sharpen_value = nullptr, g_async_shaders = nullptr;
HWND g_game_language = nullptr, g_controller = nullptr, g_audio = nullptr, g_volume = nullptr, g_volume_value = nullptr;
HWND g_reduce_input_lag = nullptr;
HWND g_ui_language = nullptr;

const wchar_t* tr(TextKey key) {
    const int lang = std::clamp(g_cfg.ui_language, 0, static_cast<int>(kLanguages.size()) - 1);
    return kLanguages[lang][key];
}

const wchar_t* reduce_input_lag_text() {
    static constexpr const wchar_t* labels[] = {
        L"Reduzir input lag (remove 2 frames de atraso forçado)",
        L"Reduce input lag (remove 2 frames of forced delay)",
        L"Reducir input lag (elimina 2 fotogramas de retraso forzado)",
        L"Réduire l'input lag (supprime 2 images de délai forcé)",
        L"Input-Lag reduzieren (2 erzwungene Frames entfernen)",
        L"Riduci input lag (rimuove 2 frame di ritardo forzato)",
        L"入力遅延を軽減（強制 2 フレーム遅延を解除）"
    };
    return labels[std::clamp(g_cfg.ui_language, 0, 6)];
}

const wchar_t* async_shaders_text() {
    static constexpr const wchar_t* labels[] = {
        L"Compilação assíncrona de shaders/pipelines (reduz travadas)",
        L"Asynchronous shader/pipeline compilation (reduces stutter)",
        L"Compilación asíncrona de shaders/pipelines (reduce tirones)",
        L"Compilation asynchrone shaders/pipelines (réduit les saccades)",
        L"Asynchrone Shader/Pipeline-Kompilierung (weniger Ruckler)",
        L"Compilazione shader/pipeline asincrona (riduce gli scatti)",
        L"非同期シェーダー/パイプラインコンパイル"
    };
    return labels[std::clamp(g_cfg.ui_language, 0, 6)];
}

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


fs::path find_theme_image() {
    const std::array<fs::path, 3> candidates = {
        g_root / L"assets" / L"ttt2_launcher_theme.jpg",
        g_root.parent_path() / L"assets" / L"ttt2_launcher_theme.jpg",
        g_root.parent_path().parent_path() / L"assets" / L"ttt2_launcher_theme.jpg",
    };
    std::error_code ec;
    for (const fs::path& path : candidates) {
        if (fs::is_regular_file(path, ec)) return path;
        ec.clear();
    }
    return {};
}

void load_theme_image() {
    HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (resource) {
        const DWORD size = SizeofResource(g_instance, resource);
        HGLOBAL loaded = LoadResource(g_instance, resource);
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        if (bytes && size != 0) {
            HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
            if (copy) {
                void* dst = GlobalLock(copy);
                if (dst) {
                    std::memcpy(dst, bytes, size);
                    GlobalUnlock(copy);
                    if (SUCCEEDED(CreateStreamOnHGlobal(copy, TRUE, &g_theme_stream))) {
                        std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromStream(g_theme_stream, FALSE));
                        if (image && image->GetLastStatus() == Gdiplus::Ok) {
                            g_theme_image = std::move(image);
                            return;
                        }
                        g_theme_stream->Release();
                        g_theme_stream = nullptr;
                    } else {
                        GlobalFree(copy);
                    }
                } else {
                    GlobalFree(copy);
                }
            }
        }
    }

    const fs::path path = find_theme_image();
    if (path.empty()) return;
    std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromFile(path.c_str(), FALSE));
    if (image && image->GetLastStatus() == Gdiplus::Ok) g_theme_image = std::move(image);
}

void draw_theme_cover(Gdiplus::Graphics& graphics, const Gdiplus::Rect& dst, float brightness) {
    if (!g_theme_image || dst.Width <= 0 || dst.Height <= 0) return;
    const UINT iw = g_theme_image->GetWidth();
    const UINT ih = g_theme_image->GetHeight();
    if (!iw || !ih) return;

    const double src_aspect = static_cast<double>(iw) / ih;
    const double dst_aspect = static_cast<double>(dst.Width) / dst.Height;
    INT sx = 0, sy = 0, sw = static_cast<INT>(iw), sh = static_cast<INT>(ih);
    if (src_aspect > dst_aspect) {
        sw = static_cast<INT>(ih * dst_aspect);
        sx = (static_cast<INT>(iw) - sw) / 2;
    } else {
        sh = static_cast<INT>(iw / dst_aspect);
        sy = (static_cast<INT>(ih) - sh) / 2;
    }

    Gdiplus::ColorMatrix matrix = {{
        {brightness, 0, 0, 0, 0},
        {0, brightness, 0, 0, 0},
        {0, 0, brightness, 0, 0},
        {0, 0, 0, 1.0f, 0},
        {0, 0, 0, 0, 1.0f},
    }};
    Gdiplus::ImageAttributes attrs;
    attrs.SetColorMatrix(&matrix);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(g_theme_image.get(), dst, sx, sy, sw, sh, Gdiplus::UnitPixel, &attrs);
}

void paint_theme_surface(HDC dc, const RECT& rect, bool page, bool home = false) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    const Gdiplus::Rect full(static_cast<INT>(rect.left), static_cast<INT>(rect.top),
                             static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::SolidBrush base(Gdiplus::Color(255, 7, 9, 14));
    graphics.FillRectangle(&base, full);

    if (!page) {
        // Main shell: artwork is atmospheric, not the focal point.
        draw_theme_cover(graphics, full, 0.48f);
        Gdiplus::SolidBrush veil(Gdiplus::Color(150, 4, 7, 12));
        graphics.FillRectangle(&veil, full);
    } else if (home) {
        Gdiplus::LinearGradientBrush home_wash(
            Gdiplus::Point(0, 0), Gdiplus::Point(width, height),
            Gdiplus::Color(28, 94, 8, 19), Gdiplus::Color(24, 8, 45, 79));
        graphics.FillRectangle(&home_wash, full);
    } else {
        // Settings pages stay clean and dark so controls are easy to scan.
        Gdiplus::LinearGradientBrush panel_wash(
            Gdiplus::Point(0, 0), Gdiplus::Point(width, height),
            Gdiplus::Color(34, 112, 9, 22), Gdiplus::Color(32, 10, 61, 105));
        graphics.FillRectangle(&panel_wash, full);
    }

    Gdiplus::SolidBrush red_line(Gdiplus::Color(230, 213, 25, 43));
    Gdiplus::SolidBrush blue_line(Gdiplus::Color(225, 53, 160, 244));
    graphics.FillRectangle(&red_line, Gdiplus::Rect(rect.left, rect.top, width / 2, 3));
    graphics.FillRectangle(&blue_line, Gdiplus::Rect(rect.left + width / 2, rect.top, width - width / 2, 3));
}

bool valid_game(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return fs::is_regular_file(path / L"code" / L"Tekken.rpx", ec);
    return fs::is_regular_file(path, ec) && lower(path.extension().wstring()) == L".wua";
}

int ini_int(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini.c_str()));
}

std::wstring ini_string(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    std::vector<wchar_t> buffer(32768, L'\0');
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), g_ini.c_str());
    return buffer.data();
}

void put_int(const wchar_t* section, const wchar_t* key, int value) {
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(section, key, text.c_str(), g_ini.c_str());
}
void put_string(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), g_ini.c_str());
}

void defaults(bool keep_game = true, bool keep_ui_language = true) {
    const fs::path game = g_cfg.game;
    const int ui = g_cfg.ui_language;
    g_cfg = Config{};
    if (keep_game) g_cfg.game = game;
    if (keep_ui_language) g_cfg.ui_language = ui;
}

void load_config() {
    g_cfg.game = ini_string(L"Launcher", L"GamePath");
    g_cfg.ui_language = std::clamp(ini_int(L"Launcher", L"UILanguage", 0), 0, 6);
    g_cfg.preset = std::clamp(ini_int(L"Graphics", L"Preset", 2), 0, 4);
    g_cfg.display_mode = std::clamp(ini_int(L"Display", L"Mode", 0), 0, 1);
    g_cfg.output_res = std::clamp(ini_int(L"Display", L"OutputResolution", 0), 0, 5);
    g_cfg.fps = std::clamp(ini_int(L"Display", L"FPS", 1), 0, 4);
    g_cfg.fill_screen = ini_int(L"Display", L"FillScreen", 0) != 0;
    g_cfg.render_scale = std::clamp(ini_int(L"Graphics", L"RenderScale", 1), 0, 4);
    g_cfg.aa = std::clamp(ini_int(L"Graphics", L"AA", 1), 0, 1);
    g_cfg.upscaler = std::clamp(ini_int(L"Graphics", L"Upscaler", 3), 0, 3);
    g_cfg.aniso = std::clamp(ini_int(L"Graphics", L"Anisotropy", 2), 0, 4);
    g_cfg.sharpness = std::clamp(ini_int(L"Graphics", L"Sharpness", 25), 0, 100);
    g_cfg.game_language = std::clamp(ini_int(L"Game", L"Language", 0), 0, 8);
    g_cfg.controller = std::clamp(ini_int(L"Game", L"Controller", 0), 0, 3);
    g_cfg.audio = ini_int(L"Game", L"Audio", 1) != 0;
    g_cfg.volume = std::clamp(ini_int(L"Game", L"Volume", 100), 0, 100);
    g_cfg.reduce_input_lag = ini_int(L"Game", L"ReduceInputLag", 1) != 0;
    g_cfg.async_shaders = ini_int(L"Graphics", L"AsyncShaders", 1) != 0;
}

void save_config() {
    put_string(L"Launcher", L"GamePath", g_cfg.game.wstring());
    put_int(L"Launcher", L"UILanguage", g_cfg.ui_language);
    put_int(L"Graphics", L"Preset", g_cfg.preset);
    put_int(L"Display", L"Mode", g_cfg.display_mode);
    put_int(L"Display", L"OutputResolution", g_cfg.output_res);
    put_int(L"Display", L"FPS", g_cfg.fps);
    put_int(L"Display", L"FillScreen", g_cfg.fill_screen ? 1 : 0);
    put_int(L"Graphics", L"RenderScale", g_cfg.render_scale);
    put_int(L"Graphics", L"AA", g_cfg.aa);
    put_int(L"Graphics", L"Upscaler", g_cfg.upscaler);
    put_int(L"Graphics", L"Anisotropy", g_cfg.aniso);
    put_int(L"Graphics", L"Sharpness", g_cfg.sharpness);
    put_int(L"Game", L"Language", g_cfg.game_language);
    put_int(L"Game", L"Controller", g_cfg.controller);
    put_int(L"Game", L"Audio", g_cfg.audio ? 1 : 0);
    put_int(L"Game", L"Volume", g_cfg.volume);
    put_int(L"Game", L"ReduceInputLag", g_cfg.reduce_input_lag ? 1 : 0);
    put_int(L"Graphics", L"AsyncShaders", g_cfg.async_shaders ? 1 : 0);
}

fs::path find_game_executable() {
    const std::array<fs::path, 2> candidates = {g_root / L"build" / L"windows" / L"ttt2.exe", g_root / L"ttt2.exe"};
    std::error_code ec;
    for (const auto& path : candidates) if (fs::is_regular_file(path, ec)) return path;
    return {};
}

std::optional<fs::path> pick_folder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Tekken Tag Tournament 2");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(g_main))) {
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
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    static const COMDLG_FILTERSPEC filters[] = {{L"Wii U archive (.wua)", L"*.wua"}, {L"All files", L"*.*"}};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetTitle(L"Tekken Tag Tournament 2");
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(g_main))) {
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
    const int choice = MessageBoxW(g_main,
        L"YES: extracted code/content/meta folder\nNO: .wua archive\nCANCEL: keep current selection",
        L"Tekken Tag Tournament 2", MB_ICONQUESTION | MB_YESNOCANCEL);
    if (choice == IDYES) return pick_folder();
    if (choice == IDNO) return pick_wua();
    return std::nullopt;
}

void set_env(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

void apply_environment() {
    // Internal rendering.
    if (g_cfg.render_scale == 0) set_env(L"TTT2_SCALE", L"");
    else set_env(L"TTT2_SCALE", std::to_wstring(g_cfg.render_scale));

    // Output resolution.
    static constexpr int widths[] = {0, 1280, 1600, 1920, 2560, 3840};
    static constexpr int heights[] = {0, 720, 900, 1080, 1440, 2160};
    if (g_cfg.output_res == 0) {
        set_env(L"TTT2_WINDOW_WIDTH", L"");
        set_env(L"TTT2_WINDOW_HEIGHT", L"");
    } else {
        set_env(L"TTT2_WINDOW_WIDTH", std::to_wstring(widths[g_cfg.output_res]));
        set_env(L"TTT2_WINDOW_HEIGHT", std::to_wstring(heights[g_cfg.output_res]));
    }

    set_env(L"TTT2_FULLSCREEN", g_cfg.display_mode == 1 ? L"1" : L"");
    set_env(L"TTT2_STRETCH", g_cfg.fill_screen ? L"1" : L"");

    static constexpr int fps_values[] = {0, 60, 120, 180, 240};
    set_env(L"TTT2_FPS", g_cfg.fps == 0 ? L"" : std::to_wstring(fps_values[g_cfg.fps]));

    set_env(L"TTT2_AA", g_cfg.aa == 1 ? L"fxaa" : L"");
    static constexpr const wchar_t* upscale[] = {L"linear", L"nearest", L"sharp", L"fsr1"};
    set_env(L"TTT2_UPSCALER", upscale[std::clamp(g_cfg.upscaler, 0, 3)]);
    wchar_t sharp[32]{};
    std::swprintf(sharp, std::size(sharp), L"%.2f", g_cfg.sharpness / 100.0);
    set_env(L"TTT2_SHARPEN", sharp);

    static constexpr int aniso[] = {0, 2, 4, 8, 16};
    set_env(L"TTT2_ANISO", g_cfg.aniso == 0 ? L"" : std::to_wstring(aniso[g_cfg.aniso]));

    static constexpr int game_languages[] = {1, 2, 3, 4, 5, 8, 9, 10, 0};
    set_env(L"TTT2_LANGUAGE", std::to_wstring(game_languages[g_cfg.game_language]));

    static constexpr const wchar_t* controllers[] = {L"", L"xbox", L"playstation", L"nintendo"};
    set_env(L"TTT2_CONTROLLER", controllers[g_cfg.controller]);

    set_env(L"TTT2_AUDIO", g_cfg.audio ? L"" : L"0");
    set_env(L"TTT2_AUDIO_VOLUME", std::to_wstring(g_cfg.volume));
    set_env(L"TTT2_REDUCE_INPUT_LAG", g_cfg.reduce_input_lag ? L"1" : L"");
    set_env(L"TTT2_ASYNC_SHADERS", g_cfg.async_shaders ? L"1" : L"0");
    set_env(L"TTT2_ASYNC_PIPELINES", L"1");
    set_env(L"TTT2_FAST_PIPELINES", L"1");
    set_env(L"TTT2_TEXTURE_CACHE", L"1");
    set_env(L"TTT2_NATIVE_EFFECT_BUFFERS", g_cfg.render_scale >= 2 ? L"1" : L"0");
}

bool launch_game() {
    if (!valid_game(g_cfg.game)) {
        MessageBoxW(g_main, tr(T_BAD_GAME), L"Tekken Tag Tournament 2", MB_ICONWARNING | MB_OK);
        return false;
    }
    if (g_exe.empty()) {
        MessageBoxW(g_main, L"build\\windows\\ttt2.exe was not found.", L"TTT2 Launcher", MB_ICONERROR | MB_OK);
        return false;
    }
    save_config();
    apply_environment();

    std::wstring command = L"\"" + g_exe.wstring() + L"\" \"" + g_cfg.game.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working = g_exe.parent_path().wstring();
    const BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, working.c_str(), &startup, &process);
    if (!ok) {
        const std::wstring message = L"Failed to start ttt2.exe. Windows error: " + std::to_wstring(GetLastError());
        MessageBoxW(g_main, message.c_str(), L"TTT2 Launcher", MB_ICONERROR | MB_OK);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void dark_control(HWND hwnd) {
    if (hwnd) SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

void font(HWND hwnd, HFONT value) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(value), TRUE);
}

HWND make_static(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT f = nullptr) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                x, y, w, h, parent, nullptr, g_instance, nullptr);
    font(hwnd, f ? f : g_font);
    return hwnd;
}

HWND make_combo(HWND parent, int id, int x, int y, int w, const std::vector<std::wstring>& items, int selected) {
    HWND hwnd = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                x, y, w, 300, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    font(hwnd, g_font);
    dark_control(hwnd);
    for (const auto& item : items) SendMessageW(hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    SendMessageW(hwnd, CB_SETCURSEL, std::clamp(selected, 0, static_cast<int>(items.size()) - 1), 0);
    return hwnd;
}

HWND make_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, bool owner = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (owner ? BS_OWNERDRAW : BS_PUSHBUTTON);
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, style, x, y, w, h, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    font(hwnd, g_font);
    dark_control(hwnd);
    return hwnd;
}

HWND make_checkbox(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, bool checked) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    font(hwnd, g_font);
    SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    dark_control(hwnd);
    return hwnd;
}

HWND make_slider(HWND parent, int id, int x, int y, int w, int minv, int maxv, int value) {
    HWND hwnd = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                x, y, w, 32, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(hwnd, TBM_SETRANGE, TRUE, MAKELPARAM(minv, maxv));
    SendMessageW(hwnd, TBM_SETPOS, TRUE, value);
    dark_control(hwnd);
    return hwnd;
}

void mark_custom() {
    if (!g_preset) return;
    SendMessageW(g_preset, CB_SETCURSEL, 0, 0);
    g_cfg.preset = 0;
}

int combo_sel(HWND hwnd) {
    const LRESULT value = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    return value == CB_ERR ? 0 : static_cast<int>(value);
}

void get_game_edit() {
    if (!g_game_edit) return;
    const int len = GetWindowTextLengthW(g_game_edit);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    if (len) GetWindowTextW(g_game_edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    g_cfg.game = text;
}

void sync_from_controls() {
    get_game_edit();
    if (g_preset) g_cfg.preset = combo_sel(g_preset);
    if (g_display_mode) g_cfg.display_mode = combo_sel(g_display_mode);
    if (g_output_res) g_cfg.output_res = combo_sel(g_output_res);
    if (g_fps) g_cfg.fps = combo_sel(g_fps);
    if (g_fill) g_cfg.fill_screen = SendMessageW(g_fill, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_render_scale) g_cfg.render_scale = combo_sel(g_render_scale);
    if (g_aa) g_cfg.aa = combo_sel(g_aa);
    if (g_upscaler) g_cfg.upscaler = std::min(combo_sel(g_upscaler), 3);
    if (g_aniso) g_cfg.aniso = combo_sel(g_aniso);
    if (g_sharpen) g_cfg.sharpness = static_cast<int>(SendMessageW(g_sharpen, TBM_GETPOS, 0, 0));
    if (g_async_shaders) g_cfg.async_shaders = SendMessageW(g_async_shaders, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_game_language) g_cfg.game_language = combo_sel(g_game_language);
    if (g_controller) g_cfg.controller = combo_sel(g_controller);
    if (g_audio) g_cfg.audio = SendMessageW(g_audio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_volume) g_cfg.volume = static_cast<int>(SendMessageW(g_volume, TBM_GETPOS, 0, 0));
    if (g_reduce_input_lag) g_cfg.reduce_input_lag = SendMessageW(g_reduce_input_lag, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_ui_language) g_cfg.ui_language = combo_sel(g_ui_language);
}

void update_slider_texts() {
    if (g_sharpen_value) {
        const std::wstring value = std::to_wstring(static_cast<int>(SendMessageW(g_sharpen, TBM_GETPOS, 0, 0))) + L"%";
        SetWindowTextW(g_sharpen_value, value.c_str());
    }
    if (g_volume_value) {
        const std::wstring value = std::to_wstring(static_cast<int>(SendMessageW(g_volume, TBM_GETPOS, 0, 0))) + L"%";
        SetWindowTextW(g_volume_value, value.c_str());
    }
}

void apply_preset(int preset) {
    if (!g_render_scale) return;
    // Custom, Performance, Balanced, Quality, Ultra.
    if (preset == 1) {
        SendMessageW(g_render_scale, CB_SETCURSEL, 1, 0);
        SendMessageW(g_aa, CB_SETCURSEL, 0, 0);
        SendMessageW(g_upscaler, CB_SETCURSEL, 3, 0);
        SendMessageW(g_aniso, CB_SETCURSEL, 1, 0);
        SendMessageW(g_sharpen, TBM_SETPOS, TRUE, 35);
    } else if (preset == 2) {
        SendMessageW(g_render_scale, CB_SETCURSEL, 1, 0);
        SendMessageW(g_aa, CB_SETCURSEL, 1, 0);
        SendMessageW(g_upscaler, CB_SETCURSEL, 3, 0);
        SendMessageW(g_aniso, CB_SETCURSEL, 2, 0);
        SendMessageW(g_sharpen, TBM_SETPOS, TRUE, 25);
    } else if (preset == 3) {
        SendMessageW(g_render_scale, CB_SETCURSEL, 2, 0);
        SendMessageW(g_aa, CB_SETCURSEL, 1, 0);
        SendMessageW(g_upscaler, CB_SETCURSEL, 0, 0);
        SendMessageW(g_aniso, CB_SETCURSEL, 3, 0);
        SendMessageW(g_sharpen, TBM_SETPOS, TRUE, 15);
    } else if (preset == 4) {
        SendMessageW(g_render_scale, CB_SETCURSEL, 3, 0);
        SendMessageW(g_aa, CB_SETCURSEL, 1, 0);
        SendMessageW(g_upscaler, CB_SETCURSEL, 0, 0);
        SendMessageW(g_aniso, CB_SETCURSEL, 4, 0);
        SendMessageW(g_sharpen, TBM_SETPOS, TRUE, 10);
    }
    update_slider_texts();
}

void show_page(Page page) {
    g_page = page;
    for (int i = 0; i < kPageCount; ++i) {
        ShowWindow(g_pages[i], i == page ? SW_SHOW : SW_HIDE);
        if (i == page) InvalidateRect(g_pages[i], nullptr, TRUE);
        InvalidateRect(g_nav[i], nullptr, TRUE);
    }
    if (page == kHome && g_home_art) InvalidateRect(g_home_art, nullptr, TRUE);
}

LRESULT CALLBACK art_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        const Gdiplus::Rect full(0, 0, std::max(1L, rc.right), std::max(1L, rc.bottom));
        Gdiplus::SolidBrush base(Gdiplus::Color(255, 7, 9, 14));
        graphics.FillRectangle(&base, full);
        if (g_theme_image) {
            draw_theme_cover(graphics,
                             Gdiplus::Rect(4, 4, std::max(1, full.Width - 8), std::max(1, full.Height - 8)),
                             1.0f);
        }
        Gdiplus::Pen frame(Gdiplus::Color(235, 196, 205, 220), 1.0f);
        graphics.DrawRectangle(&frame, Gdiplus::Rect(1, 1, std::max(1, full.Width - 3), std::max(1, full.Height - 3)));
        Gdiplus::SolidBrush red(Gdiplus::Color(245, 209, 24, 43));
        Gdiplus::SolidBrush blue(Gdiplus::Color(245, 43, 147, 232));
        graphics.FillRectangle(&red, Gdiplus::Rect(1, 1, 4, std::max(1, full.Height - 2)));
        graphics.FillRectangle(&blue, Gdiplus::Rect(std::max(1, full.Width - 5), 1, 4, std::max(1, full.Height - 2)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return SendMessageW(g_main, msg, wparam, lparam);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        paint_theme_surface(dc, r, true, hwnd == g_pages[kHome]);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

HWND page_window() {
    return CreateWindowExW(0, L"TTT2LauncherPage", L"", WS_CHILD | WS_CLIPCHILDREN,
                           kContentX, kContentY, kContentW, kContentH, g_main, nullptr, g_instance, nullptr);
}

void page_heading(HWND page, const wchar_t* title, const wchar_t* desc = nullptr) {
    make_static(page, title, 22, 18, 750, 40, g_title);
    if (desc) make_static(page, desc, 24, 62, 740, 30, g_small);
}

void label(HWND page, const wchar_t* text, int x, int y, int w = 260) {
    make_static(page, text, x, y, w, 24, g_small);
}

void create_home() {
    HWND page = g_pages[kHome] = page_window();

    g_home_art = CreateWindowExW(0, L"TTT2LauncherArt", L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 365, 28, 430, 244, page, nullptr, g_instance, nullptr);

    make_static(page, tr(T_HOME_TITLE), 24, 22, 315, 38, g_title);
    make_static(page, tr(T_HOME_DESC), 24, 63, 315, 38, g_small);

    label(page, tr(T_GAME_SOURCE), 24, 118, 300);
    g_game_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_cfg.game.wstring().c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  24, 146, 230, 34, page, reinterpret_cast<HMENU>(ID_GAME_PATH), g_instance, nullptr);
    font(g_game_edit, g_font);
    dark_control(g_game_edit);
    make_button(page, ID_BROWSE, tr(T_BROWSE), 263, 146, 80, 34, true);

    label(page, tr(T_PRESET), 24, 207, 300);
    g_preset = make_combo(page, ID_PRESET, 24, 235, 319,
                          {L"Custom", L"Performance", L"Balanced", L"Quality", L"Ultra"}, g_cfg.preset);

    make_static(page, L"VULKAN 1.3", 24, 304, 180, 26, g_nav_font);
    make_static(page, L"NATIVE RECOMPILATION", 24, 333, 260, 24, g_small);

    make_button(page, ID_PLAY, tr(T_PLAY), 24, 378, 319, 58, true);
    make_button(page, ID_SAVE, tr(T_SAVE), 24, 452, 150, 42, true);
    make_button(page, ID_RESET, tr(T_RESET), 185, 452, 158, 42, true);

    make_static(page, L"60 / 120 / 180 / 240 FPS", 395, 296, 330, 26, g_nav_font);
    make_static(page, L"FSR 1  •  ASYNC SHADERS  •  LOW LATENCY", 395, 327, 360, 24, g_small);
}

void create_display() {
    HWND page = g_pages[kDisplay] = page_window();
    page_heading(page, tr(T_DISPLAY_TITLE));

    label(page, tr(T_DISPLAY_MODE), 24, 105);
    g_display_mode = make_combo(page, ID_DISPLAY_MODE, 24, 133, 300, {L"Windowed", L"Fullscreen"}, g_cfg.display_mode);

    label(page, tr(T_OUTPUT_RES), 410, 105);
    g_output_res = make_combo(page, ID_OUTPUT_RES, 410, 133, 300,
                              {L"Auto", L"1280 × 720", L"1600 × 900", L"1920 × 1080", L"2560 × 1440", L"3840 × 2160"},
                              g_cfg.output_res);

    label(page, tr(T_FRAME_RATE), 24, 215);
    g_fps = make_combo(page, ID_FPS, 24, 243, 300, {L"Auto", L"60 FPS", L"120 FPS", L"180 FPS", L"240 FPS"}, g_cfg.fps);

    g_fill = make_checkbox(page, ID_FILL_SCREEN, tr(T_FILL_SCREEN), 410, 242, 320, 32, g_cfg.fill_screen);

    make_static(page, L"F11", 24, 345, 70, 26, g_nav_font);
    make_static(page, L"toggles fullscreen while the game is running.", 88, 346, 480, 24, g_small);
}

void create_graphics() {
    HWND page = g_pages[kGraphics] = page_window();
    page_heading(page, tr(T_GRAPHICS_TITLE));

    label(page, tr(T_INTERNAL_RES), 24, 102);
    g_render_scale = make_combo(page, ID_RENDER_SCALE, 24, 130, 300,
                                {L"Auto", L"1×  (1280×720)", L"2×  (2560×1440)", L"3×  (3840×2160)", L"4×  (5120×2880)"},
                                g_cfg.render_scale);

    label(page, tr(T_AA), 410, 102);
    g_aa = make_combo(page, ID_AA, 410, 130, 300, {L"Off", L"FXAA"}, g_cfg.aa);

    label(page, tr(T_UPSCALER), 24, 200);
    g_upscaler = make_combo(page, ID_UPSCALER, 24, 228, 300,
                            {L"Linear", L"Nearest", L"Adaptive Sharp", L"AMD FidelityFX FSR 1 (EASU)",
                             L"NVIDIA DLSS — unavailable", L"Intel XeSS — unavailable"}, g_cfg.upscaler);

    label(page, tr(T_ANISO), 410, 200);
    g_aniso = make_combo(page, ID_ANISO, 410, 228, 300, {L"Game default", L"2×", L"4×", L"8×", L"16×"}, g_cfg.aniso);

    label(page, tr(T_SHARPNESS), 24, 300);
    g_sharpen = make_slider(page, ID_SHARPEN, 24, 328, 280, 0, 100, g_cfg.sharpness);
    g_sharpen_value = make_static(page, L"", 315, 330, 80, 24, g_small);
    g_async_shaders = make_checkbox(page, ID_ASYNC_SHADERS, async_shaders_text(), 410, 320, 350, 42, g_cfg.async_shaders);

    make_static(page, tr(T_UPSCALE_NOTE), 24, 405, 735, 54, g_small);
    update_slider_texts();
}

void create_game() {
    HWND page = g_pages[kGame] = page_window();
    page_heading(page, tr(T_GAME_TITLE));

    label(page, tr(T_GAME_LANGUAGE), 24, 105);
    g_game_language = make_combo(page, ID_GAME_LANGUAGE, 24, 133, 300,
        {L"English", L"Français", L"Deutsch", L"Italiano", L"Español", L"Nederlands", L"Português", L"Русский", L"日本語"},
        g_cfg.game_language);

    label(page, tr(T_CONTROLLER), 410, 105);
    g_controller = make_combo(page, ID_CONTROLLER, 410, 133, 300,
                              {L"Auto", L"Xbox / PC", L"PlayStation", L"Nintendo"}, g_cfg.controller);

    g_audio = make_checkbox(page, ID_AUDIO, tr(T_AUDIO), 24, 235, 220, 32, g_cfg.audio);
    label(page, tr(T_VOLUME), 24, 290);
    g_volume = make_slider(page, ID_VOLUME, 24, 318, 280, 0, 100, g_cfg.volume);
    g_volume_value = make_static(page, L"", 315, 320, 80, 24, g_small);
    g_reduce_input_lag = make_checkbox(page, 500, reduce_input_lag_text(), 24, 375, 700, 34, g_cfg.reduce_input_lag);
    update_slider_texts();

    make_static(page, L"PERFORMANCE", 410, 235, 300, 24, g_nav_font);
    make_static(page, L"Low-latency input • Native Vulkan rendering", 410, 270, 350, 46, g_small);
    make_static(page, L"Performance options are applied directly by this Windows runtime.",
                410, 325, 350, 60, g_small);
}

void create_language() {
    HWND page = g_pages[kLanguage] = page_window();
    page_heading(page, tr(T_LANGUAGE_TITLE), tr(T_LANGUAGE_DESC));

    label(page, tr(T_LAUNCHER_LANGUAGE), 24, 120);
    g_ui_language = make_combo(page, ID_UI_LANGUAGE, 24, 148, 360,
        {L"Português (Brasil)", L"English", L"Español", L"Français", L"Deutsch", L"Italiano", L"日本語"},
        g_cfg.ui_language);

    make_static(page, L"TTT2 LAUNCHER", 24, 250, 300, 28, g_nav_font);
    make_static(page, L"UI language is separate from the Wii U game language.", 24, 290, 540, 28, g_small);
}

void destroy_pages() {
    for (HWND& page : g_pages) {
        if (page) DestroyWindow(page);
        page = nullptr;
    }
    g_home_art = nullptr;
    g_game_edit = g_preset = nullptr;
    g_display_mode = g_output_res = g_fps = g_fill = nullptr;
    g_render_scale = g_aa = g_upscaler = g_aniso = g_sharpen = g_sharpen_value = g_async_shaders = nullptr;
    g_game_language = g_controller = g_audio = g_volume = g_volume_value = nullptr;
    g_reduce_input_lag = nullptr;
    g_ui_language = nullptr;
}

void refresh_nav_text() {
    SetWindowTextW(g_nav[kHome], tr(T_HOME));
    SetWindowTextW(g_nav[kDisplay], tr(T_DISPLAY));
    SetWindowTextW(g_nav[kGraphics], tr(T_GRAPHICS));
    SetWindowTextW(g_nav[kGame], tr(T_GAME));
    SetWindowTextW(g_nav[kLanguage], tr(T_LANGUAGE));
}

void rebuild_pages(Page keep) {
    destroy_pages();
    refresh_nav_text();
    create_home();
    create_display();
    create_graphics();
    create_game();
    create_language();
    show_page(keep);
    InvalidateRect(g_main, nullptr, TRUE);
}

void set_ui_language(int language) {
    sync_from_controls();
    g_cfg.ui_language = std::clamp(language, 0, 6);
    save_config();
    rebuild_pages(kLanguage);
}

void draw_button(const DRAWITEMSTRUCT* item) {
    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    RECT r = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const int id = static_cast<int>(item->CtlID);

    COLORREF fill = kPanel2, border = kBorder, fg = kText;
    if (id >= ID_NAV_HOME && id <= ID_NAV_LANGUAGE) {
        const int p = id - ID_NAV_HOME;
        fill = p == g_page ? RGB(34, 17, 25) : kSidebar;
        border = p == g_page ? kRed : RGB(20, 24, 31);
    } else if (id == ID_PLAY) {
        fill = pressed ? kRedDark : kRed;
        border = kIce;
    } else {
        fill = pressed ? RGB(29, 48, 66) : kPanel2;
        border = kBorder;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(item->hDC, &r, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, id == ID_PLAY ? 2 : 1, border);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(item->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(pen);

    if (id >= ID_NAV_HOME && id <= ID_NAV_LANGUAGE && id - ID_NAV_HOME == g_page) {
        RECT accent_left{r.left, r.top, r.left + 4, r.bottom};
        RECT accent_right{r.right - 2, r.top, r.right, r.bottom};
        HBRUSH red = CreateSolidBrush(kRed);
        HBRUSH blue = CreateSolidBrush(kBlue);
        FillRect(item->hDC, &accent_left, red);
        FillRect(item->hDC, &accent_right, blue);
        DeleteObject(red);
        DeleteObject(blue);
    }

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, fg);
    SelectObject(item->hDC, id == ID_PLAY ? g_nav_font : g_font);
    DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void handle_browse() {
    const auto chosen = choose_game();
    if (!chosen) return;
    g_cfg.game = *chosen;
    SetWindowTextW(g_game_edit, g_cfg.game.c_str());
}

void handle_upscaler_change() {
    const int selected = combo_sel(g_upscaler);
    if (selected <= 3) {
        mark_custom();
        return;
    }
    MessageBoxW(g_main, tr(T_UNSUPPORTED_UPSCALER), L"TTT2 Launcher", MB_ICONINFORMATION | MB_OK);
    SendMessageW(g_upscaler, CB_SETCURSEL, g_cfg.upscaler <= 3 ? g_cfg.upscaler : 0, 0);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        if (id >= ID_NAV_HOME && id <= ID_NAV_LANGUAGE && code == BN_CLICKED) {
            show_page(static_cast<Page>(id - ID_NAV_HOME));
            return 0;
        }
        if (id == ID_BROWSE && code == BN_CLICKED) {
            handle_browse();
            return 0;
        }
        if (id == ID_PLAY && code == BN_CLICKED) {
            sync_from_controls();
            if (launch_game()) DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_SAVE && code == BN_CLICKED) {
            sync_from_controls();
            save_config();
            return 0;
        }
        if (id == ID_RESET && code == BN_CLICKED) {
            sync_from_controls();
            defaults(true, true);
            save_config();
            rebuild_pages(kHome);
            return 0;
        }
        if (id == ID_PRESET && code == CBN_SELCHANGE) {
            g_cfg.preset = combo_sel(g_preset);
            apply_preset(g_cfg.preset);
            return 0;
        }
        if (id == ID_UPSCALER && code == CBN_SELCHANGE) {
            handle_upscaler_change();
            return 0;
        }
        if (id == ID_UI_LANGUAGE && code == CBN_SELCHANGE) {
            set_ui_language(combo_sel(g_ui_language));
            return 0;
        }
        if ((id == ID_RENDER_SCALE || id == ID_AA || id == ID_ANISO) && code == CBN_SELCHANGE) mark_custom();
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == g_sharpen) mark_custom();
        update_slider_texts();
        return 0;

    case WM_DRAWITEM:
        draw_button(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kText);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel2);
        return reinterpret_cast<LRESULT>(g_edit_brush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(g_panel_brush);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);

        paint_theme_surface(dc, client, false);

        RECT side{0, 0, kSidebarWidth, client.bottom};
        HBRUSH sidebar = CreateSolidBrush(RGB(7, 9, 14));
        FillRect(dc, &side, sidebar);
        DeleteObject(sidebar);

        RECT top{kSidebarWidth, 0, client.right, 92};
        Gdiplus::Graphics top_graphics(dc);
        Gdiplus::SolidBrush top_veil(Gdiplus::Color(120, 6, 9, 15));
        top_graphics.FillRectangle(
            &top_veil,
            Gdiplus::Rect(static_cast<INT>(top.left), static_cast<INT>(top.top),
                          static_cast<INT>(top.right - top.left), static_cast<INT>(top.bottom - top.top)));

        HBRUSH red = CreateSolidBrush(kRedDark);
        HBRUSH blue = CreateSolidBrush(kBlueDark);
        RECT red_bar{kSidebarWidth, 89, kSidebarWidth + (client.right - kSidebarWidth) / 2, 92};
        RECT blue_bar{red_bar.right, 89, client.right, 92};
        FillRect(dc, &red_bar, red);
        FillRect(dc, &blue_bar, blue);
        DeleteObject(red);
        DeleteObject(blue);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        SelectObject(dc, g_title);
        RECT title{kContentX, 18, client.right - 30, 58};
        DrawTextW(dc, L"TEKKEN TAG TOURNAMENT 2", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, kIce);
        SelectObject(dc, g_small);
        RECT subtitle{kContentX + 2, 58, client.right - 30, 82};
        DrawTextW(dc, tr(T_SUBTITLE), -1, &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(dc, kIce);
        SelectObject(dc, g_nav_font);
        RECT brand{22, 25, kSidebarWidth - 15, 55};
        DrawTextW(dc, L"TTT2", -1, &brand, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, kMuted);
        SelectObject(dc, g_small);
        RECT tag{22, 56, kSidebarWidth - 15, 90};
        DrawTextW(dc, tr(T_TAGLINE), -1, &tag, DT_LEFT | DT_WORDBREAK);

        RECT footer{22, client.bottom - 58, kSidebarWidth - 15, client.bottom - 16};
        SetTextColor(dc, RGB(118, 132, 151));
        DrawTextW(dc, L"EU v16  •  VULKAN 1.3  •  WINDOWS", -1, &footer, DT_LEFT | DT_WORDBREAK);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void create_nav() {
    const TextKey labels[] = {T_HOME, T_DISPLAY, T_GRAPHICS, T_GAME, T_LANGUAGE};
    for (int i = 0; i < kPageCount; ++i) {
        g_nav[i] = make_button(g_main, ID_NAV_HOME + i, tr(labels[i]), 18, 125 + i * 54, 184, 42, true);
        font(g_nav[i], g_nav_font);
    }
}

bool init_window() {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW art_class{sizeof(art_class)};
    art_class.lpfnWndProc = art_proc;
    art_class.hInstance = g_instance;
    art_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    art_class.hbrBackground = g_bg_brush;
    art_class.lpszClassName = L"TTT2LauncherArt";
    if (!RegisterClassExW(&art_class)) return false;

    WNDCLASSEXW page_class{sizeof(page_class)};
    page_class.lpfnWndProc = page_proc;
    page_class.hInstance = g_instance;
    page_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    page_class.hbrBackground = g_panel_brush;
    page_class.lpszClassName = L"TTT2LauncherPage";
    if (!RegisterClassExW(&page_class)) return false;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = g_bg_brush;
    wc.lpszClassName = L"TTT2LauncherWindow";
    if (!RegisterClassExW(&wc)) return false;

    g_main = CreateWindowExW(0, wc.lpszClassName, L"Tekken Tag Tournament 2 — Launcher",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 690,
                             nullptr, nullptr, g_instance, nullptr);
    if (!g_main) return false;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_main, 20, &dark, sizeof(dark));

    create_nav();
    create_home();
    create_display();
    create_graphics();
    create_game();
    create_language();
    show_page(kHome);

    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr);

    const fs::path self = module_path();
    g_root = self.empty() ? fs::current_path() : self.parent_path();
    g_ini = g_root / L"TTT2 Launcher.ini";
    g_exe = find_game_executable();

    load_theme_image();
    load_config();

    g_bg_brush = CreateSolidBrush(kBg);
    g_panel_brush = CreateSolidBrush(kPanel);
    g_edit_brush = CreateSolidBrush(kPanel2);
    g_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_small = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_title = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_nav_font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    int result = 1;
    if (init_window()) {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        result = static_cast<int>(msg.wParam);
    }

    DeleteObject(g_font);
    DeleteObject(g_small);
    DeleteObject(g_title);
    DeleteObject(g_nav_font);
    DeleteObject(g_bg_brush);
    DeleteObject(g_panel_brush);
    DeleteObject(g_edit_brush);
    g_theme_image.reset();
    if (g_theme_stream) {
        g_theme_stream->Release();
        g_theme_stream = nullptr;
    }
    if (g_gdiplus_token) Gdiplus::GdiplusShutdown(g_gdiplus_token);
    CoUninitialize();
    return result;
}
#endif
