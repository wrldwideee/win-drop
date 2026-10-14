<#
.SYNOPSIS
    Register-WindowsShare.ps1 — Register WIN-DROP in Windows Explorer "Send to" and Context Menus

.DESCRIPTION
    Integrates WIN-DROP into the Windows File Explorer shell:
      1. Adds "AirDrop (Win-Drop)" to the "Send to" context menu.
      2. Registers a direct right-click context menu "AirDrop to Apple Device (Win-Drop)".

.PARAMETER Unregister
    Removes the Send-To shortcut and registry context menu entries.
#>

param(
    [switch]$Unregister
)

$ErrorActionPreference = "Stop"

$sendToDir = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::SendTo)
$sendToCmd = Join-Path $sendToDir "AirDrop (Win-Drop).cmd"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$sendScript = Join-Path $scriptDir "Send-WinDrop.ps1"
$regKeyPath = "HKCU:\Software\Classes\*\shell\WinDrop"

if ($Unregister) {
    Write-Host "`n[*] Removing WIN-DROP Windows Explorer integration..." -ForegroundColor Yellow

    if (Test-Path -LiteralPath $sendToCmd) {
        Remove-Item -LiteralPath $sendToCmd -Force
        Write-Host "    [x] Removed SendTo shortcut: $sendToCmd" -ForegroundColor Green
    }

    if (Test-Path -Path $regKeyPath) {
        Remove-Item -Path $regKeyPath -Recurse -Force
        Write-Host "    [x] Removed Context Menu registry key: $regKeyPath" -ForegroundColor Green
    }

    Write-Host "`n[+] Successfully unregistered WIN-DROP from Windows Explorer." -ForegroundColor Green
    return
}

Write-Host "`n=== WIN-DROP Windows Shell Integration ===" -ForegroundColor Cyan

# 1. Create SendTo shortcut (.cmd wrapper)
Write-Host "[1/2] Creating Windows 'Send to' shortcut..." -ForegroundColor White
$cmdContent = @"
@echo off
title WIN-DROP AirDrop Sender
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$sendScript" -FilePath "%~1"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Press any key to close...
    pause >nul
)
"@

Set-Content -Path $sendToCmd -Value $cmdContent -Encoding ASCII
Write-Host "    [+] Created SendTo item: $sendToCmd" -ForegroundColor Green

# 2. Register Context Menu in HKCU (no Admin rights required)
Write-Host "[2/2] Registering Windows Explorer context menu..." -ForegroundColor White
try {
    New-Item -Path $regKeyPath -Force | Out-Null
    Set-ItemProperty -Path $regKeyPath -Name "(Default)" -Value "AirDrop to Apple Device (Win-Drop)" -Force
    Set-ItemProperty -Path $regKeyPath -Name "Icon" -Value "imageres.dll,-1015" -Force

    $cmdKey = "$regKeyPath\command"
    New-Item -Path $cmdKey -Force | Out-Null
    $cmdVal = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$sendScript`" -FilePath `"%1`""
    Set-ItemProperty -Path $cmdKey -Name "(Default)" -Value $cmdVal -Force

    Write-Host "    [+] Registered context menu entry in HKCU:\Software\Classes\*\shell\WinDrop" -ForegroundColor Green
} catch {
    Write-Warning "Failed to set registry context menu: $($_.Exception.Message)"
}

Write-Host "`n[+] Integration complete! You can now right-click any file in Windows Explorer and choose:" -ForegroundColor Cyan
Write-Host "    * Send to -> AirDrop (Win-Drop)" -ForegroundColor White
Write-Host "    * AirDrop to Apple Device (Win-Drop)" -ForegroundColor White
