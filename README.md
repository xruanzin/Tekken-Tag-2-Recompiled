# Tekken Tag Tournament 2 Recompiled — Windows Port

[![Windows host](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml/badge.svg?branch=windows-host)](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml)

Unofficial **Windows 11 port** of the original Linux static-recompilation project
[Petexy/Tekken-Tag-2-Recompiled](https://github.com/Petexy/Tekken-Tag-2-Recompiled).

**Original Linux port / recompiler architecture:** Petexy and upstream contributors.  
**Windows port, launcher, host integration and Windows-focused performance work:** xruanzin.

This fork keeps the original project's native Windows runtime and Vulkan GX2 implementation,
while providing two execution paths. Developer builds can still use the original ahead-of-time
PowerPC-to-C++ recompiler; public Windows releases use a generic zero-install host that reads
the user's supported RPX at run time, predecodes guest code and dispatches it through the native
runtime. This removes the end user's C++ build/toolchain step.

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

The generic Windows host validates this hash at run time. The developer build script verifies
the same hash before ahead-of-time recompilation.

## Releases

The [GitHub Releases](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/releases) page provides
the complete **ready-to-run Windows package**.

For a new installation, download `TTT2-Windows-<version>.zip`. It contains:

- `TTT2 Launcher.exe`;
- the generic `ttt2.exe` Windows host;
- the project README and license.

The generic `ttt2.exe` does **not** contain Tekken game files or precompiled Tekken game code.
The user's own supported EU v16 game is selected from the launcher and validated at run time.

The standalone launcher and host executables are also attached separately to releases for users
updating an existing installation.

## First run

There is no first-run C++ compilation.

1. Extract the complete Windows release ZIP.
2. Open `TTT2 Launcher.exe`.
3. Choose the game source: either the supported extracted game folder or a supported `.wua`.
4. Click **PLAY / JOGAR**.

No Git, CMake, Ninja, LLVM/Clang, Python or Visual Studio Build Tools are required to play the
release build. No PowerShell or build terminal is launched.

Runtime diagnostics are written to `logs/ttt2-last-run.txt` beside the selected game source
(or inside the selected extracted game folder). Shader, pipeline and texture caches continue to
be reused between runs.

## Quick start on Windows

### Requirements

For the release package:

- Windows 11 x64
- CPU with AVX2
- Vulkan 1.3-capable GPU and current driver
- a legally obtained supported EU v16 game folder or supported `.wua`

The development toolchain below is needed only when building the project from source.

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

The Windows fork keeps reusable shader, Vulkan pipeline and detiled-texture caches. The first
run through a stage or character combination can still be less smooth while new work is being
compiled. Repeated content should reuse more cached data.

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
