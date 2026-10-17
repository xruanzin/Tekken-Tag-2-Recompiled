# Tekken Tag Tournament 2 Recompiled — Windows Port

[![Windows host](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml/badge.svg?branch=windows-host)](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml)

Unofficial **Windows 11 port** of the original Linux static-recompilation project
[Petexy/Tekken-Tag-2-Recompiled](https://github.com/Petexy/Tekken-Tag-2-Recompiled).

**Original Linux port / recompiler architecture:** Petexy and upstream contributors.  
**Windows port, launcher, host integration and Windows-focused performance work:** xruanzin.

This fork keeps the original project's static-recompilation architecture and native Windows/Vulkan
runtime. Public Player releases do not contain game-derived AOT code. On first launch the Player
prepares a local PowerPC-to-x86-64 runtime from the user's own supported EU v16 RPX, shows the
current preparation stage in the launcher, and reuses that local runtime on later launches.

> [!IMPORTANT]
> **No game files are included.** You must provide your own legally obtained copy of
> *TEKKEN TAG TOURNAMENT 2 Wii U EDITION*. Do not upload RPX files, game archives,
> generated game code, captures, dumps or other copyrighted game assets to this repository.
>
> This project is not affiliated with or endorsed by Bandai Namco Entertainment or Nintendo.

## Windows fork goals

This branch focuses on making the original Linux port feel like a native Windows project:

- native Windows 11 build with Clang/LLVM, CMake, Ninja and Vulkan;
- dedicated Windows launcher;
- persistent shader, pipeline and detiled-texture caches;
- Windows file I/O and presentation tuning;
- low-latency input option;
- 60 / 120 / 180 / 240 Hz presentation modes;
- internal-resolution scaling and FSR 1 presentation scaling;
- Windows-specific AMD iGPU pipeline-latency mitigation;
- reproducible CI builds and data-independent tests.

Linux development remains centered on the
[original upstream repository](https://github.com/Petexy/Tekken-Tag-2-Recompiled).

## Current status

The Windows port boots and runs the game through menus, character selection and matches.
The runtime is actively being optimized, so first-use shader/pipeline compilation may still
cause temporary stutter while caches are populated.

The renderer uses Vulkan 1.3. Recent Windows-specific work includes persistent pipeline and
shader caches, persistent texture detile cache, asynchronous compilation, adaptive file
read-ahead, presentation tuning and special handling for expensive first-use pipeline creation
on integrated AMD GPUs.

## Supported game revision

Only the European v16 executable is supported.

| Property | Value |
| --- | --- |
| Product code | WUP-P-AKNP |
| Base title ID | `000500001010F800` |
| Update title ID | `0005000E1010F800` |
| Version | 16 |
| `code/Tekken.rpx` SHA-256 | `fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019` |

The maintainer build verifies this hash before ahead-of-time recompilation, and the packaged
runtime verifies the player's selected game source before launch.

## Releases

The player-facing package follows the same workflow goal as the GOW3 Windows port: download one ZIP,
open the launcher, choose your own game folder and press **PLAY**. Because the Wii U title uses
PowerPC code, the first launch performs a one-time local AOT preparation from the player's own
`code/Tekken.rpx`. The launcher shows the current stage and a progress bar during this process.

A generic Player installation is laid out as:

```text
ttt2-windows/
├─ Tekken Tag Tournament 2.exe
├─ Play Tekken Tag Tournament 2.exe
├─ bin/
│  └─ ttt2-runtime.exe        # created locally after first-run preparation
├─ tools/
│  ├─ prepare-player.ps1
│  └─ source/
├─ user/
├─ out/
├─ README-PLAYER.txt
└─ BUILD-INFO.txt
```

The downloadable ZIP does **not** contain `ttt2-runtime.exe` or generated game code. If required
compiler components are missing, the first-run preparer attempts to install them automatically
through Windows Package Manager. This one-time setup can be large and can take several minutes.

## First run

1. Extract the complete Windows Player ZIP.
2. Open `Tekken Tag Tournament 2.exe`.
3. Select the supported extracted EU v16 game folder containing `code/Tekken.rpx`.
4. Click **PLAY / JOGAR**.
5. Wait for the preparation progress bar to reach 100%; the game then starts automatically.

On later starts, `Play Tekken Tag Tournament 2.exe` skips the launcher window and uses the saved
settings and locally prepared runtime. Player-created data stays under `user/`, including
preparation/runtime logs and persistent caches.

## Quick start on Windows

### Requirements

For the Player package:

- Windows 11 x64
- CPU with AVX2
- Vulkan 1.3-capable GPU and current driver
- a legally obtained extracted EU v16 game folder containing `code/Tekken.rpx`
- Windows Package Manager (`winget`) if the required first-run compiler components are not already installed

The launcher handles the one-time local preparation. The manual developer toolchain below is only
needed when working on the project itself.

### Manual developer build

```powershell
git clone https://github.com/xruanzin/Tekken-Tag-2-Recompiled.git
cd Tekken-Tag-2-Recompiled
git checkout windows-host

Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1 -Rpx "D:\Path\To\Game\code\Tekken.rpx"
```

Do not use `-Clean` for ordinary updates. The build system can reuse already compiled
objects and generated code.

Successful output is placed at:

```text
build\windows\ttt2.exe
TTT2 Launcher.exe
```

More Windows-specific details are in [docs/WINDOWS.md](docs/WINDOWS.md).

## Maintainer release build

The public Player ZIP is built and published with:

```powershell
.\release-player.ps1 -Version v1.0.0 -Publish
```

This package contains only the generic launcher, quick-play helper and local preparation sources.
It deliberately excludes generated game code and any locally built `ttt2-runtime.exe`.

`release-windows.ps1` remains available for local developer/testing packages built from the
maintainer's own RPX, but its `-Publish` path is disabled.

## Launcher

The Windows launcher provides:

- game folder or `.wua` selection;
- windowed/fullscreen mode and output resolution;
- 60/120/180/240 FPS presentation;
- internal render scale and FXAA;
- linear, nearest, adaptive-sharp and FSR 1 scaling;
- anisotropic filtering and sharpness;
- audio and volume;
- controller icon preference;
- game/interface language;
- asynchronous shader/pipeline compilation;
- reduced input-lag mode.

Launcher artwork and launcher-specific code live together under
[`launcher/`](launcher/).

## Performance and caches

The Windows fork keeps reusable shader, Vulkan pipeline and detiled-texture caches. After the
one-time first-run AOT preparation, normal launches reuse the locally prepared runtime and do not
recompile the game. The first run through a stage or character combination can still be less
smooth while new graphics work is compiled; repeated content should reuse more cached data.

For performance reports, include the runtime log and describe the exact match, stage and action
that caused the hitch. Do **not** attach game data.

## Repository layout

| Path | Purpose |
| --- | --- |
| `launcher/` | Windows launcher source, artwork and launcher helper scripts |
| `runtime/` | Native host runtime, Cafe OS replacements, GX2/Vulkan renderer, audio/input/filesystem |
| `recomp/` | PowerPC-to-C++ static recompiler |
| `tools/` | Generic project/recompiler utilities |
| `tests/` | Data-independent and differential tests |
| `docs/` | Architecture, Windows notes and project structure |
| `.github/` | CI and contribution metadata |
| `build-windows.ps1` | Main Windows build entry point |

See [docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md) for the detailed map.

Generated/local directories such as `build*/`, `generated/`, `third_party/` and
`local/` are ignored and should not be committed.

## Development

Useful technical documentation:

- [Windows host notes](docs/WINDOWS.md)
- [Runtime and renderer architecture](docs/ARCHITECTURE.md)
- [Repository structure](docs/PROJECT_STRUCTURE.md)
- [Contribution guidelines](.github/CONTRIBUTING.md)

## Credits

This Windows fork exists because of the work done by the original project and the broader Wii U
reverse-engineering community.

- **Petexy / Tekken-Tag-2-Recompiled** — original Linux port, static recompilation design and
  the core TTT2 runtime this fork is based on.
- **Upstream contributors** — fixes, testing and continued work on the original project.
- **xruanzin** — Windows port, Windows launcher, host integration and Windows-focused
  performance/compatibility work in this fork.
- **OpenAI ChatGPT** — AI-assisted code review, debugging, Windows launcher/UI refinements,
  performance investigation, repository organization and documentation for this Windows fork.
- **decaf-emu** — Wii U/GX2 research and AMD address-library work used by the runtime.
- **Dolphin** — PowerPC interpreter/reference work used by the project's instruction tests.
- **XenonRecomp** and **N64Recomp** — important prior art for function-level static recompilation.

Third-party dependencies retain their own licenses.

## License

Repository code is provided under the [MIT License](LICENSE), subject to the licenses of
third-party components.

The license does **not** grant rights to TEKKEN, Wii U game data or any code/assets generated
from a user's copy of the game.
