param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('modern', 'win7')]
    [string]$BuildProfile,
    [Parameter(Mandatory = $true)]
    [string]$Root,
    [string]$LockPath = "$PSScriptRoot/../../dependencies.lock.json",
    [string]$CacheDirectory = "$Root/installers"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force $Root | Out-Null
$Root = (Resolve-Path $Root).Path
. "$PSScriptRoot/download-verified.ps1"
$python = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $python) {
    $python = Get-Command python3 -CommandType Application -ErrorAction Stop | Select-Object -First 1
}
$resolved = & $python.Source "$PSScriptRoot/../../.github/scripts/resolve-dependencies.py" `
    --lock-path $LockPath --profile $BuildProfile --json
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve locked GStreamer installers' }
$gst = ($resolved | ConvertFrom-Json).gstreamer
$Version = $gst.version
$installers = @{}
foreach ($item in $gst.installers) {
    $installers[$item.file] = Get-VerifiedDownload -Uri "$($gst.base_url)/$($item.file)" `
        -Target (Join-Path $CacheDirectory $item.file) -Sha256 $item.sha256
}
# All cached files are verified before starting either installer. Save them even
# if SDK installation subsequently fails, avoiding another network download.
if ($env:GITHUB_OUTPUT) { 'installers_verified=true' >> $env:GITHUB_OUTPUT }

function Invoke-Installer([string]$File, [string[]]$Arguments) {
    $process = Start-Process $File -ArgumentList $Arguments -Wait -PassThru
    if ($process.ExitCode -notin @(0, 3010)) {
        throw "Installer failed: $File, exit $($process.ExitCode)"
    }
}

if ($gst.installer_kind -eq 'msi') {
    # Administrative extraction includes all features, including the SDK,
    # without registering either MSI globally on the runner.
    $prefix = Join-Path $Root 'extracted'
    foreach ($kind in @('', 'devel-')) {
        $name = "gstreamer-1.0-${kind}msvc-x86_64-$Version.msi"
        $installer = $installers[$name]
        Invoke-Installer 'msiexec.exe' @('/a', "`"$installer`"", '/qn',
            "TARGETDIR=`"$prefix`"", '/L*v', "`"$Root/$name.log`"")
    }
    $pcFiles = @(Get-ChildItem $prefix -Filter gstreamer-1.0.pc -Recurse -File)
    if ($pcFiles.Count -ne 1) {
        throw "Expected one extracted SDK, found $($pcFiles.Count)"
    }
    $sdk = $pcFiles[0].Directory.Parent.Parent.FullName
} else {
    # GStreamer 1.28 uses one Inno Setup installer for runtime and SDK files.
    $sdk = Join-Path $Root 'sdk'
    $installer = $installers["gstreamer-1.0-msvc-x86_64-$Version.exe"]
    Invoke-Installer $installer @('/VERYSILENT', '/NORESTART', '/CURRENTUSER',
        '/TYPE=devel', "/DIR=`"$sdk`"", "/LOG=`"$Root/install.log`"")
}

foreach ($relative in @('bin/gst-inspect-1.0.exe', 'lib/pkgconfig/gstreamer-1.0.pc',
                        'include/gstreamer-1.0/gst/gst.h')) {
    if (-not (Test-Path (Join-Path $sdk $relative))) {
        throw "Incomplete GStreamer SDK: $relative"
    }
}
"GSTREAMER_1_0_ROOT_MSVC_X86_64=$sdk" >> $env:GITHUB_ENV
"$sdk/bin" >> $env:GITHUB_PATH
Write-Host "Installed MSVC GStreamer $Version at $sdk"
