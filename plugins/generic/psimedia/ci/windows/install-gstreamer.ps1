param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('1.24.13', '1.28.7')]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$Root
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force $Root | Out-Null
$Root = (Resolve-Path $Root).Path
$baseUrl = "https://gstreamer.freedesktop.org/pkg/windows/$Version/msvc"

function Get-VerifiedInstaller([string]$Name) {
    $target = Join-Path $Root $Name
    Invoke-WebRequest "$baseUrl/$Name" -OutFile $target
    $checksum = (Invoke-WebRequest "$baseUrl/$Name.sha256sum").Content
    if ($checksum -is [byte[]]) {
        $checksum = [System.Text.Encoding]::UTF8.GetString($checksum)
    }
    $expected = ($checksum.Trim() -split '\s+')[0]
    if ($expected -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Invalid upstream SHA256 for $Name"
    }
    $actual = (Get-FileHash $target -Algorithm SHA256).Hash
    if ($actual -ne $expected) {
        throw "SHA256 mismatch for $Name"
    }
    Write-Host "Verified $Name SHA256=$actual"
    return $target
}

function Invoke-Installer([string]$File, [string[]]$Arguments) {
    $process = Start-Process $File -ArgumentList $Arguments -Wait -PassThru
    if ($process.ExitCode -notin @(0, 3010)) {
        throw "Installer failed: $File, exit $($process.ExitCode)"
    }
}

if ($Version -eq '1.24.13') {
    # Administrative extraction includes all features, including the SDK,
    # without registering either MSI globally on the runner.
    $prefix = Join-Path $Root 'extracted'
    foreach ($kind in @('', 'devel-')) {
        $name = "gstreamer-1.0-${kind}msvc-x86_64-$Version.msi"
        $installer = Get-VerifiedInstaller $name
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
    $installer = Get-VerifiedInstaller "gstreamer-1.0-msvc-x86_64-$Version.exe"
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
