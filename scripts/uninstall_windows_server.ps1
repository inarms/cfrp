# cfrp Windows Server Uninstall
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

$ServiceName = "cfrp-server"
$InstallDir = "C:\Program Files\cfrp"

Write-Host "Uninstalling cfrp Server..." -ForegroundColor Cyan
$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service) {
    Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $Service | Remove-Service -ErrorAction SilentlyContinue
}

if (Test-Path $InstallDir) {
    Remove-Item -Path (Join-Path $InstallDir "server.toml") -Force -ErrorAction SilentlyContinue
    # Only remove exe and dir if server.toml was the only/last config? 
    # For simplicity, we remove the binary as well if it exists.
    Remove-Item -Path (Join-Path $InstallDir "cfrp.exe") -Force -ErrorAction SilentlyContinue
    
    if (-not (Get-ChildItem -Path $InstallDir -ErrorAction SilentlyContinue)) {
        Remove-Item -Path $InstallDir -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "cfrp Server uninstalled." -ForegroundColor Green
