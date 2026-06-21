$ErrorActionPreference = "Stop"

function Resolve-HdrCorrectorVersion {
    param(
        [string]$Version,
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $versionPath = Join-Path $RepoRoot "VERSION"
        if (!(Test-Path -LiteralPath $versionPath)) {
            throw "No version was provided and VERSION was not found."
        }

        $Version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    }

    $normalized = $Version.Trim()
    if ($normalized.StartsWith("v", [System.StringComparison]::OrdinalIgnoreCase)) {
        $normalized = $normalized.Substring(1)
    }

    if ($normalized -notmatch '^(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?$') {
        throw "Version must be numeric SemVer, for example 0.1.0 or 0.1.0.0."
    }

    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
    $build = if ($Matches[4]) { [int]$Matches[4] } else { 0 }

    [pscustomobject]@{
        Version = $normalized
        Tag = "v$normalized"
        Major = $major
        Minor = $minor
        Patch = $patch
        Build = $build
        FileVersion = "$major.$minor.$patch.$build"
    }
}
