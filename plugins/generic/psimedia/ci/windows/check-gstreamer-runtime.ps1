param(
    [Parameter(Mandatory = $true)]
    [string]$Stage,
    [Parameter(Mandatory = $true)]
    [string]$Sdk
)

# Checks the GStreamer runtime installed by PSIMEDIA_INSTALL_GSTREAMER_RUNTIME:
# 1. the element -> plugin table in cmake/modules/PsimediaGStreamerRuntime.cmake matches the SDK;
# 2. every installed element loads from the package alone, without the SDK on PATH.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$manifests = @(Get-ChildItem $Stage -Filter gstreamer-runtime.txt -Recurse -File)
if ($manifests.Count -ne 1) { throw "Expected one gstreamer-runtime.txt in $Stage, found $($manifests.Count)" }
$root = $manifests[0].Directory.FullName
$pluginDir = Join-Path $root 'gstreamer-1.0'
$entries = @(Get-Content $manifests[0].FullName | Where-Object { $_ -match '^\S+ \S+ (installed|absent)$' } |
    ForEach-Object {
        $parts = $_ -split ' '
        [pscustomobject]@{ Element = $parts[0]; Plugin = $parts[1]; Status = $parts[2] }
    })
$installed = @($entries | Where-Object Status -eq 'installed')
Write-Host "Manifest: $($installed.Count) installed, $(@($entries | Where-Object Status -eq 'absent').Count) absent"

# 1. The table against the SDK.
$sdkInspect = Join-Path $Sdk 'bin/gst-inspect-1.0.exe'
$mismatches = @()
foreach ($entry in $installed | Where-Object { -not $_.Element.StartsWith('(') }) {
    $output = & $sdkInspect $entry.Element 2>&1
    if ($LASTEXITCODE -ne 0) {
        $mismatches += "$($entry.Element): not in the SDK"
        continue
    }
    $match = $output | Select-String '^\s*Filename\s+(.+)$' | Select-Object -First 1
    $file = if ($match) { [IO.Path]::GetFileName($match.Matches[0].Groups[1].Value.Trim()) } else { '?' }
    if ($file -ne "gst$($entry.Plugin).dll") {
        $mismatches += "$($entry.Element): provided by $file, the table says gst$($entry.Plugin).dll"
    }
}
if ($mismatches) { throw "Element table does not match the SDK:`n$($mismatches -join "`n")" }

# 2. The package alone: its DLLs, its plugins, Windows itself.
$isolated = Join-Path $env:RUNNER_TEMP 'gstreamer-runtime-isolated'
Remove-Item $isolated -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $isolated | Out-Null
Copy-Item (Join-Path $root '*.dll') $isolated
Copy-Item $sdkInspect $isolated
$env:PATH = "$isolated;$env:SystemRoot\System32;$env:SystemRoot"
foreach ($name in @('GST_PLUGIN_PATH', 'GST_PLUGIN_PATH_1_0', 'GST_PLUGIN_SYSTEM_PATH')) {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}
$env:GST_PLUGIN_SYSTEM_PATH_1_0 = $pluginDir
$env:GST_REGISTRY_1_0 = Join-Path $isolated 'registry.bin'
$inspect = Join-Path $isolated 'gst-inspect-1.0.exe'
$failed = @()
foreach ($entry in $installed) {
    $name = if ($entry.Element.StartsWith('(')) { $entry.Plugin } else { $entry.Element }
    & $inspect $name *> $null
    if ($LASTEXITCODE -ne 0) { $failed += "$name ($($entry.Plugin))" }
}
$blacklist = & $inspect -b 2>&1
if ($failed) {
    throw "Not loadable from the package alone: $($failed -join ', ')`n$($blacklist -join "`n")"
}
Write-Host "All $($installed.Count) installed elements load from the package alone"
Get-Content $manifests[0].FullName
