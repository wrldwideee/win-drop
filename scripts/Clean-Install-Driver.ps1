#
# Clean-Install-Driver.ps1
# Cleanly removes stale/conflicting mt7922_awdl driver packages from DriverStore
# and installs the newly compiled KMDF 1.31 package.
#
# Run in an elevated PowerShell session (Run as Administrator).
#

#Requires -RunAsAdministrator

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

Write-Host "`n=== WIN-DROP Driver Clean Installation ===" -ForegroundColor Magenta

# 1. Stop the service if present
Write-Host "`n[1/5] Stopping existing mt7922_awdl service..." -ForegroundColor Cyan
sc.exe stop mt7922_awdl | Out-Null

# Setup Debug Print Filter so kernel DbgPrint messages are captured
New-Item -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name 'Debug Print Filter' -Force -ErrorAction SilentlyContinue | Out-Null
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter' -Name 'DEFAULT' -Value 0xFFFFFFFF -Type DWord -ErrorAction SilentlyContinue
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter' -Name 'IHVDRIVER' -Value 0xFFFFFFFF -Type DWord -ErrorAction SilentlyContinue

# 2. Find and uninstall all stale OEM packages for mt7922_awdl
Write-Host "`n[2/5] Removing conflicting mt7922_awdl packages from DriverStore..." -ForegroundColor Cyan
$oemList = pnputil /enum-drivers | Select-String -Pattern "oem\d+\.inf" -Context 0, 6 |
    Where-Object { $_.Context.PostContext -match "mt7922_awdl|WinDrop" } |
    ForEach-Object {
        if ($_.Line -match "(oem\d+\.inf)") { $matches[1] }
    }

if ($oemList) {
    foreach ($oem in $oemList) {
        Write-Host "    Removing $oem (/uninstall /force)..." -ForegroundColor Yellow
        pnputil /delete-driver $oem /uninstall /force | Out-Null
    }
} else {
    Write-Host "    No existing mt7922 packages found in DriverStore." -ForegroundColor Green
}

# 3. Clean up old driver binary in System32\drivers and reset debug log
Write-Host "`n[3/5] Cleaning up stale driver binaries..." -ForegroundColor Cyan
Remove-Item "$PSScriptRoot\..\windrop_debug.log" -Force -ErrorAction SilentlyContinue
Remove-Item "$env:SystemRoot\windrop_debug.log" -Force -ErrorAction SilentlyContinue
$sysPath = "$env:SystemRoot\System32\drivers\mt7922_awdl.sys"
if (Test-Path $sysPath) {
    try {
        Remove-Item $sysPath -Force -ErrorAction Stop
        Write-Host "    Removed old mt7922_awdl.sys from System32\drivers." -ForegroundColor Green
    } catch {
        Write-Host "    Notice: mt7922_awdl.sys is locked by the OS; will be replaced on reboot." -ForegroundColor Yellow
    }
}

# 4. Install the new KMDF 1.35 driver package
$infPath = "$PSScriptRoot\..\driver\mt7922_awdl\x64\Release\mt7922_awdl\mt7922_awdl.inf"
$infFullPath = (Resolve-Path $infPath).Path

Write-Host "`n[4/5] Installing fresh KMDF 1.35 package: $infFullPath..." -ForegroundColor Cyan
pnputil /add-driver $infFullPath /install

# 5. Check Device Status
Write-Host "`n[5/5] Verifying device status in PnP manager..." -ForegroundColor Cyan
Start-Sleep -Seconds 2

$dev = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -match "14C3" -and ($_.HardwareID -match "7922" -or $_.HardwareID -match "0616") } |
    Select-Object -First 1

if ($dev) {
    Write-Host "    Restarting PnP device to load new driver binary..." -ForegroundColor Yellow
    pnputil /restart-device $dev.InstanceId | Out-Null
    Start-Sleep -Seconds 2
    $dev = Get-PnpDevice -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue
}

if ($dev) {
    $statusColor = if ($dev.Status -eq "OK") { "Green" } else { "Red" }
    $problemColor = if ($dev.Problem -eq 0) { "Green" } else { "Yellow" }

    Write-Host "    Device Name   : $($dev.FriendlyName)" -ForegroundColor Green
    Write-Host "    Device Status : $($dev.Status)" -ForegroundColor $statusColor
    Write-Host "    Problem Code  : $($dev.Problem)" -ForegroundColor $problemColor
    Write-Host "    Service       : $($dev.Service)" -ForegroundColor Green

    # Show kernel debug trace log if generated
    $logPath = "$PSScriptRoot\..\windrop_debug.log"
    if (Test-Path $logPath) {
        Write-Host "`n=== Kernel Driver Execution Log ($logPath) ===" -ForegroundColor Cyan
        Get-Content $logPath | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }
    }

    if ($dev.Status -eq "OK") {
        Write-Host "`n=== Driver is WORKING! ===" -ForegroundColor Green
        Write-Host "Testing hardware with inject_test status..." -ForegroundColor Cyan
        & "$PSScriptRoot\..\userland\inject_test\inject_test.exe" status
    } else {
        Write-Host "`n[!] If a reboot is required to replace a locked file, run: Restart-Computer" -ForegroundColor Yellow
    }
} else {
    Write-Host "    PCI Wi-Fi device not found." -ForegroundColor Yellow
}
