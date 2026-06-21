# HDR Corrector

HDR Corrector is a small native Windows tray utility for the common Windows 11 HDR capture problem: SDR capture tools, screenshot viewers, and Discord screen share often treat HDR desktop pixels incorrectly, producing overly bright or washed-out output.

HDR Corrector does **not** disable HDR on the monitor. It keeps the source display in HDR and captures the desktop through Windows Graphics Capture as `R16G16B16A16_FLOAT` scRGB frames.

- **HDR screenshots:** press `Ctrl+PrtScn`. HDR Corrector copies the screenshot preview to the clipboard, saves a `.jxr` JPEG XR screenshot containing the HDR half-float capture, and writes a tone-mapped `.preview.png` for SDR apps.
- **Live stream mirror:** press `Ctrl+Alt+H`. HDR Corrector opens a separate "HDR Corrector Stream Mirror" window rendered from the HDR capture stream with GPU tone mapping. Share this window in Discord instead of sharing the HDR monitor directly.

## Why this approach

Discord does not expose a public capture-processing hook, and a normal background executable cannot transparently replace frames inside Discord's private screen-capture pipeline. HDR Corrector therefore provides a clean capture target: an app-owned mirror window produced from the real HDR source.

If the receiving service only accepts SDR video, a local utility cannot force true HDR metadata and HDR transport through that service. In that case the correct no-driver path is high-quality tone mapping from the HDR source into an SDR-compatible mirror. The source display remains HDR, HDR screenshots remain HDR, and the live stream avoids the washed-out Windows HDR capture path.

HDR Corrector keeps the implementation intentionally small:

- Native C++/Win32, Windows Graphics Capture, and D3D11.
- No network access.
- No service.
- No kernel driver.
- No Discord injection.
- No HDR display toggling.
- No runtime dependency when built with the provided script and MSVC static runtime on Windows 11.

## Download

Prebuilt Windows x64 packages are published on the GitHub Releases page.

Download the latest `HDRCorrector-vX.Y.Z-win-x64.zip`, extract it, and run `HDRCorrector.exe`.

Each release also includes `SHA256SUMS.txt` so the downloaded zip can be verified.

## VS Code Development

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

## Release

Releases are tag-driven. Commit your changes first, then run:

```powershell
.\scripts\release.ps1 0.1.0 -Push
```

The release script:

- verifies the git worktree is clean;
- builds and packages the release locally;
- creates the annotated tag `v0.1.0`;
- pushes the branch and tag when `-Push` is provided.

Pushing the tag starts the GitHub Actions release workflow. The workflow builds on a clean Windows runner, packages the executable, writes SHA256 hashes, and publishes the GitHub Release.

To create a local tag without pushing it:

```powershell
.\scripts\release.ps1 0.1.0
```

Then push it manually:

```powershell
git push origin main
git push origin v0.1.0
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

## Run

```powershell
.\dist\HDRCorrector.exe
```

The app starts in the notification area. Right-click the tray icon for:

- `Capture HDR screenshot`
- `Show stream mirror`
- `Use monitor under cursor`
- `Run at startup`
- `Open screenshots folder`
- `Exit`

Screenshots are saved under:

```text
%USERPROFILE%\Pictures\HDRCorrector
```

Logs are written under:

```text
%LOCALAPPDATA%\HDRCorrector\hdr-corrector.log
```

## Hotkeys

- `Ctrl+PrtScn`: copy the screenshot preview to the clipboard and save the HDR `.jxr`.
- `Ctrl+Alt+H`: show or hide the stream mirror window.

If another application owns `Ctrl+PrtScn`, use the tray menu instead.

## Notes

- The monitor HDR state is never changed.
- Windows shows a colored capture border while Windows Graphics Capture is active. HDR Corrector does not keep capture running while idle, so the border should only appear briefly during a screenshot or while the stream mirror is visible.
- Screenshots are saved as HDR `.jxr` files because PNG is not an HDR desktop screenshot format.
- The clipboard receives a paste-friendly DIB/DIBV5 tone-mapped preview because the standard Windows bitmap clipboard path is SDR.
- For Discord, share the `HDR Corrector Stream Mirror` window. Sharing the HDR monitor directly still uses Discord's own capture path.
