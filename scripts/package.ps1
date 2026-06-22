param(
    [string]$Version,
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$IncludeMsix,
    [string]$SignCertificateThumbprint,
    [string]$SignPfxPath,
    [string]$SignPfxPassword,
    [string]$TimestampServer = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

$versionInfo = Resolve-HdrCorrectorVersion -Version $Version -RepoRoot $repoRoot
$artifactsRoot = Join-Path $repoRoot "artifacts"
$assetBase = "HDRCorrector-$($versionInfo.Tag)-win-x64"
$staging = Join-Path $artifactsRoot $assetBase
$symbolsStaging = Join-Path $artifactsRoot "$assetBase-symbols"
$zip = Join-Path $artifactsRoot "$assetBase.zip"
$symbolsZip = Join-Path $artifactsRoot "$assetBase-symbols.zip"
$setup = Join-Path $artifactsRoot "$assetBase-setup.exe"
$msix = Join-Path $artifactsRoot "$assetBase.msix"
$msixCertificate = Join-Path $artifactsRoot "$assetBase-msix.cer"
$msixInstallScript = Join-Path $artifactsRoot "Install-$assetBase-msix.ps1"
$checksums = Join-Path $artifactsRoot "SHA256SUMS.txt"

if ($Clean) {
    Remove-Item -LiteralPath $artifactsRoot -Recurse -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $artifactsRoot | Out-Null

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration Release -Version $versionInfo.Version
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE"
    }
}

$exe = Join-Path $repoRoot "dist\HDRCorrector.exe"
$pdb = Join-Path $repoRoot "dist\HDRCorrector.pdb"
if (!(Test-Path -LiteralPath $exe)) {
    throw "Release executable was not found: $exe"
}

if (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint) -or ![string]::IsNullOrWhiteSpace($SignPfxPath)) {
    $signArgs = @{
        Path = $exe
        TimestampServer = $TimestampServer
    }
    if (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint)) {
        $signArgs["CertificateThumbprint"] = $SignCertificateThumbprint
    }
    if (![string]::IsNullOrWhiteSpace($SignPfxPath)) {
        $signArgs["PfxPath"] = $SignPfxPath
    }
    if (![string]::IsNullOrWhiteSpace($SignPfxPassword)) {
        $signArgs["PfxPassword"] = $SignPfxPassword
    }

    & (Join-Path $PSScriptRoot "sign.ps1") @signArgs
}

Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $symbolsStaging -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $setup -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $symbolsZip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $msix -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $msixCertificate -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $msixInstallScript -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $checksums -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $staging | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $staging "HDRCorrector.exe")
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $staging "README.md")
Set-Content -LiteralPath (Join-Path $staging "VERSION.txt") -Value $versionInfo.Version

$license = Join-Path $repoRoot "LICENSE"
if (Test-Path -LiteralPath $license) {
    Copy-Item -LiteralPath $license -Destination (Join-Path $staging "LICENSE")
}

Compress-Archive -LiteralPath $staging -DestinationPath $zip -Force

$checksumTargets = @($zip)
if (Test-Path -LiteralPath $pdb) {
    New-Item -ItemType Directory -Force -Path $symbolsStaging | Out-Null
    Copy-Item -LiteralPath $pdb -Destination (Join-Path $symbolsStaging "HDRCorrector.pdb")
    Set-Content -LiteralPath (Join-Path $symbolsStaging "VERSION.txt") -Value $versionInfo.Version
    Compress-Archive -LiteralPath $symbolsStaging -DestinationPath $symbolsZip -Force
    $checksumTargets += $symbolsZip
}

if ($IncludeMsix) {
    $msixArgs = @{
        Version = $versionInfo.Version
        SkipBuild = $true
        Clean = $true
        TimestampServer = $TimestampServer
    }
    if (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint)) {
        $msixArgs["SignCertificateThumbprint"] = $SignCertificateThumbprint
    }
    if (![string]::IsNullOrWhiteSpace($SignPfxPath)) {
        $msixArgs["SignPfxPath"] = $SignPfxPath
    }
    if (![string]::IsNullOrWhiteSpace($SignPfxPassword)) {
        $msixArgs["SignPfxPassword"] = $SignPfxPassword
    }

    & (Join-Path $PSScriptRoot "package-msix.ps1") @msixArgs

    if (Test-Path -LiteralPath $msix) {
        $installerArgs = @{
            Version = $versionInfo.Version
            MsixPath = $msix
            OutputPath = $setup
            TimestampServer = $TimestampServer
        }
        if (Test-Path -LiteralPath $msixCertificate) {
            $installerArgs["CertificatePath"] = $msixCertificate
        }
        if (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint)) {
            $installerArgs["SignCertificateThumbprint"] = $SignCertificateThumbprint
        }
        if (![string]::IsNullOrWhiteSpace($SignPfxPath)) {
            $installerArgs["SignPfxPath"] = $SignPfxPath
        }
        if (![string]::IsNullOrWhiteSpace($SignPfxPassword)) {
            $installerArgs["SignPfxPassword"] = $SignPfxPassword
        }

        & (Join-Path $PSScriptRoot "build-installer.ps1") @installerArgs
        $checksumTargets = @($setup) + $checksumTargets
    }
}

$checksumLines = foreach ($target in $checksumTargets) {
    $hash = Get-FileHash -LiteralPath $target -Algorithm SHA256
    "$($hash.Hash.ToLowerInvariant())  $(Split-Path -Leaf $target)"
}
Set-Content -LiteralPath $checksums -Value $checksumLines

Write-Host "Created release artifacts:"
if (Test-Path -LiteralPath $setup) {
    Write-Host "  $setup"
}
Write-Host "  $zip"
if (Test-Path -LiteralPath $symbolsZip) {
    Write-Host "  $symbolsZip"
}
Write-Host "  $checksums"

if (Test-Path -LiteralPath $msix) {
    Write-Host "Created installer staging artifacts:"
    Write-Host "  $msix"
    if (Test-Path -LiteralPath $msixCertificate) {
        Write-Host "  $msixCertificate"
    }
    if (Test-Path -LiteralPath $msixInstallScript) {
        Write-Host "  $msixInstallScript"
    }
}
