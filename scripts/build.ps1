param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$Version,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

$src = Join-Path $repoRoot "src\main.cpp"
$resource = Join-Path $repoRoot "src\version.rc"
$manifest = Join-Path $repoRoot "src\app.manifest"
$distRoot = Join-Path $repoRoot "dist"
$buildRoot = Join-Path $repoRoot "build"
$dist = if ($Configuration -eq "Release") { $distRoot } else { Join-Path $distRoot $Configuration }
$obj = Join-Path $buildRoot $Configuration
$exe = Join-Path $dist "HDRCorrector.exe"
$pdb = Join-Path $dist "HDRCorrector.pdb"
$objFile = Join-Path $obj "main.obj"
$resFile = Join-Path $obj "version.res"
$versionHeader = Join-Path $obj "version_autogen.h"

if ($Clean) {
    Remove-Item -LiteralPath $distRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Cleaned build outputs."
    exit 0
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$versionInfo = Resolve-HdrCorrectorVersion -Version $Version -RepoRoot $repoRoot

$versionHeaderContent = @"
#define HDRCORRECTOR_VERSION_MAJOR $($versionInfo.Major)
#define HDRCORRECTOR_VERSION_MINOR $($versionInfo.Minor)
#define HDRCORRECTOR_VERSION_PATCH $($versionInfo.Patch)
#define HDRCORRECTOR_VERSION_BUILD $($versionInfo.Build)
#define HDRCORRECTOR_VERSION_TEXT "$($versionInfo.Version)"
#define HDRCORRECTOR_FILE_VERSION_TEXT "$($versionInfo.FileVersion)"
#define HDRCORRECTOR_BUILD_CONFIGURATION "$Configuration"
"@
Set-Content -LiteralPath $versionHeader -Value $versionHeaderContent

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

$optimizationFlags = if ($Configuration -eq "Debug") {
    @("/Od", "/Zi", "/MTd", "/DDEBUG", "/Fd`"$pdb`"")
} else {
    @("/O2", "/MT", "/DNDEBUG", "/Zi", "/Fd`"$pdb`"")
}

$compilePrefix = @(
    "cl.exe",
    "/nologo",
    "/std:c++20",
    "/permissive-",
    "/EHsc",
    "/W4",
    "/DUNICODE",
    "/D_UNICODE",
    "/Fo`"$objFile`"",
    "/Fe:`"$exe`""
)

$compileSuffix = @(
    "`"$src`"",
    "`"$resFile`"",
    "/link",
    "/SUBSYSTEM:WINDOWS",
    "/MANIFEST:EMBED",
    "/MANIFESTINPUT:`"$manifest`"",
    "/DEBUG",
    "user32.lib",
    "gdi32.lib",
    "shell32.lib",
    "ole32.lib",
    "oleaut32.lib",
    "uuid.lib",
    "windowscodecs.lib",
    "advapi32.lib",
    "d3d11.lib",
    "dxgi.lib",
    "d3dcompiler.lib",
    "runtimeobject.lib"
)

$resourceCompile = @(
    "rc.exe",
    "/nologo",
    "/i`"$obj`"",
    "/fo`"$resFile`""
)

if ($Configuration -eq "Debug") {
    $resourceCompile += "/dDEBUG"
}

$resourceCompile += "`"$resource`""

$resourceCommand = $resourceCompile -join " "
$compile = ($compilePrefix + $optimizationFlags + $compileSuffix) -join " "

$command = "call `"$vcVars`" >nul && $resourceCommand && $compile"
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Configuration $exe version $($versionInfo.Version)"
