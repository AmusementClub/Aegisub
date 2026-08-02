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

function Get-BuildTimeUtc {
    if ($env:SOURCE_DATE_EPOCH -match '^\d+$') {
        $epochBase = [DateTime]::SpecifyKind([DateTime]::Parse('1970-01-01'), [DateTimeKind]::Utc)
        return $epochBase.AddSeconds([int64]$env:SOURCE_DATE_EPOCH).ToString("yyyyMMdd'T'HHmmss'Z'")
    }

    return (Get-Date).ToUniversalTime().ToString("yyyyMMdd'T'HHmmss'Z'")
}

$buildTimeUtc = Get-BuildTimeUtc

if (-not (Test-Path (Join-Path $repoRootResolved '.git'))) {
    $forcedVersion = $env:FORCE_GIT_VERSION
    if ($forcedVersion) {
        $content = @(
            '#define BUILD_GIT_VERSION_NUMBER 0',
            "#define BUILD_GIT_VERSION_STRING `"$($forcedVersion.Trim())`"",
            "#define BUILD_GIT_BUILD_TIME_UTC `"$buildTimeUtc`"",
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
    $gitVersion = "$gitRevision-$gitBranch-$gitHash-$buildTimeUtc"
}

$header = @(
    "#define BUILD_GIT_VERSION_NUMBER $gitRevision",
    "#define BUILD_GIT_VERSION_STRING `"$gitVersion`"",
    "#define BUILD_GIT_BUILD_TIME_UTC `"$buildTimeUtc`"",
    "#define TAGGED_RELEASE $taggedRelease",
    "#define INSTALLER_VERSION `"$installerVersion`"",
    "#define RESOURCE_BASE_VERSION $resourceVersion"
) -join "`n"

if (-not (Test-Path $headerPath) -or (Get-Content $headerPath -Raw) -ne ($header + "`n")) {
    Set-Content -Path $headerPath -Value $header -Encoding Ascii
}
