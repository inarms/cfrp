# cfrp Windows Client Uninstall
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

$ServiceName = "cfrp-client"
$InstallDir = "C:\Program Files\cfrp"

Write-Host "Uninstalling cfrp Client..." -ForegroundColor Cyan
$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service) {
    Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $Service | Remove-Service -ErrorAction SilentlyContinue
}

if (Test-Path $InstallDir) {
    Remove-Item -Path (Join-Path $InstallDir "client.toml") -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $InstallDir "config.d") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $InstallDir "cfrp.exe") -Force -ErrorAction SilentlyContinue
    
    if (-not (Get-ChildItem -Path $InstallDir -ErrorAction SilentlyContinue)) {
        Remove-Item -Path $InstallDir -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "cfrp Client uninstalled." -ForegroundColor Green
