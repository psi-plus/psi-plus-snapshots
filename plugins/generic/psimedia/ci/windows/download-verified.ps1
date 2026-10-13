# Download into a temporary file so failed transfers never become cached installers.
function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$Sha256,
        [ValidateRange(1, 5)][int]$Attempts = 5
    )
    if (Test-Path $Target) {
        if ((Get-FileHash $Target -Algorithm SHA256).Hash -eq $Sha256) {
            Write-Host "Using verified cached installer: $Target"
            return $Target
        }
        Write-Warning "Discarding installer cache entry with incorrect SHA256: $Target"
        Remove-Item $Target -Force
    }
    New-Item -ItemType Directory -Force (Split-Path $Target) | Out-Null
    $partial = "$Target.partial"
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Write-Host "Downloading $Uri (attempt $attempt/$Attempts)"
            Invoke-WebRequest -Uri $Uri -OutFile $partial -ConnectionTimeoutSeconds 30 `
                -OperationTimeoutSeconds 600 -ErrorAction Stop | Out-Null
            break
        } catch {
            Remove-Item $partial -Force -ErrorAction SilentlyContinue
            if ($attempt -eq $Attempts) { throw }
            Write-Warning "Download failed: $($_.Exception.Message); retrying"
            Start-Sleep -Seconds (10 * $attempt)
        }
    }
    if ((Get-FileHash $partial -Algorithm SHA256).Hash -ne $Sha256) {
        Remove-Item $partial -Force
        throw "SHA256 mismatch for $Uri"
    }
    Move-Item $partial $Target -Force
    Write-Host "Verified $Target SHA256=$Sha256"
    return $Target
}
