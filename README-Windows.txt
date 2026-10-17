Tekken Tag Tournament 2 Recompiled - Windows
================================================

No original Tekken game files are included.

Player goal
Download one ZIP, open the launcher, choose your own supported game folder and press PLAY.
There is no compiler installation and no Full-AOT preparation before the first launch.

Requirements
- Windows 11 x64.
- Vulkan 1.3-capable GPU with a current driver.
- Your own legally obtained European v16 Wii U copy of Tekken Tag Tournament 2.
- An extracted game folder containing code\Tekken.rpx.

Starting
1. Extract the complete ZIP to a normal empty folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Select the extracted EU v16 game folder.
4. Click PLAY / JOGAR.

The release already contains:
  bin\ttt2-runtime.exe

The launcher validates the selected code\Tekken.rpx and starts that runtime directly.
The runtime reads the PowerPC executable from the selected game directory, translates code
through its built-in dynamic host and emits native x64 blocks on demand. Reusable block and
renderer caches are kept under out\ so later execution can reuse warmed work.

The player does NOT install or require:
- CMake
- Ninja
- LLVM / clang-cl
- Visual Studio Build Tools
- Python
- Git
- vcpkg

The player also does NOT generate a Full-AOT ttt2-runtime.exe from the user's RPX.

Next time
- "Play Tekken Tag Tournament 2.exe" starts directly with the saved game source and settings.
- Player-created settings, saves and logs stay under user\.
- Reusable code and renderer caches stay under out\.

Package layout
  Tekken Tag Tournament 2.exe
  Play Tekken Tag Tournament 2.exe
  bin\
    ttt2-runtime.exe
  launcher\assets\
    ttt2_launcher_theme.jpg
  user\
  out\
  README-PLAYER.txt
  BUILD-INFO.txt

Maintainer public release creation
- release-player.ps1 creates the zero-install Player ZIP with the generic prebuilt runtime.
- Public Player releases contain no Tekken game files and no generated Full-AOT game code.
- release-windows.ps1 remains available for local static AOT development/testing.

Original Linux port/static recompilation architecture: Petexy and upstream contributors.
Windows port, launcher and host integration: xruanzin.
AI-assisted code review, debugging and documentation: OpenAI ChatGPT.
