# HDR Corrector

[![CI](https://img.shields.io/github/actions/workflow/status/mat100payette/HDR-Corrector/ci.yml?branch=main&label=CI&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/actions/workflow/status/mat100payette/HDR-Corrector/release.yml?label=release&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/mat100payette/HDR-Corrector?include_prereleases&sort=semver&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/mat100payette/HDR-Corrector/total?style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases)
[![Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D4?logo=windows11&logoColor=white&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases/latest)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white&style=flat-square)](https://github.com/mat100payette/HDR-Corrector)

HDR Corrector is a native Windows 11 tray utility that captures HDR desktop content correctly and produces SDR-friendly screenshots and stream mirrors.

It is built for the common Windows HDR capture problem where screenshots, screen share, and SDR viewers treat HDR desktop pixels incorrectly, resulting in overly bright or washed-out output. HDR Corrector keeps the display in HDR and captures the desktop through Windows Graphics Capture as `R16G16B16A16_FLOAT` scRGB frames.

## Download

Download the latest `HDRCorrector-vX.Y.Z-win-x64.zip` from [GitHub Releases](https://github.com/mat100payette/HDR-Corrector/releases/latest), extract it, and run `HDRCorrector.exe`.

Each release includes `SHA256SUMS.txt` for verifying the downloaded zip.

## Features

- `Ctrl+PrtScn` captures the selected HDR monitor.
- Saves an HDR `.jxr` screenshot with the original half-float capture.
- Saves a tone-mapped `.preview.png` for SDR apps and uploads.
- Copies a paste-friendly preview to the clipboard, including a PNG/file-drop fallback for stricter apps.
- `Ctrl+Alt+H` opens a tone-mapped "HDR Corrector Stream Mirror" window for apps such as Discord.
- Right-click tray menu for capture, mirror, monitor selection, startup toggle, screenshot folder, and exit.

## Usage

Run `HDRCorrector.exe`. The app starts in the notification area.

Hotkeys:

| Action | Shortcut |
| --- | --- |
| Capture HDR screenshot | `Ctrl+PrtScn` |
| Show or hide stream mirror | `Ctrl+Alt+H` |

For Discord screen share, share the `HDR Corrector Stream Mirror` window instead of sharing the HDR monitor directly.

Screenshots are saved under:

```text
%USERPROFILE%\Pictures\HDRCorrector
```

Logs are written under:

```text
%LOCALAPPDATA%\HDRCorrector\hdr-corrector.log
```

## Why this approach

Discord does not expose a public capture-processing hook, and a normal background executable cannot transparently replace frames inside Discord's private screen-capture pipeline. HDR Corrector therefore provides a clean capture target: an app-owned mirror window produced from the real HDR source.

If the receiving service only accepts SDR video, a local utility cannot force true HDR metadata and HDR transport through that service. In that case the correct no-driver path is high-quality tone mapping from the HDR source into an SDR-compatible mirror. The source display remains HDR, HDR screenshots remain HDR, and the live stream avoids the washed-out Windows HDR capture path.

## How It Works

HDR Corrector keeps the pipeline small and local:

1. It captures the selected monitor with Windows Graphics Capture using `R16G16B16A16_FLOAT`, which preserves the HDR desktop as scRGB half-float pixels.
2. For screenshots, it saves the original HDR frame as `.jxr`, then tone maps the same frame into an SDR `.preview.png`.
3. The SDR preview is copied to the clipboard in multiple formats: Windows bitmap formats, encoded PNG formats, and a file-drop reference to the saved preview PNG for apps that prefer pasted files.
4. For live sharing, it renders the HDR capture stream into a separate D3D11 mirror window with GPU tone mapping. Apps such as Discord can share that window instead of capturing the HDR monitor directly.

## Design

- Native C++/Win32, Windows Graphics Capture, and D3D11.
- No network access.
- No service.
- No kernel driver.
- No Discord injection.
- No HDR display toggling.
- No runtime dependency when built with the provided script and MSVC static runtime on Windows 11.

## Build

Requirements:

- Windows 11.
- Visual Studio Code.
- Visual Studio 2026 Build Tools, or Visual Studio 2026, with the **Desktop development with C++** workload. Visual Studio 2022 with the same workload should also work.
- Recommended VS Code extensions from `.vscode/extensions.json`: Microsoft C/C++ and PowerShell.

Open the workspace in VS Code:

```powershell
code .\HDR-Corrector.code-workspace
```

Useful VS Code actions:

- `Ctrl+Shift+B`: build the Debug configuration.
- `F5`: build and launch `dist\Debug\HDRCorrector.exe` under the Visual Studio Windows debugger.
- `Terminal > Run Task > Build Release`: build the standalone release executable.
- `Terminal > Run Task > Clean`: remove generated build outputs.

Command-line build:

```powershell
.\scripts\build.ps1
```

Build a specific version:

```powershell
.\scripts\build.ps1 -Configuration Release -Version 0.1.0
```

Build outputs:

```text
dist\HDRCorrector.exe          Release, default command-line build
dist\Debug\HDRCorrector.exe    Debug, used by VS Code F5
```

## Package

Create local release artifacts:

```powershell
.\scripts\package.ps1 -Version 0.1.0 -Clean
```

Package outputs:

```text
artifacts\HDRCorrector-v0.1.0-win-x64.zip
artifacts\HDRCorrector-v0.1.0-win-x64-symbols.zip
artifacts\SHA256SUMS.txt
```

## Maintainer Release

The easiest release path is the manual GitHub Actions workflow:

1. Open **Actions > Release > Run workflow**.
2. Choose the branch to release from.
3. Choose `patch`, `minor`, or `major`.
4. Run the workflow.

The workflow finds the latest `vX.Y.Z` tag, computes the next version from the selected bump, updates `VERSION`, commits that version bump, creates the new tag, builds on a clean Windows runner, packages the executable, writes SHA256 hashes, and publishes the GitHub Release.

Version bump examples:

| Latest tag | Bump | New tag |
| --- | --- | --- |
| `v0.1.0` | `patch` | `v0.1.1` |
| `v0.1.0` | `minor` | `v0.2.0` |
| `v0.1.0` | `major` | `v1.0.0` |

Tag pushes are still supported. To release an exact version manually from your machine, commit your changes first, then run:

```powershell
.\scripts\release.ps1 0.1.0 -Push
```

## Code signing

Unsigned Windows executables can show SmartScreen warnings. The release workflow works without a signing certificate, but it will automatically sign the exe before packaging if these repository secrets are configured:

```text
WINDOWS_SIGNING_CERT_PFX_BASE64
WINDOWS_SIGNING_CERT_PASSWORD
```

Create the base64 secret from a `.pfx` certificate:

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\path\to\certificate.pfx"))
```

Local signed packaging is also supported:

```powershell
.\scripts\package.ps1 -Version 0.1.0 -Clean -SignPfxPath C:\path\to\certificate.pfx -SignPfxPassword "pfx-password"
```

## Notes

- The monitor HDR state is never changed.
- Windows shows a colored capture border while Windows Graphics Capture is active. HDR Corrector does not keep capture running while idle, so the border should only appear briefly during a screenshot or while the stream mirror is visible.
- Screenshots are saved as HDR `.jxr` files because PNG is not an HDR desktop screenshot format.
- The clipboard receives a paste-friendly DIB/DIBV5 tone-mapped preview because the standard Windows bitmap clipboard path is SDR.
- For Discord, share the `HDR Corrector Stream Mirror` window. Sharing the HDR monitor directly still uses Discord's own capture path.
