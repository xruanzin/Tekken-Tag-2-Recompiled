# Project structure

This fork keeps Windows-specific presentation code separate from the core recompiler/runtime so
that the repository is easier to understand and easier to sync with upstream.

## Top-level directories

### `launcher/`

Windows-only user interface code.

- `src/windows_launcher.cpp` — native Win32 launcher.
- `assets/ttt2_launcher_theme.jpg` — launcher artwork embedded into Windows builds.
- `make_windows_icon.py` — converts the user's local title icon to a Windows ICO during a
  local build.

### `runtime/`

The native host runtime used by the recompiled title. It contains host OS replacements,
GX2/Vulkan rendering, shader translation, presentation, input, audio and filesystem support.

### `recomp/`

Function-level PowerPC-to-C++ recompiler.

### `tools/`

Generic development utilities such as dependency setup, RPX inspection and WUA tooling.
Windows launcher code deliberately does not live here.

### `tests/`

Data-independent tests plus optional differential tests that can use a locally supplied RPX.

### `docs/`

Long-form architecture and Windows documentation.

### `.github/`

CI workflows and contribution guidance.

## Root files

The root is kept to common project entry points and build/package metadata:

- `README.md`
- `LICENSE`
- `CMakeLists.txt`
- `build-windows.ps1`
- `vcpkg.json`
- `dependencies.lock.json`
- `.gitignore`

## Local/generated content

The following are excluded from Git: `build*/`, `generated/`, `third_party/`, `local/`,
game archives/executables, captures/dumps, generated Windows executables and launcher config.
