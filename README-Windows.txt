Tekken Tag Tournament 2 Recompiled - Windows
================================================

No Tekken game files are included.

Requirements
- Windows 11 x64.
- Vulkan 1.3-capable GPU with a current driver.
- Your own legally obtained European v16 Wii U copy of Tekken Tag Tournament 2.

Starting
1. Extract the complete ttt2-windows folder.
2. Open "Tekken Tag Tournament 2.exe".
3. Select either the extracted game folder containing code/content/meta or a supported .wua.
4. Click PLAY / JOGAR.

Nothing else is required on the player's PC. The release already contains the launcher and the
generic Windows runtime under bin\ttt2-runtime.exe. Do not install CMake, Ninja, LLVM/Clang,
Python or Visual Studio Build Tools just to play.

Next time
- "Play Tekken Tag Tournament 2.exe" starts directly with the game source and settings saved by
  the launcher.
- Player-created files stay under user\ next to the launcher.
- The latest runtime log is user\last_run.log.

Package layout
  Tekken Tag Tournament 2.exe
  Play Tekken Tag Tournament 2.exe
  bin\ttt2-runtime.exe
  user\
  README-Windows.txt

The public package contains no Tekken.rpx, generated game C++ source or Tekken game assets.

Original Linux port/static recompilation architecture: Petexy and upstream contributors.
Windows port, launcher and host integration: xruanzin.
AI-assisted code review, debugging and documentation: OpenAI ChatGPT.
