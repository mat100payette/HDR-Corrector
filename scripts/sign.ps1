param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [string]$CertificateThumbprint,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampServer = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $Path)) {
    throw "File to sign was not found: $Path"
}

if ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -and [string]::IsNullOrWhiteSpace($PfxPath)) {
    throw "Provide either -CertificateThumbprint or -PfxPath."
}

if (![string]::IsNullOrWhiteSpace($CertificateThumbprint) -and ![string]::IsNullOrWhiteSpace($PfxPath)) {
    throw "Use either -CertificateThumbprint or -PfxPath, not both."
}

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidate = Get-ChildItem -LiteralPath $kitsRoot -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1

        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "signtool.exe was not found. Install the Windows SDK."
}

$signTool = Find-SignTool
$signArgs = @("sign", "/fd", "SHA256", "/tr", $TimestampServer, "/td", "SHA256")

if (![string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $signArgs += @("/sha1", $CertificateThumbprint)
} else {
    if (!(Test-Path -LiteralPath $PfxPath)) {
        throw "PFX file was not found: $PfxPath"
    }

    $signArgs += @("/f", $PfxPath)
    if (![string]::IsNullOrWhiteSpace($PfxPassword)) {
        $signArgs += @("/p", $PfxPassword)
    }
}

$signArgs += $Path
& $signTool @signArgs
if ($LASTEXITCODE -ne 0) {
    throw "signtool sign failed with exit code $LASTEXITCODE"
}

& $signTool verify /pa /v $Path
if ($LASTEXITCODE -ne 0) {
    throw "signtool verify failed with exit code $LASTEXITCODE"
}

Write-Host "Signed $Path"
