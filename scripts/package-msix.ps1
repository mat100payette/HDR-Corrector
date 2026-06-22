param(
    [string]$Version,
    [switch]$Clean,
    [switch]$SkipBuild,
    [string]$SignCertificateThumbprint,
    [string]$SignPfxPath,
    [string]$SignPfxPassword,
    [string]$TimestampServer = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

function Get-PfxSubject {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [string]$Password
    )

    if (!(Test-Path -LiteralPath $Path)) {
        throw "PFX file was not found: $Path"
    }

    $flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($Path, $Password, $flags)
    try {
        return $certificate.Subject
    } finally {
        $certificate.Dispose()
    }
}

function Get-StoreCertificateSubject {
    param([Parameter(Mandatory = $true)][string]$Thumbprint)

    $normalized = $Thumbprint.Replace(" ", "").ToUpperInvariant()
    $certificate = Get-ChildItem -Path Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $normalized } |
        Select-Object -First 1

    if (!$certificate) {
        throw "Certificate thumbprint was not found in CurrentUser\My or LocalMachine\My: $Thumbprint"
    }

    return $certificate.Subject
}

function Export-IconPng {
    param(
        [Parameter(Mandatory = $true)]
        [string]$IconPath,
        [Parameter(Mandatory = $true)]
        [string]$OutputPath,
        [Parameter(Mandatory = $true)]
        [int]$Size
    )

    Add-Type -AssemblyName System.Drawing

    $sourceImage = $null
    $sourceStream = $null
    $icon = $null
    try {
        $bytes = [System.IO.File]::ReadAllBytes($IconPath)
        $entries = @()
        if ($bytes.Length -ge 6 -and [System.BitConverter]::ToUInt16($bytes, 0) -eq 0 -and [System.BitConverter]::ToUInt16($bytes, 2) -eq 1) {
            $entryCount = [System.BitConverter]::ToUInt16($bytes, 4)
            for ($index = 0; $index -lt $entryCount; $index++) {
                $entryOffset = 6 + ($index * 16)
                if ($entryOffset + 16 -gt $bytes.Length) {
                    break
                }

                $width = if ($bytes[$entryOffset] -eq 0) { 256 } else { [int]$bytes[$entryOffset] }
                $height = if ($bytes[$entryOffset + 1] -eq 0) { 256 } else { [int]$bytes[$entryOffset + 1] }
                $imageLength = [System.BitConverter]::ToUInt32($bytes, $entryOffset + 8)
                $imageOffset = [System.BitConverter]::ToUInt32($bytes, $entryOffset + 12)
                $isPng = $imageOffset + 8 -le $bytes.Length -and
                    $bytes[$imageOffset] -eq 0x89 -and $bytes[$imageOffset + 1] -eq 0x50 -and
                    $bytes[$imageOffset + 2] -eq 0x4E -and $bytes[$imageOffset + 3] -eq 0x47

                if ($isPng -and $imageOffset + $imageLength -le $bytes.Length) {
                    $entries += [pscustomobject]@{
                        Width = $width
                        Height = $height
                        Length = [int]$imageLength
                        Offset = [int]$imageOffset
                    }
                }
            }
        }

        $entry = $entries |
            Where-Object { $_.Width -ge $Size -and $_.Height -ge $Size } |
            Sort-Object Width |
            Select-Object -First 1

        if (!$entry) {
            $entry = $entries |
                Sort-Object Width -Descending |
                Select-Object -First 1
        }

        if ($entry) {
            $sourceStream = [System.IO.MemoryStream]::new($bytes, $entry.Offset, $entry.Length, $false)
            $sourceImage = [System.Drawing.Image]::FromStream($sourceStream)
        } else {
            $icon = [System.Drawing.Icon]::new($IconPath, $Size, $Size)
            $sourceImage = $icon.ToBitmap()
        }

        $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.DrawImage($sourceImage, 0, 0, $Size, $Size)
            } finally {
                $graphics.Dispose()
            }

            $bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $bitmap.Dispose()
        }
    } finally {
        if ($sourceImage) {
            $sourceImage.Dispose()
        }
        if ($sourceStream) {
            $sourceStream.Dispose()
        }
        if ($icon) {
            $icon.Dispose()
        }
    }
}

function New-LocalSigningPfx {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Subject,
        [Parameter(Mandatory = $true)]
        [string]$CerPath,
        [Parameter(Mandatory = $true)]
        [string]$PfxPath,
        [Parameter(Mandatory = $true)]
        [string]$Password
    )

    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    $certificate = $null
    try {
        $distinguishedName = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new($Subject)
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            $distinguishedName,
            $rsa,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $false))
        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,
                $false))

        $codeSigningOids = [System.Security.Cryptography.OidCollection]::new()
        [void]$codeSigningOids.Add([System.Security.Cryptography.Oid]::new("1.3.6.1.5.5.7.3.3"))
        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($codeSigningOids, $false))

        $certificate = $request.CreateSelfSigned(
            [System.DateTimeOffset]::UtcNow.AddMinutes(-5),
            [System.DateTimeOffset]::UtcNow.AddYears(3))

        [System.IO.File]::WriteAllBytes(
            $CerPath,
            $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        [System.IO.File]::WriteAllBytes(
            $PfxPath,
            $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $Password))
    } finally {
        if ($certificate) {
            $certificate.Dispose()
        }
        $rsa.Dispose()
    }
}

function New-InstallScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$PackageFileName,
        [Parameter(Mandatory = $true)]
        [string]$CertificateFileName
    )

    $content = @"
`$ErrorActionPreference = "Stop"

`$root = Split-Path -Parent `$MyInvocation.MyCommand.Path
`$package = Join-Path `$root "$PackageFileName"
`$certificate = Join-Path `$root "$CertificateFileName"

if (!(Test-Path -LiteralPath `$package)) {
    throw "MSIX package was not found next to this script: `$package"
}

if (Test-Path -LiteralPath `$certificate) {
    Write-Host "Trusting HDR Corrector's local signing certificate for the current user..."
    `$certificateObject = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(`$certificate)
    try {
        foreach (`$storeName in @("Root", "TrustedPeople")) {
            `$store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
                `$storeName,
                [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser)
            `$store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
            try {
                `$existing = `$store.Certificates.Find(
                    [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                    `$certificateObject.Thumbprint,
                    `$false)
                if (`$existing.Count -eq 0) {
                    `$store.Add(`$certificateObject)
                }
            } finally {
                `$store.Close()
            }
        }
    } finally {
        `$certificateObject.Dispose()
    }
}

Write-Host "Installing HDR Corrector..."
Add-AppxPackage -Path `$package
Write-Host "HDR Corrector installed."
"@

    Set-Content -LiteralPath $Path -Value $content
}

$versionInfo = Resolve-HdrCorrectorVersion -Version $Version -RepoRoot $repoRoot
$artifactsRoot = Join-Path $repoRoot "artifacts"
$buildRoot = Join-Path $repoRoot "build\msix"
$layout = Join-Path $buildRoot "layout"
$assetBase = "HDRCorrector-$($versionInfo.Tag)-win-x64"
$msix = Join-Path $artifactsRoot "$assetBase.msix"
$cer = Join-Path $artifactsRoot "$assetBase-msix.cer"
$installScript = Join-Path $artifactsRoot "Install-$assetBase-msix.ps1"

if ($Clean) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $msix -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $cer -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $installScript -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $artifactsRoot | Out-Null

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration Release -Version $versionInfo.Version
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE"
    }
}

$exe = Join-Path $repoRoot "dist\HDRCorrector.exe"
if (!(Test-Path -LiteralPath $exe)) {
    throw "Release executable was not found: $exe"
}

$publisher = $null
$selfSigned = $false
$selfSignedPfx = Join-Path $buildRoot "HDRCorrectorLocalMsix.pfx"
$selfSignedPfxPassword = $null
if (![string]::IsNullOrWhiteSpace($SignPfxPath)) {
    $publisher = Get-PfxSubject -Path $SignPfxPath -Password $SignPfxPassword
} elseif (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint)) {
    $publisher = Get-StoreCertificateSubject -Thumbprint $SignCertificateThumbprint
} else {
    $selfSigned = $true
    $publisher = "CN=HDR Corrector Local MSIX"
    $selfSignedPfxPassword = "$([System.Guid]::NewGuid().ToString("N"))$([System.Guid]::NewGuid().ToString("N"))"
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
    New-LocalSigningPfx -Subject $publisher -CerPath $cer -PfxPath $selfSignedPfx -Password $selfSignedPfxPassword
    New-InstallScript -Path $installScript -PackageFileName (Split-Path -Leaf $msix) -CertificateFileName (Split-Path -Leaf $cer)
}

Remove-Item -LiteralPath $layout -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $msix -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $layout | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $layout "Assets") | Out-Null

Copy-Item -LiteralPath $exe -Destination (Join-Path $layout "HDRCorrector.exe")
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $layout "README.md")
Set-Content -LiteralPath (Join-Path $layout "VERSION.txt") -Value $versionInfo.Version

$license = Join-Path $repoRoot "LICENSE"
if (Test-Path -LiteralPath $license) {
    Copy-Item -LiteralPath $license -Destination (Join-Path $layout "LICENSE")
}

$icon = Join-Path $repoRoot "src\app.ico"
Export-IconPng -IconPath $icon -OutputPath (Join-Path $layout "Assets\Square44x44Logo.png") -Size 44
Export-IconPng -IconPath $icon -OutputPath (Join-Path $layout "Assets\Square150x150Logo.png") -Size 150
Export-IconPng -IconPath $icon -OutputPath (Join-Path $layout "Assets\StoreLogo.png") -Size 50

$manifestTemplate = Get-Content -LiteralPath (Join-Path $repoRoot "packaging\AppxManifest.xml.template") -Raw
$manifest = $manifestTemplate.Replace("@@PUBLISHER@@", [System.Security.SecurityElement]::Escape($publisher))
$manifest = $manifest.Replace("@@FILE_VERSION@@", $versionInfo.FileVersion)
Set-Content -LiteralPath (Join-Path $layout "AppxManifest.xml") -Value $manifest

$makeAppx = Find-WindowsSdkTool -ToolName "makeappx.exe"
& $makeAppx pack /d $layout /p $msix /o
if ($LASTEXITCODE -ne 0) {
    throw "makeappx pack failed with exit code $LASTEXITCODE"
}

if ($selfSigned) {
    & (Join-Path $PSScriptRoot "sign.ps1") -Path $msix -PfxPath $selfSignedPfx -PfxPassword $selfSignedPfxPassword -TimestampServer "" -SkipVerify
} else {
    $signArgs = @{
        Path = $msix
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

Write-Host "Created MSIX artifact:"
Write-Host "  $msix"
if ($selfSigned) {
    Write-Host "  $cer"
    Write-Host "  $installScript"
}
