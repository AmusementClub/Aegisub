param(
    [string]$RepoRoot = '.'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$lastSvnRevision = 6962
$lastSvnHash = '16cd907fe7482cb54a7374cd28b8501f138116be'

$repoRootResolved = [System.IO.Path]::GetFullPath($RepoRoot)
$buildDir = Join-Path $repoRootResolved 'build'
$headerPath = Join-Path $buildDir 'git_version.h'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Get-GitText([string[]]$Arguments) {
    $result = & git -C $repoRootResolved @Arguments 2>$null
    if ($LASTEXITCODE -ne 0 -or $null -eq $result) {
        return ''
    }

    return [string]::Join("`n", @($result)).Trim()
}

# Resolve the build moment as UTC epoch seconds, honoring SOURCE_DATE_EPOCH for
# reproducible builds (https://reproducible-builds.org/docs/source-date-epoch/).
# Computed from a literal 1970-01-01T00:00:00Z instead of [DateTimeOffset]::UnixEpoch:
# that static is absent from some PowerShell/.NET combinations, so using it would
# break the version step on those hosts while producing the same value here.
function Get-UnixEpoch {
    [DateTimeOffset]::new([DateTime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc),
                          [TimeSpan]::Zero)
}

function Get-BuildEpochSeconds {
    if ($env:SOURCE_DATE_EPOCH -match '^\d+$') {
        return [int64]$env:SOURCE_DATE_EPOCH
    }
    $now = [DateTimeOffset]::new([DateTime]::UtcNow, [TimeSpan]::Zero)
    return [int64][Math]::Floor(($now - (Get-UnixEpoch)).TotalSeconds)
}

# Parse BUILD_TIME_OFFSET (e.g. "8", "+8", "-5", "5:30", "+05:30") into signed
# minutes. Defaults to +480 (UTC+8) when unset or empty.
function Get-OffsetMinutes {
    $raw = $env:BUILD_TIME_OFFSET
    if ([string]::IsNullOrWhiteSpace($raw)) { $raw = '8' }
    if ($raw -notmatch '^\s*([+-]?)(\d+)(?::(\d{1,2}))?\s*$') {
        Write-Warning "BUILD_TIME_OFFSET='$raw' is not a valid offset (e.g. 8, -5, 5:30); falling back to +8."
        $raw = '8'
        if ($raw -notmatch '^\s*([+-]?)(\d+)(?::(\d{1,2}))?\s*$') { return 480 }
    }
    $signStr = $matches[1]; if ($signStr -eq '-') { $sign = -1 } else { $sign = 1 }
    $hours = [int]$matches[2]
    $mins = 0; if ($matches[3]) { $mins = [int]$matches[3] }
    return $sign * ($hours * 60 + $mins)
}

function Format-EpochLocal {
    param([int64]$EpochSeconds, [int]$OffsetMinutes)
    $moment = (Get-UnixEpoch).AddSeconds($EpochSeconds + $OffsetMinutes * 60)
    $base = $moment.ToString("yyyyMMdd'T'HHmmss")
    if ($OffsetMinutes -eq 0) { return "${base}Z" }
    $sign = if ($OffsetMinutes -lt 0) { '-' } else { '+' }
    $absMin = [math]::Abs($OffsetMinutes)
    $h = [int][math]::Floor($absMin / 60.0)
    $m = $absMin % 60
    return ($base + $sign + ('{0:D2}{1:D2}' -f $h, $m))
}

$buildEpoch = Get-BuildEpochSeconds
$offsetMinutes = Get-OffsetMinutes
$buildTimeUtc = Format-EpochLocal -EpochSeconds $buildEpoch -OffsetMinutes 0
$buildTime = Format-EpochLocal -EpochSeconds $buildEpoch -OffsetMinutes $offsetMinutes

if (-not (Test-Path (Join-Path $repoRootResolved '.git'))) {
    $forcedVersion = $env:FORCE_GIT_VERSION
    if ($forcedVersion) {
        $content = @(
            '#define BUILD_GIT_VERSION_NUMBER 0',
            "#define BUILD_GIT_VERSION_STRING `"$($forcedVersion.Trim())`"",
            "#define BUILD_GIT_BUILD_TIME_UTC `"$buildTimeUtc`"",
            "#define BUILD_GIT_BUILD_TIME `"$buildTime`"",
            '#define TAGGED_RELEASE 0',
            '#define INSTALLER_VERSION "0.0.0"',
            '#define RESOURCE_BASE_VERSION 0, 0, 0'
        ) -join "`n"
        if (-not (Test-Path $headerPath) -or (Get-Content $headerPath -Raw) -ne ($content + "`n")) {
            Set-Content -Path $headerPath -Value $content -Encoding Ascii
        }
        exit 0
    }

    if (Test-Path $headerPath) {
        exit 0
    }

    throw 'git repo not found and no cached git_version.h - use FORCE_GIT_VERSION to override'
}

$revCountText = Get-GitText @('rev-list', '--count', "$lastSvnHash..HEAD")
if ($revCountText -eq '') {
    Write-Warning "Could not count commits since $lastSvnHash (shallow clone or missing history?). Version number will be 0. Use fetch-depth: 0 in CI."
    $gitRevision = 0
}
else {
    $gitRevision = $lastSvnRevision + [int]$revCountText
    if ($gitRevision -eq $lastSvnRevision) {
        $gitRevision = 0
    }
}

$gitVersion = Get-GitText @('describe', '--exact-match')
$installerVersion = '0.0.0'
$resourceVersion = '0, 0, 0'
$taggedRelease = 0

if ($gitVersion) {
    $gitVersion = $gitVersion.TrimStart('v')
    $taggedRelease = 1
    if ($gitVersion -match '^\d+\.\d+\.\d+$') {
        $installerVersion = $gitVersion
        $resourceVersion = $gitVersion -replace '\.', ', '
    }
}
else {
    $gitBranch = Get-GitText @('symbolic-ref', 'HEAD')
    if ($gitBranch) {
        $gitBranch = $gitBranch -replace '^refs/heads/', ''
    }
    else {
        $gitBranch = '(unnamed branch)'
    }
    $gitHash = Get-GitText @('rev-parse', '--short', 'HEAD')
    $gitVersion = "$gitRevision-$gitBranch-$gitHash-$buildTime"
}

$header = @(
    "#define BUILD_GIT_VERSION_NUMBER $gitRevision",
    "#define BUILD_GIT_VERSION_STRING `"$gitVersion`"",
    "#define BUILD_GIT_BUILD_TIME_UTC `"$buildTimeUtc`"",
    "#define BUILD_GIT_BUILD_TIME `"$buildTime`"",
    "#define TAGGED_RELEASE $taggedRelease",
    "#define INSTALLER_VERSION `"$installerVersion`"",
    "#define RESOURCE_BASE_VERSION $resourceVersion"
) -join "`n"

if (-not (Test-Path $headerPath) -or (Get-Content $headerPath -Raw) -ne ($header + "`n")) {
    Set-Content -Path $headerPath -Value $header -Encoding Ascii
}
