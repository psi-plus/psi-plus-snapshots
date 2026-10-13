$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$parent = if ($env:PSIMEDIA_TEST_TMPDIR) { $env:PSIMEDIA_TEST_TMPDIR } else { [IO.Path]::GetTempPath() }
$fixture = Join-Path $parent "psimedia-installers-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force $fixture | Out-Null
$oldEnvFile, $oldPathFile, $oldOutputFile = $env:GITHUB_ENV, $env:GITHUB_PATH, $env:GITHUB_OUTPUT

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$global:psiDownloadPayload = [Text.Encoding]::UTF8.GetBytes('verified installer fixture')
$global:psiDownloadCalls = 0
$global:psiDownloadFailures = 0
$global:psiDownloadSleeps = [Collections.Generic.List[int]]::new()
$global:psiInstallerExitCode = 0
$global:psiInstallerCalls = [Collections.Generic.List[object]]::new()
function global:Invoke-WebRequest {
    param([string]$Uri, [string]$OutFile, [int]$ConnectionTimeoutSeconds,
          [int]$OperationTimeoutSeconds, [string]$ErrorAction)
    $global:psiDownloadCalls++
    Assert-True ($ConnectionTimeoutSeconds -eq 30 -and $OperationTimeoutSeconds -eq 600) 'Missing request timeouts'
    [IO.File]::WriteAllBytes($OutFile, $global:psiDownloadPayload)
    if ($global:psiDownloadFailures -gt 0) {
        $global:psiDownloadFailures--
        throw '503 Backend unavailable / ResponseEnded fixture'
    }
}
function global:Start-Sleep([int]$Seconds) { $global:psiDownloadSleeps.Add($Seconds) }
function global:Start-Process {
    param([string]$FilePath, [string[]]$ArgumentList, [switch]$Wait, [switch]$PassThru)
    $global:psiInstallerCalls.Add([pscustomobject]@{ File = $FilePath; Arguments = $ArgumentList })
    Assert-True ($Wait -and $PassThru) 'Installer was not awaited'
    if ($FilePath -eq 'msiexec.exe') {
        Assert-True ($ArgumentList[0] -eq '/a' -and $ArgumentList -contains '/qn') 'Incorrect MSI arguments'
        $target = ($ArgumentList | Where-Object { $_ -like 'TARGETDIR=*' }) -replace '^TARGETDIR="|"$', ''
        $sdk = "$target/gstreamer/msvc_x86_64"
    } else {
        Assert-True ($ArgumentList -contains '/TYPE=devel' -and $ArgumentList -contains '/NORESTART') 'Incorrect Inno arguments'
        $sdk = ($ArgumentList | Where-Object { $_ -like '/DIR=*' }) -replace '^/DIR="|"$', ''
    }
    foreach ($name in @('bin/gst-inspect-1.0.exe', 'lib/pkgconfig/gstreamer-1.0.pc', 'include/gstreamer-1.0/gst/gst.h')) {
        $path = Join-Path $sdk $name
        New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
        [IO.File]::WriteAllText($path, 'SDK fixture')
    }
    return [pscustomobject]@{ ExitCode = $global:psiInstallerExitCode }
}

try {
    foreach ($file in Get-ChildItem "$repo/ci/windows" -Filter '*.ps1') {
        $tokens, $errors = $null, $null
        [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
        Assert-True ($errors.Count -eq 0) "Invalid PowerShell syntax: $($file.Name)"
    }
    . "$repo/ci/windows/download-verified.ps1"
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($global:psiDownloadPayload)).ToLowerInvariant()
    $target = Join-Path $fixture 'download with spaces/installer.exe'
    $global:psiDownloadFailures = 2
    $download = Get-VerifiedDownload -Uri 'https://example.test/installer' -Target $target -Sha256 $hash -Attempts 3
    Assert-True ($download -eq $target -and $global:psiDownloadCalls -eq 3) 'Interrupted download was not retried'
    Assert-True (($global:psiDownloadSleeps -join ',') -eq '10,20') 'Incorrect retry backoff'
    Assert-True (-not (Test-Path "$target.partial")) 'Partial download leaked into cache'

    $global:psiDownloadCalls = 0
    $global:psiDownloadFailures = 99
    Get-VerifiedDownload -Uri 'https://example.test/installer' -Target $target -Sha256 $hash | Out-Null
    Assert-True ($global:psiDownloadCalls -eq 0) 'Verified cache entry contacted unavailable upstream'

    [IO.File]::WriteAllText($target, 'corrupt cached file')
    $global:psiDownloadFailures = 0
    Get-VerifiedDownload -Uri 'https://example.test/installer' -Target $target -Sha256 $hash | Out-Null
    Assert-True ($global:psiDownloadCalls -eq 1) 'Corrupt cache was not replaced'

    $bad = Join-Path $fixture 'wrong-hash.exe'
    $rejected = $false
    try { Get-VerifiedDownload -Uri 'https://example.test/installer' -Target $bad -Sha256 ('0' * 64) | Out-Null }
    catch { Assert-True ($_.Exception.Message -like '*SHA256 mismatch*') 'Unexpected checksum failure'; $rejected = $true }
    Assert-True ($rejected -and -not (Test-Path $bad) -and -not (Test-Path "$bad.partial")) 'Bad checksum was cached'

    $global:psiDownloadCalls = 0
    $global:psiDownloadFailures = 9
    $failed = Join-Path $fixture 'failed.exe'
    $rejected = $false
    try { Get-VerifiedDownload -Uri 'https://example.test/installer' -Target $failed -Sha256 $hash -Attempts 3 | Out-Null }
    catch { $rejected = $true }
    Assert-True ($rejected -and $global:psiDownloadCalls -eq 3) 'Retry limit was not enforced'
    Assert-True (-not (Test-Path $failed) -and -not (Test-Path "$failed.partial")) 'Failed transfer was cached'
    Write-Host 'Download checks passed: retries, timeouts, offline cache, corruption, integrity and retry limit'

    # Exercise both installers against the real resolver and a lock with fixture hashes.
    $lock = Get-Content "$repo/dependencies.lock.json" -Raw | ConvertFrom-Json -AsHashtable
    foreach ($selected in $lock.windows.Values) {
        foreach ($item in $selected.gstreamer.installers) { $item.sha256 = $hash }
    }
    $lockPath = Join-Path $fixture 'dependencies.lock.json'
    $lock | ConvertTo-Json -Depth 10 | Set-Content $lockPath
    foreach ($mode in @('modern', 'win7')) {
        $global:psiDownloadFailures = 0
        $global:psiDownloadCalls = 0
        $global:psiInstallerCalls.Clear()
        $root = Join-Path $fixture "$mode with spaces"
        $env:GITHUB_ENV = Join-Path $fixture "env-$mode.txt"
        $env:GITHUB_PATH = Join-Path $fixture "path-$mode.txt"
        $env:GITHUB_OUTPUT = Join-Path $fixture "output-$mode.txt"
        & "$repo/ci/windows/install-gstreamer.ps1" -BuildProfile $mode -LockPath $lockPath -Root $root
        $expected = if ($mode -eq 'win7') { 2 } else { 1 }
        Assert-True ($global:psiInstallerCalls.Count -eq $expected) "Wrong installer count for $mode"
        Assert-True ($global:psiDownloadCalls -eq $expected) "Installer unexpectedly fetched checksum sidecars for $mode"
        Assert-True ((Get-Content $env:GITHUB_OUTPUT) -contains 'installers_verified=true') 'Verified assets cannot be cached'
        $sdk = (Get-Content $env:GITHUB_ENV | Select-Object -Last 1) -replace '^GSTREAMER_1_0_ROOT_MSVC_X86_64=', ''
        Assert-True (Test-Path "$sdk/include/gstreamer-1.0/gst/gst.h") "Invalid exported SDK for $mode"
        $global:psiDownloadCalls = 0
        & "$repo/ci/windows/install-gstreamer.ps1" -BuildProfile $mode -LockPath $lockPath -Root $root
        Assert-True ($global:psiDownloadCalls -eq 0) "Cached installer repeated network access for $mode"
        Write-Host "Installer checks passed: $mode, locked metadata, paths with spaces and verified cache reuse"
    }
    $global:psiInstallerExitCode = 1603
    $env:GITHUB_OUTPUT = Join-Path $fixture 'failed-install-output.txt'
    $rejected = $false
    try {
        & "$repo/ci/windows/install-gstreamer.ps1" -BuildProfile modern -LockPath $lockPath -Root "$fixture/install-error"
    } catch {
        Assert-True ($_.Exception.Message -like '*Installer failed*') 'Unexpected installer exit failure'
        $rejected = $true
    }
    Assert-True ($rejected -and (Get-Content $env:GITHUB_OUTPUT) -contains 'installers_verified=true') 'Installation failure lost verified cache eligibility'
    $global:psiInstallerExitCode = 0
    $global:psiInstallerCalls.Clear()
    $global:psiDownloadPayload = [Text.Encoding]::UTF8.GetBytes('corrupt installer payload')
    $rejected = $false
    try {
        & "$repo/ci/windows/install-gstreamer.ps1" -BuildProfile modern -LockPath $lockPath -Root "$fixture/bad-sdk"
    } catch {
        Assert-True ($_.Exception.Message -like '*SHA256 mismatch*') 'Unexpected installer verification failure'
        $rejected = $true
    }
    Assert-True ($rejected -and $global:psiInstallerCalls.Count -eq 0) 'Corrupt installer was executed'
} finally {
    $env:GITHUB_ENV, $env:GITHUB_PATH, $env:GITHUB_OUTPUT = $oldEnvFile, $oldPathFile, $oldOutputFile
    Remove-Item $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
