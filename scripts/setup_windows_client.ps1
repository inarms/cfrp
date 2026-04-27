# cfrp Windows Client Setup
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

$Binary = Join-Path $PWD "cfrp.exe"
$Config = Join-Path $PWD "client.toml"
$InstallDir = "C:\Program Files\cfrp"
$BinaryDest = Join-Path $InstallDir "cfrp.exe"
$ConfigDest = Join-Path $InstallDir "client.toml"
$ServiceName = "cfrp-client"

Write-Host "Installing cfrp Client..." -ForegroundColor Cyan
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir -Force }
Copy-Item $Binary $BinaryDest -Force
Copy-Item $Config $ConfigDest -Force
New-Item -ItemType Directory -Path (Join-Path $InstallDir "config.d") -Force

$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service) {
    Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $Service | Remove-Service -ErrorAction SilentlyContinue
}

New-Service -Name $ServiceName `
            -BinaryPathName "`"$BinaryDest`" `"$ConfigDest`"" `
            -DisplayName "cfrp Client Service" `
            -Description "C++ Fast Reverse Proxy Client" `
            -StartupType Automatic

Start-Service -Name $ServiceName
Write-Host "cfrp Client installed and started." -ForegroundColor Green
