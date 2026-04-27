# cfrp Windows Universal Uninstaller
# Usage: iex (iwr https://raw.githubusercontent.com/inarms/cfrp/main/scripts/uninstall.ps1).Content -Args "-Mode server"

param (
    [Parameter(Mandatory=$false)]
    [ValidateSet("server", "client", "cli")]
    $Mode = "server"
)

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

$InstallDir = "C:\Program Files\cfrp"
$ServiceName = "cfrp-$Mode"

Write-Host "Uninstalling cfrp $Mode..." -ForegroundColor Cyan

# 1. Stop and Remove Service
if ($Mode -ne "cli") {
    $Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($Service) {
        Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        $Service | Remove-Service -ErrorAction SilentlyContinue
    }
}

# 2. Remove Configs
if (Test-Path $InstallDir) {
    if ($Mode -eq "cli") {
        # For CLI, we just remove the binary if possible
    } else {
        $ConfigPath = Join-Path $InstallDir "$Mode.toml"
        if (Test-Path $ConfigPath) { Remove-Item $ConfigPath -Force }
        if ($Mode -eq "client") {
            $ConfD = Join-Path $InstallDir "config.d"
            if (Test-Path $ConfD) { Remove-Item $ConfD -Recurse -Force }
        }
    }
}

# 3. Remove Binary (if no services remain)
$ServerService = Get-Service -Name "cfrp-server" -ErrorAction SilentlyContinue
$ClientService = Get-Service -Name "cfrp-client" -ErrorAction SilentlyContinue

if ($Mode -eq "cli" -or (-not $ServerService -and -not $ClientService)) {
    Write-Host "Removing $InstallDir..."
    if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
}

Write-Host "--------------------------------------------------" -ForegroundColor Green
Write-Host "cfrp $Mode has been uninstalled."
Write-Host "--------------------------------------------------" -ForegroundColor Green
