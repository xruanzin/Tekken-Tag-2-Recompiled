# Windows 11 host port

This branch keeps the existing Vulkan renderer and ports the host/runtime to
Windows 11 x64.  It is still a static-recompilation project: no game files or
generated game code belong in Git.

## Runtime requirements

- Windows 11 x64
- AVX2/FMA capable CPU (same x86-64-v3-era requirement as the original port)
- Vulkan 1.3 driver with `VK_EXT_external_memory_host` and push descriptors
- Current AMD, Intel or NVIDIA Vulkan driver

## One-command local build

Once you have extracted your own European v16 `Tekken.rpx`, the repository can generate the local C++ and build the final Windows executable without uploading game-derived files anywhere:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build-windows.ps1 -Rpx "C:\path\to\Tekken.rpx"
```

The result is `build\windows\ttt2.exe`.

## Build requirements

- Git, CMake, Ninja, Python 3
- LLVM/Clang (the Windows CI uses `clang-cl`)
- vcpkg

The included `vcpkg.json` supplies SDL3, shaderc, OpenSSL, zlib, zstd and the
minimal Vulkan headers/loader package. The documented triplet is static so the finished port does
not need SDL/shaderc DLLs beside the executable; the system Vulkan loader comes
from the installed GPU driver.  `tools/setup_deps.py` still downloads the pinned
ZArchive and AMD addrlib sources.

```powershell
py tools/setup_deps.py

# From a Developer PowerShell with LLVM available:
cmake -S . -B build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

cmake --build build/windows --target cafe-recomp cafe_runtime
```

The hardened `wua_extract` helper is still POSIX-only.  On Windows, extract
`code/Tekken.rpx` from your own European v16 title with Cemu (or perform the
analysis extraction on Linux/WSL), then generate the local C++ exactly as on
the original project:

```powershell
build/windows/recomp/cafe-recomp.exe C:\path\to\Tekken.rpx generated\ttt2
cmake -S . -B build/windows -DTTT2_GENERATED_DIR="$PWD/generated/ttt2"
cmake --build build/windows --target ttt2
```

Run directly from your own `.wua` or extracted title folder:

```powershell
build\windows\ttt2.exe "D:\Games\TEKKEN TAG 2 Wii U EDITION (EU).wua"
```

Default saves and shader caches are under `%LOCALAPPDATA%\ttt2`.  Setting
`TTT2_SAVE_DIR` overrides the save directory.

## What changed

- guest 4 GiB reservation/commit uses `VirtualAlloc` on Windows;
- host threads use `_beginthreadex`, while Linux keeps pthreads behind one
  portable wrapper;
- crash reporting uses an unhandled-exception filter/SEH on Windows;
- host file I/O no longer requires `pread`, `pwrite`, `fsync` or POSIX file
  descriptors;
- save/cache/config paths use `%LOCALAPPDATA%` on Windows and XDG on Linux;
- SDL3 still owns window/input/audio; Vulkan remains the renderer;
- a Windows CI job compiles the host runtime without any game-derived code.

The CI deliberately cannot build the final `ttt2.exe`, because the generated
C++ is derived from the user's game and must not be uploaded to GitHub.
