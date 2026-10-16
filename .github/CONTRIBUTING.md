# Contributing

Thanks for helping improve the Windows port.

## Before opening a report

Update `windows-host`, rebuild without `-Clean`, and reproduce the issue with the latest
executable.

For performance issues, include the CPU/GPU, driver, render scale, target FPS, whether the hitch
only occurs the first time an effect/stage appears, and the runtime log around the hitch.

For graphics issues, include a screenshot and the matching runtime log when possible.

## Do not upload game files

Never attach or commit game archives, RPX/RPL files, generated game C++, extracted game assets,
or copyrighted dumps. The repository must remain distributable without game content.

## Code organization

- Windows launcher/UI: `launcher/`
- host/runtime behavior: `runtime/`
- static recompiler: `recomp/`
- generic developer tools: `tools/`
- documentation: `docs/`

Keep generated build output out of Git.

## Validation

Windows changes should keep the **Windows host** GitHub Actions workflow green and preserve the
data-independent tests.
