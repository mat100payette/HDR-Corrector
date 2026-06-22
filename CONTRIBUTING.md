# Contributing

Thanks for taking a look at HDR Corrector.

## Before opening a PR

- Open an issue first for larger behavior changes.
- Keep changes focused; avoid unrelated formatting or refactors.
- Build locally on Windows before sending a PR.

```powershell
.\scripts\build.ps1 -Configuration Release
```

## Development notes

- The app is native C++20/Win32 with Windows Graphics Capture and D3D11.
- Keep runtime dependencies out unless they are clearly necessary.
- Do not add network access, services, drivers, or Discord injection.
- Release tags and GitHub Releases are created by the manual Release workflow.

## Pull requests

Include:

- What changed.
- Why it changed.
- How it was tested.
- Any user-visible behavior changes.
