Tekken Tag Tournament 2 Recompiled - Windows
================================================

No original Tekken game files are included.

This Windows Player package follows the same player-facing goal as the GOW3 Windows port:
download one ZIP, open the launcher, choose your own game folder and press PLAY.

Requirements
- Windows 11 x64.
- Vulkan 1.3-capable GPU with a current driver.
- Your own legally obtained European v16 Wii U copy of Tekken Tag Tournament 2.
- An extracted game folder containing code\Tekken.rpx.
- Windows Package Manager (winget) if required first-run compiler components are not installed.

Starting
1. Extract the complete ZIP.
2. Open "Tekken Tag Tournament 2.exe".
3. Select the extracted EU v16 game folder.
4. Click PLAY / JOGAR.
5. On the first launch, wait for the preparation progress bar to reach 100%.

The first start performs a one-time local PowerPC -> x86-64 preparation from your own
code\Tekken.rpx. The generated bin\ttt2-runtime.exe stays only on your PC. The downloadable
Player ZIP does not include generated game code or a precompiled game-derived AOT runtime.

If required compiler components are missing, the launcher preparer attempts to install them
automatically through winget. This one-time setup can be large and can take several minutes.

Next time
- "Play Tekken Tag Tournament 2.exe" starts directly with the saved game source and settings.
- The locally prepared runtime is reused; the game is not recompiled on every launch.
- Player-created files stay under user\ next to the launcher.
- Preparation details are written to user\prepare.log.
- Runtime logs and reusable caches remain local to the Player folder.

Package layout
  Tekken Tag Tournament 2.exe
  Play Tekken Tag Tournament 2.exe
  bin\                    (local runtime appears here after preparation)
  tools\prepare-player.ps1
  tools\source\
  user\
  out\
  README-PLAYER.txt
  BUILD-INFO.txt

Maintainer public release creation
- release-player.ps1 creates the generic Player ZIP.
- Public Player releases contain no generated game code.
- release-windows.ps1 remains for local static AOT testing only and cannot publish.

Original Linux port/static recompilation architecture: Petexy and upstream contributors.
Windows port, launcher and host integration: xruanzin.
AI-assisted code review, debugging and documentation: OpenAI ChatGPT.
