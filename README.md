# HDR Corrector

[![CI](https://img.shields.io/github/actions/workflow/status/mat100payette/HDR-Corrector/ci.yml?branch=main&label=CI&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/actions/workflow/status/mat100payette/HDR-Corrector/release.yml?label=release&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/mat100payette/HDR-Corrector?include_prereleases&sort=semver&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases/latest)
[![Download](https://img.shields.io/badge/download-latest-2ea44f?style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases/latest)
[![Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D4?logo=windows11&logoColor=white&style=flat-square)](https://github.com/mat100payette/HDR-Corrector/releases/latest)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white&style=flat-square)](https://github.com/mat100payette/HDR-Corrector)
[![License: MIT](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)

HDR Corrector is a native Windows 11 tray utility that captures HDR desktop content correctly and produces SDR-friendly screenshots and stream mirrors.

It is built for the common Windows HDR capture problem where screenshots, screen share, and SDR viewers treat HDR desktop pixels incorrectly, resulting in overly bright or washed-out output. HDR Corrector keeps the display in HDR and captures the desktop through Windows Graphics Capture as `R16G16B16A16_FLOAT` scRGB frames.

## Download

Download the latest `HDRCorrector-vX.Y.Z-win-x64-setup.exe` from [GitHub Releases](https://github.com/mat100payette/HDR-Corrector/releases/latest), run it, and follow the installer.

Each release includes `SHA256SUMS.txt` for verifying the downloaded artifacts.

The installer is the recommended option. It installs the packaged build for the current Windows user. This packaged build gives HDR Corrector package identity, which lets Windows offer a borderless-capture consent path for the stream mirror.

A portable zip is also available as a fallback for advanced users. It works for screenshots and mirroring, but it cannot use the packaged borderless-capture capability.

## Features

- `Ctrl+PrtScn` captures the selected HDR monitor.
- `Ctrl+Alt+PrtScn` captures only the active window, similar to Windows `Alt+PrtScn`.
- Saves an HDR `.jxr` screenshot with the original half-float capture.
- Saves a tone-mapped `.preview.png` for SDR apps and uploads.
- Copies a paste-friendly preview to the clipboard, including a PNG/file-drop fallback for stricter apps.
- `Ctrl+Alt+H` opens a tone-mapped "HDR Corrector Stream Mirror" window for apps such as Discord.
- Experimental desktop-audio relay for the mirror, enabled by default and toggleable from the tray menu.
- Right-click tray menu for capture, mirror, audio relay, monitor selection, startup toggle, screenshot folder, and exit.

## Usage

Install HDR Corrector, then launch it from the installer or the Start menu. The app starts in the notification area.

Hotkeys:

| Action | Shortcut |
| --- | --- |
| Capture HDR screenshot | `Ctrl+PrtScn` |
| Capture active window | `Ctrl+Alt+PrtScn` |
| Show or hide stream mirror | `Ctrl+Alt+H` |

For Discord screen share, share the `HDR Corrector Stream Mirror` window instead of sharing the HDR monitor directly. The audio relay starts with the mirror by default and republishes desktop audio from HDR Corrector's own process so Discord has an application audio source to capture.

If you hear local echo or doubled audio, right-click the tray icon and uncheck **Relay desktop audio with mirror**. You can also lower or mute **HDR Corrector Stream Audio** in the Windows volume mixer if Discord still receives the stream audio on your setup.

The portable build uses the normal Windows capture border while the mirror is active. The MSIX build requests Windows borderless-capture consent on first capture; if Windows grants it, the mirror can run without the colored capture border. If consent is denied, capture still works with the border.

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
2. For screenshots, it captures either the selected monitor or the active window, saves the original HDR frame as `.jxr`, then tone maps the same frame into an SDR `.preview.png`.
3. The SDR preview is copied to the clipboard in multiple formats: Windows bitmap formats, encoded PNG formats, and a file-drop reference to the saved preview PNG for apps that prefer pasted files.
4. For live sharing, it renders the HDR capture stream into a separate D3D11 mirror window with GPU tone mapping. Apps such as Discord can share that window instead of capturing the HDR monitor directly.
5. When the audio relay is enabled, it uses WASAPI process loopback to capture desktop audio while excluding HDR Corrector's own process, then renders that audio from an HDR Corrector audio session. This gives application-based stream capture a matching audio source without a driver or Discord injection.
6. In the MSIX build, the app declares `graphicsCaptureWithoutBorder` and requests Windows consent for borderless capture before starting a capture session. The portable build cannot use that packaged capability.

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
.\scripts\package.ps1 -Version 0.1.0 -Clean -IncludeMsix
```

Package outputs:

```text
artifacts\HDRCorrector-v0.1.0-win-x64-setup.exe
artifacts\HDRCorrector-v0.1.0-win-x64.zip
artifacts\HDRCorrector-v0.1.0-win-x64-symbols.zip
artifacts\SHA256SUMS.txt
```

The setup exe embeds the MSIX package. If the build uses the local self-signed MSIX certificate, Windows requires that certificate in the Local Machine Trusted People store before the MSIX can install, so users may need to approve Windows security and administrator prompts.

Create only local MSIX artifacts:

```powershell
.\scripts\package-msix.ps1 -Version 0.1.0 -Clean
```

MSIX package outputs:

```text
artifacts\HDRCorrector-v0.1.0-win-x64.msix
artifacts\HDRCorrector-v0.1.0-win-x64-msix.cer
artifacts\Install-HDRCorrector-v0.1.0-win-x64-msix.ps1
```

When no signing certificate is provided, the MSIX script creates a self-signed local package and an install helper script. The public release workflow wraps that MSIX into the setup exe so users do not need to run the helper script themselves. For fewer Windows trust prompts, pass the same trusted signing inputs used by `package.ps1`.

## Maintainer Release

The easiest release path is the manual GitHub Actions workflow:

1. Open **Actions > Release > Run workflow**.
2. Choose the branch to release from.
3. Choose `patch`, `minor`, or `major`.
4. Run the workflow.

The workflow finds the latest `vX.Y.Z` tag, computes the next version from the selected bump, updates `VERSION`, commits that version bump, creates the new tag, builds on a clean Windows runner, packages the installer and portable fallback, writes SHA256 hashes, and publishes the GitHub Release.

Version bump examples:

| Latest tag | Bump | New tag |
| --- | --- | --- |
| `v0.1.0` | `patch` | `v0.1.1` |
| `v0.1.0` | `minor` | `v0.2.0` |
| `v0.1.0` | `major` | `v1.0.0` |

## Code signing

Unsigned Windows executables can show SmartScreen warnings. The release workflow works without a trusted signing certificate by using a self-signed package certificate, but users may see Windows security and administrator prompts. A trusted code-signing certificate is optional and mainly improves the install experience by reducing those warnings.

The workflow automatically signs the app and setup exe before packaging if these repository secrets are configured:

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

An OV code-signing certificate can still need time to build reputation. An EV code-signing certificate or Microsoft Store distribution is the cleaner route when you want the first downloads to avoid the "unrecognized app" warning.

## Contributing

Issues and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for build expectations and project constraints.

Security issues should follow [SECURITY.md](SECURITY.md).

## Notes

- The monitor HDR state is never changed.
- Windows shows a colored capture border while Windows Graphics Capture is active. HDR Corrector does not keep capture running while idle, so the border should only appear briefly during a screenshot or while the stream mirror is visible.
- Screenshots are saved as HDR `.jxr` files because PNG is not an HDR desktop screenshot format.
- The clipboard receives a paste-friendly DIB/DIBV5 tone-mapped preview because the standard Windows bitmap clipboard path is SDR.
- For Discord, share the `HDR Corrector Stream Mirror` window. Sharing the HDR monitor directly still uses Discord's own capture path.
- The audio relay is best-effort because Discord ultimately decides which application audio sessions it captures. It is intentionally easy to disable from the tray menu if a setup produces local echo.

## License

HDR Corrector is released under the [MIT License](LICENSE).
