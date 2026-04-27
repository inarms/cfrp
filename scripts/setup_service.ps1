# cfrp Windows service setup script
# Usage: Run as Administrator
# .\setup_service.ps1 -Mode [server|client] -Config [config_path]

param (
    [Parameter(Mandatory=$false)]
    [ValidateSet("server", "client")]
    $Mode,

    [Parameter(Mandatory=$false)]
    $Config
)

# 1. Elevate to Administrator if not already
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

# 2. Detect Mode
if (-not $Mode) {
    if (Test-Path "server.toml") { $Mode = "server" }
    elseif (Test-Path "client.toml") { $Mode = "client" }
    else {
        Write-Host "Usage: .\setup_service.ps1 -Mode [server|client] [-Config path]" -ForegroundColor Red
        return
    }
}

# 3. Detect Config
if (-not $Config) {
    $Config = Join-Path $PWD "${Mode}.toml"
}
$ConfigPath = Resolve-Path $Config -ErrorAction SilentlyContinue
if (-not $ConfigPath) {
    Write-Error "Configuration file $Config not found."
    return
}

# 4. Detect Binary
$Binary = Join-Path $PWD "cfrp.exe"
if (-not (Test-Path $Binary)) {
    $Binary = Get-Command cfrp.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if (-not $Binary) {
    Write-Error "cfrp.exe not found in current directory or PATH."
    return
}

$InstallDir = "C:\Program Files\cfrp"
$BinaryDest = Join-Path $InstallDir "cfrp.exe"
$ConfigDest = Join-Path $InstallDir "${Mode}.toml"
$ServiceName = "cfrp-$Mode"

Write-Host "Setting up $ServiceName..." -ForegroundColor Cyan

# 5. Copy files
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force
}
Copy-Item $Binary $BinaryDest -Force
Copy-Item $ConfigPath $ConfigDest -Force

if ($Mode -eq "client") {
    New-Item -ItemType Directory -Path (Join-Path $InstallDir "config.d") -Force
}

# 6. Create Service
# Note: Since cfrp doesn't natively implement ServiceMain, we use a trick or just sc.exe.
# For better reliability on Windows, consider using NSSM (Non-Sucking Service Manager).
$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service) {
    Write-Host "Service $ServiceName already exists. Stopping and removing..."
    Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
    # Give it a moment to stop
    Start-Sleep -Seconds 2
    $Service | Remove-Service -ErrorAction SilentlyContinue
}

$Arguments = "`"$ConfigDest`""
New-Service -Name $ServiceName `
            -BinaryPathName "`"$BinaryDest`" $Arguments" `
            -DisplayName "cfrp $Mode Service" `
            -Description "C++ Fast Reverse Proxy $Mode" `
            -StartupType Automatic

# 7. Start Service
Write-Host "Starting $ServiceName..."
Start-Service -Name $ServiceName

Write-Host "-----------------------------------------------" -ForegroundColor Green
Write-Host "cfrp $Mode service has been installed and started."
Write-Host "Service name: $ServiceName"
Write-Host "Install dir:  $InstallDir"
Write-Host "Config file:  $ConfigDest"
Write-Host "Status check: Get-Service $ServiceName"
Write-Host "Logs check:   Check '$InstallDir\cfrp.log' if enabled"
Write-Host "-----------------------------------------------" -ForegroundColor Green
