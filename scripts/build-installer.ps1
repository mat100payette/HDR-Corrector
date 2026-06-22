param(
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$MsixPath,
    [string]$CertificatePath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$SignCertificateThumbprint,
    [string]$SignPfxPath,
    [string]$SignPfxPassword,
    [string]$TimestampServer = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

function ConvertTo-RcString {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value.Replace("\", "\\").Replace('"', '\"')
}

$versionInfo = Resolve-HdrCorrectorVersion -Version $Version -RepoRoot $repoRoot
$msix = Resolve-Path -LiteralPath $MsixPath
if (!$msix) {
    throw "MSIX package was not found: $MsixPath"
}

$certificate = $null
if (![string]::IsNullOrWhiteSpace($CertificatePath) -and (Test-Path -LiteralPath $CertificatePath)) {
    $certificate = Resolve-Path -LiteralPath $CertificatePath
}

$installerRoot = Join-Path $repoRoot "installer"
$source = Join-Path $installerRoot "setup.cpp"
$resourceHeader = Join-Path $installerRoot "resource.h"
$icon = Join-Path $installerRoot "setup.ico"
if (!(Test-Path -LiteralPath $icon)) {
    $icon = Join-Path $repoRoot "src\app.ico"
}
$buildRoot = Join-Path $repoRoot "build\Installer"
$versionHeader = Join-Path $buildRoot "setup_version.h"
$resourceScript = Join-Path $buildRoot "setup_resources.rc"
$resFile = Join-Path $buildRoot "setup_resources.res"
$pdb = Join-Path $buildRoot "HDRCorrectorSetup.pdb"

if (!(Test-Path -LiteralPath $source)) {
    throw "Installer source was not found: $source"
}
if (!(Test-Path -LiteralPath $resourceHeader)) {
    throw "Installer resource header was not found: $resourceHeader"
}

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

$versionHeaderContent = @"
#define HDRCORRECTOR_INSTALLER_VERSION_MAJOR $($versionInfo.Major)
#define HDRCORRECTOR_INSTALLER_VERSION_MINOR $($versionInfo.Minor)
#define HDRCORRECTOR_INSTALLER_VERSION_PATCH $($versionInfo.Patch)
#define HDRCORRECTOR_INSTALLER_VERSION_BUILD $($versionInfo.Build)
#define HDRCORRECTOR_INSTALLER_VERSION_TEXT "$($versionInfo.Version)"
#define HDRCORRECTOR_INSTALLER_FILE_VERSION_TEXT "$($versionInfo.FileVersion)"
"@
Set-Content -LiteralPath $versionHeader -Value $versionHeaderContent

$resourceLines = @(
    '#include <windows.h>',
    "#include `"$(ConvertTo-RcString $resourceHeader)`"",
    '#include "setup_version.h"',
    '',
    "IDI_SETUP_ICON ICON `"$(ConvertTo-RcString $icon)`"",
    "IDR_SETUP_MSIX RCDATA `"$(ConvertTo-RcString $msix.Path)`""
)

if ($certificate) {
    $resourceLines += "IDR_SETUP_CERTIFICATE RCDATA `"$(ConvertTo-RcString $certificate.Path)`""
}

$resourceLines += @(
    '',
    '1 VERSIONINFO',
    ' FILEVERSION HDRCORRECTOR_INSTALLER_VERSION_MAJOR,HDRCORRECTOR_INSTALLER_VERSION_MINOR,HDRCORRECTOR_INSTALLER_VERSION_PATCH,HDRCORRECTOR_INSTALLER_VERSION_BUILD',
    ' PRODUCTVERSION HDRCORRECTOR_INSTALLER_VERSION_MAJOR,HDRCORRECTOR_INSTALLER_VERSION_MINOR,HDRCORRECTOR_INSTALLER_VERSION_PATCH,HDRCORRECTOR_INSTALLER_VERSION_BUILD',
    ' FILEFLAGSMASK VS_FFI_FILEFLAGSMASK',
    ' FILEFLAGS 0x0L',
    ' FILEOS VOS_NT_WINDOWS32',
    ' FILETYPE VFT_APP',
    ' FILESUBTYPE VFT2_UNKNOWN',
    'BEGIN',
    '    BLOCK "StringFileInfo"',
    '    BEGIN',
    '        BLOCK "040904B0"',
    '        BEGIN',
    '            VALUE "CompanyName", "HDR Corrector contributors\0"',
    '            VALUE "FileDescription", "HDR Corrector Setup\0"',
    '            VALUE "FileVersion", HDRCORRECTOR_INSTALLER_FILE_VERSION_TEXT "\0"',
    '            VALUE "InternalName", "HDRCorrectorSetup\0"',
    '            VALUE "LegalCopyright", "Copyright (C) 2026 HDR Corrector contributors\0"',
    '            VALUE "OriginalFilename", "HDRCorrectorSetup.exe\0"',
    '            VALUE "ProductName", "HDR Corrector\0"',
    '            VALUE "ProductVersion", HDRCORRECTOR_INSTALLER_VERSION_TEXT "\0"',
    '        END',
    '    END',
    '    BLOCK "VarFileInfo"',
    '    BEGIN',
    '        VALUE "Translation", 0x0409, 1200',
    '    END',
    'END'
)
Set-Content -LiteralPath $resourceScript -Value $resourceLines

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio 2022 Build Tools with the Desktop development with C++ workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) {
    throw "No Visual Studio C++ toolchain was found. Install the Desktop development with C++ workload."
}

$vcVars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (!(Test-Path -LiteralPath $vcVars)) {
    throw "vcvars64.bat was not found at $vcVars"
}

$resourceCompile = @(
    "rc.exe",
    "/nologo",
    "/i`"$buildRoot`"",
    "/fo`"$resFile`"",
    "`"$resourceScript`""
) -join " "

$compile = @(
    "cl.exe",
    "/nologo",
    "/std:c++20",
    "/permissive-",
    "/EHsc",
    "/W4",
    "/O2",
    "/MT",
    "/DNDEBUG",
    "/DUNICODE",
    "/D_UNICODE",
    "/I`"$installerRoot`"",
    "/I`"$buildRoot`"",
    "/Fd`"$pdb`"",
    "`"$source`"",
    "`"$resFile`"",
    "/Fe:`"$OutputPath`"",
    "/link",
    "/SUBSYSTEM:WINDOWS",
    "/DEBUG",
    "/INCREMENTAL:NO",
    "/PDB:`"$pdb`"",
    "user32.lib",
    "gdi32.lib",
    "shell32.lib",
    "ole32.lib",
    "oleaut32.lib",
    "comctl32.lib",
    "crypt32.lib",
    "dwmapi.lib",
    "shlwapi.lib",
    "runtimeobject.lib",
    "advapi32.lib"
) -join " "

$command = "call `"$vcVars`" >nul && $resourceCompile && $compile"
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Installer build failed with exit code $LASTEXITCODE"
}

if (![string]::IsNullOrWhiteSpace($SignCertificateThumbprint) -or ![string]::IsNullOrWhiteSpace($SignPfxPath)) {
    $signArgs = @{
        Path = $OutputPath
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

Write-Host "Built installer $OutputPath"
