param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,
    [switch]$Push,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

$versionInfo = Resolve-HdrCorrectorVersion -Version $Version -RepoRoot $repoRoot

$git = Get-Command git -ErrorAction Stop
$branch = (& $git.Source -C $repoRoot branch --show-current).Trim()
if ([string]::IsNullOrWhiteSpace($branch)) {
    throw "Cannot release from a detached HEAD. Check out a branch first."
}

$status = & $git.Source -C $repoRoot status --porcelain
if ($status) {
    throw "Working tree is not clean. Commit or stash changes before tagging $($versionInfo.Tag)."
}

$existingTag = (& $git.Source -C $repoRoot tag --list $versionInfo.Tag).Trim()
if ($existingTag) {
    throw "Tag already exists locally: $($versionInfo.Tag)"
}

if (!$SkipPackage) {
    & (Join-Path $PSScriptRoot "package.ps1") -Version $versionInfo.Version -Clean
    if ($LASTEXITCODE -ne 0) {
        throw "Packaging failed with exit code $LASTEXITCODE"
    }
}

& $git.Source -C $repoRoot tag -a $versionInfo.Tag -m "HDR Corrector $($versionInfo.Tag)"
if ($LASTEXITCODE -ne 0) {
    throw "Could not create tag $($versionInfo.Tag)"
}

if ($Push) {
    & $git.Source -C $repoRoot push origin $branch
    if ($LASTEXITCODE -ne 0) {
        throw "Could not push branch $branch"
    }

    & $git.Source -C $repoRoot push origin $versionInfo.Tag
    if ($LASTEXITCODE -ne 0) {
        throw "Could not push tag $($versionInfo.Tag)"
    }

    Write-Host "Pushed $($versionInfo.Tag). GitHub Actions will publish the release."
} else {
    Write-Host "Created local tag $($versionInfo.Tag)."
    Write-Host "Push it when ready:"
    Write-Host "  git push origin $branch"
    Write-Host "  git push origin $($versionInfo.Tag)"
}
