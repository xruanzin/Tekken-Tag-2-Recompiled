Tekken Tag Tournament 2 Recompiled - Windows
================================================

No original Tekken game files are included.

This Windows package follows the same player-facing model as the GOW3 Windows port:
the port runtime is already built by the maintainer. The player does NOT compile the
game locally.

Requirements
- Windows 11 x64.
- Vulkan 1.3-capable GPU with a current driver.
- Your own legally obtained European v16 Wii U copy of Tekken Tag Tournament 2.

Starting
1. Extract the complete ttt2-windows folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Select either the extracted game folder containing code/content/meta or a supported .wua.
4. Click PLAY / JOGAR.

The included bin\ttt2-runtime.exe is the prebuilt static ahead-of-time recompiled Windows
runtime. The player's PC does not need cafe-recomp, CMake, Ninja, LLVM/Clang, Python or
Visual Studio Build Tools.

Next time
- "Play Tekken Tag Tournament 2.exe" starts directly with the game source and settings saved
  by the launcher.
- Player-created files stay under user\ next to the launcher.
- The latest runtime log is user\last_run.log.
- Reusable caches stay under out\.

Package layout
  Tekken Tag Tournament 2.exe
  Play Tekken Tag Tournament 2.exe
  bin\ttt2-runtime.exe
  user\
  out\
  README-Windows.txt
  BUILD-INFO.txt

The launcher verifies the selected EU v16 source before starting the runtime.

Maintainer release creation
- The maintainer builds the static runtime once from the supported Tekken.rpx.
- release-windows.ps1 verifies that the produced binary is a static AOT build before it can
  be packaged or published.
- GitHub CI tests the host, launchers and game-independent code, but it does not publish the
  experimental dynamic/JIT runtime as a player release.

Original Linux port/static recompilation architecture: Petexy and upstream contributors.
Windows port, launcher and host integration: xruanzin.
AI-assisted code review, debugging and documentation: OpenAI ChatGPT.
