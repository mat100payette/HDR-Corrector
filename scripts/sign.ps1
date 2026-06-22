param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [string]$CertificateThumbprint,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string]$TimestampServer = "http://timestamp.digicert.com",
    [switch]$SkipVerify
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

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

$signTool = Find-WindowsSdkTool -ToolName "signtool.exe"
$signArgs = @("sign", "/fd", "SHA256")
if (![string]::IsNullOrWhiteSpace($TimestampServer)) {
    $signArgs += @("/tr", $TimestampServer, "/td", "SHA256")
}

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

if (!$SkipVerify) {
    & $signTool verify /pa /v $Path
    if ($LASTEXITCODE -ne 0) {
        throw "signtool verify failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Signed $Path"
