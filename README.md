# Tekken Tag Tournament 2 Recompiled — Windows Port

[![Windows host](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml/badge.svg?branch=windows-host)](https://github.com/xruanzin/Tekken-Tag-2-Recompiled/actions/workflows/windows.yml)

Unofficial **Windows 11 port** of the original Linux static-recompilation project
[Petexy/Tekken-Tag-2-Recompiled](https://github.com/Petexy/Tekken-Tag-2-Recompiled).

**Original Linux port / recompiler architecture:** Petexy and upstream contributors.  
**Windows port, launcher, host integration and Windows-focused performance work:** xruanzin.

This fork keeps the original project's static-recompilation architecture and native Windows/Vulkan
runtime. Public player releases use the ahead-of-time PowerPC-to-x86-64 port built once by the
maintainer from the supported EU v16 RPX. The player never runs cafe-recomp, CMake or Clang.

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

The player-facing package follows the same workflow as the GOW3 Windows port: download one ZIP,
open the launcher, choose the original game and press **PLAY**. The release already contains the
prebuilt static port runtime.

A packaged installation is laid out as:

```text
ttt2-windows/
├─ Tekken Tag Tournament 2.exe
├─ Play Tekken Tag Tournament 2.exe
├─ bin/
│  └─ ttt2-runtime.exe
├─ user/
├─ out/
├─ README-Windows.txt
└─ BUILD-INFO.txt
```

`bin/ttt2-runtime.exe` is produced from the ahead-of-time recompiler by the maintainer before
the release is published. End users do not need the compiler toolchain.

GitHub CI deliberately does **not** publish the experimental generic/JIT runtime as a player
release. Production player packages are created with `release-windows.ps1`, which refuses to
package a runtime unless it identifies itself as a static AOT build.

## First run

1. Extract the complete Windows ZIP.
2. Open `Tekken Tag Tournament 2.exe`.
3. Select the supported EU v16 game folder or `.wua`.
4. Click **PLAY / JOGAR**.

On later starts, `Play Tekken Tag Tournament 2.exe` skips the launcher window and uses the
saved settings. Player-created data stays under `user/`, including `user/last_run.log`.

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

## Maintainer release build

The release owner can build and publish the complete player package with one command after
checking out `windows-host`:

```powershell
.\release-windows.ps1 -Game "D:\Path\To\TTT2-EU-v16" -Version v1.0.0 -Publish
```

The script verifies the EU v16 `Tekken.rpx`, builds the static AOT runtime, rejects the
experimental dynamic host, assembles the GOW3-style folder layout, creates SHA-256 checksums and
can upload the ZIP plus executables to GitHub Releases.

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

The Windows fork keeps reusable shader, Vulkan pipeline and detiled-texture caches. The static
player runtime starts without any local game recompilation. The first run through a stage or
character combination can still be less smooth while new graphics work is compiled; repeated
content should reuse more cached data.

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
