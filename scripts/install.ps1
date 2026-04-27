# cfrp Windows Universal Installer
# Usage: iex (iwr https://raw.githubusercontent.com/neesonqk/cfrp/main/scripts/install.ps1).Content -Args "-Mode server"

param (
    [Parameter(Mandatory=$false)]
    [ValidateSet("server", "client", "cli")]
    $Mode = "server",

    [Parameter(Mandatory=$false)]
    $Version = "latest"
)

# 1. Elevate
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    return
}

$Repo = "neesonqk/cfrp"
$Arch = if ([Environment]::Is64BitOperatingSystem) { "amd64" } else { "x86" }
# Note: Windows ARM64 detection is more complex but usually we use amd64 for now unless specifically requested.
if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $Arch = "arm64" }

# 2. Fetch Version info
Write-Host "Fetching $Version version info..." -ForegroundColor Cyan
$Url = if ($Version -eq "latest") { "https://api.github.com/repos/$Repo/releases/latest" } else { "https://api.github.com/repos/$Repo/releases/tags/$Version" }
$ReleaseInfo = Invoke-RestMethod -Uri $Url -UseBasicParsing

$PackageMode = if ($Mode -eq "cli") { "server" } else { $Mode }
$AssetName = "cfrp-$PackageMode-windows-$Arch.zip"
$DownloadUrl = ($ReleaseInfo.assets | Where-Object { $_.name -eq $AssetName }).browser_download_url

if (-not $DownloadUrl) {
    Write-Error "Could not find download URL for $AssetName"
    return
}

# 3. Download and Extract
$TmpDir = Join-Path $env:TEMP "cfrp_install"
if (Test-Path $TmpDir) { Remove-Item $TmpDir -Recurse -Force }
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

Write-Host "Downloading from $DownloadUrl..." -ForegroundColor Cyan
$ZipPath = Join-Path $TmpDir "cfrp.zip"
Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing

Expand-Archive -Path $ZipPath -DestinationPath $TmpDir -Force

# 4. Install Binary
$InstallDir = "C:\Program Files\cfrp"
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null }

Write-Host "Installing to $InstallDir..."
Copy-Item (Join-Path $TmpDir "cfrp.exe") (Join-Path $InstallDir "cfrp.exe") -Force

# 5. Setup Service or Tool
if ($Mode -eq "cli") {
    Write-Host "CLI tool installed. Add '$InstallDir' to your PATH manually if desired." -ForegroundColor Green
    return
}

$ConfigDest = Join-Path $InstallDir "$Mode.toml"
Copy-Item (Join-Path $TmpDir "$Mode.toml") $ConfigDest -Force
if ($Mode -eq "client") {
    New-Item -ItemType Directory -Path (Join-Path $InstallDir "config.d") -Force | Out-Null
}

$ServiceName = "cfrp-$Mode"
$Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($Service) {
    Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $Service | Remove-Service -ErrorAction SilentlyContinue
}

$BinaryDest = Join-Path $InstallDir "cfrp.exe"
New-Service -Name $ServiceName `
            -BinaryPathName "`"$BinaryDest`" `"$ConfigDest`"" `
            -DisplayName "cfrp $Mode Service" `
            -Description "C++ Fast Reverse Proxy $Mode" `
            -StartupType Automatic

Start-Service -Name $ServiceName

Write-Host "--------------------------------------------------" -ForegroundColor Green
Write-Host "cfrp $Mode service installed and started successfully!"
Write-Host "Binary:  $BinaryDest"
Write-Host "Config:  $ConfigDest"
Write-Host "--------------------------------------------------" -ForegroundColor Green
