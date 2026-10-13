param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('modern', 'win7')]
    [string]$BuildProfile,
    [Parameter(Mandatory = $true)]
    [string]$GStreamerVersion,
    [Parameter(Mandatory = $true)]
    [int]$QtMajor,
    [switch]$UseCcache
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit $LASTEXITCODE" }
}

$source = (Get-Location).Path
$gst = $env:GSTREAMER_1_0_ROOT_MSVC_X86_64
$inspect = Join-Path $gst 'bin/gst-inspect-1.0.exe'
$env:PKG_CONFIG_PATH = Join-Path $gst 'lib/pkgconfig'
$env:GST_PLUGIN_SYSTEM_PATH_1_0 = Join-Path $gst 'lib/gstreamer-1.0'
$env:GST_REGISTRY_1_0 = Join-Path $env:RUNNER_TEMP "gst-registry-msvc-$BuildProfile.bin"
$env:PATH = "$gst/bin;$env:QT_ROOT_DIR/bin;$env:PATH"
$versionOutput = & $inspect --version
if ($LASTEXITCODE -ne 0 -or ($versionOutput -join "`n") -notmatch [regex]::Escape($GStreamerVersion)) {
    throw "Unexpected GStreamer runtime: $versionOutput"
}
Invoke-Checked $inspect @('mfvideosrc')

$triplet = if ($BuildProfile -eq 'win7') { 'x64-win7' } else { 'x64-windows' }
$vcpkg = Join-Path $env:VCPKG_INSTALLATION_ROOT 'vcpkg.exe'
# The legacy triplet also builds SRTP/OpenSSL with v142 and a Win7 API baseline.
Invoke-Checked $vcpkg @('install', "libsrtp[openssl]:$triplet", 'pkgconf:x64-windows',
    "--overlay-triplets=$source/ci/windows")
$srtp = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/$triplet"
$pkgconfFiles = @(Get-ChildItem (Join-Path $env:VCPKG_INSTALLATION_ROOT 'installed/x64-windows/tools') `
    -Filter pkgconf.exe -Recurse -File)
if ($pkgconfFiles.Count -ne 1) { throw 'Expected one vcpkg pkgconf executable' }
$env:PATH = "$srtp/bin;$env:PATH"

$baseline = if ($BuildProfile -eq 'win7') { '0x0601' } else { '0x0A00' }
$build = "build-msvc-$BuildProfile"
$stage = Join-Path $source "stage-msvc-$BuildProfile"
# CMake copies module paths into try_compile scripts; Windows backslashes
# become escapes such as \a there. Use forward slashes for every CMake path.
$sourceCmake = $source.Replace('\', '/')
$stageCmake = $stage.Replace('\', '/')
$qtCmake = $env:QT_ROOT_DIR.Replace('\', '/')
$srtpCmake = $srtp.Replace('\', '/')
$pkgconfCmake = $pkgconfFiles[0].FullName.Replace('\', '/')
$configure = @('-S', '.', '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$stageCmake", "-DCMAKE_PREFIX_PATH=$qtCmake",
    "-DQT_DEFAULT_MAJOR_VERSION=$QtMajor", "-DCMAKE_C_FLAGS=/D_WIN32_WINNT=$baseline /DWINVER=$baseline",
    "-DCMAKE_CXX_FLAGS=/D_WIN32_WINNT=$baseline /DWINVER=$baseline",
    "-DPKG_CONFIG_EXECUTABLE=$pkgconfCmake", '-DPKG_CONFIG_ARGN=--define-prefix',
    "-DSRTP_ROOT=$srtpCmake", '-DBUILD_DEMO=OFF', '-DBUILD_PSIPLUGIN=ON',
    '-DPSIMEDIA_BUILD_TESTS=ON', '-DPSIMEDIA_ENABLE_SRTP=ON',
    '-DPSIMEDIA_WINDOWS_VIDEO_SOURCE=mfvideosrc', "-DPLUGINS_ROOT_DIR=$sourceCmake/_psi/plugins")
if ($UseCcache) {
    $ccache = (Get-Command ccache -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    Invoke-Checked $ccache @('--version')
    $ccacheCmake = $ccache.Replace('\', '/')
    $configure += @("-DCMAKE_C_COMPILER_LAUNCHER=$ccacheCmake", "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccacheCmake")
}
Invoke-Checked 'cmake' $configure
Invoke-Checked 'cmake' @('--build', $build, '--parallel', '2')
$env:QT_QPA_PLATFORM = 'offscreen'
$env:GST_DEBUG_NO_COLOR = '1'
Invoke-Checked 'ctest' @('--test-dir', $build, '--output-on-failure', '--no-tests=error',
    '-R', '^psimedia_(payloadinfo|opusrtppay|securertp|securertpgroup|securertpqueue)$', '-j1')
Invoke-Checked 'cmake' @('--install', $build)
$plugins = @(Get-ChildItem $stage -Filter '*mediaplugin.dll' -Recurse -File)
if ($plugins.Count -ne 1) { throw "Expected one installed media plugin, found $($plugins.Count)" }
$deps = & dumpbin.exe /DEPENDENTS $plugins[0].FullName
if ($LASTEXITCODE -ne 0 -or ($deps -join "`n") -notmatch '(?i)srtp.*\.dll') {
    throw "Installed plugin does not import the shared SRTP runtime: $deps"
}
$deps | Set-Content "plugin-dependencies-$BuildProfile.txt"
