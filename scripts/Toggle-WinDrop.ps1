[CmdletBinding()]
param(
    [Parameter(Mandatory = $false, Position = 0)]
    [ValidateSet("WinDrop", "WiFi", "Status")]
    [string]$Mode = "Status"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

function Assert-Administrator {
    $currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "`n[!] Error: Mode '$Mode' requires an elevated PowerShell session (Run as Administrator)." -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n=== WIN-DROP Driver Mode Switcher ===" -ForegroundColor Magenta
Write-Host "Target Silicon: MediaTek MT7922 / AMD RZ616 (PCI\VEN_14C3&DEV_0616)" -ForegroundColor Cyan
Write-Host "Selected Mode : $Mode" -ForegroundColor Yellow

switch ($Mode) {
    "WiFi" {
        Assert-Administrator
        Write-Host "`n[1/3] Stopping mt7922_awdl service..." -ForegroundColor Cyan
        sc.exe stop mt7922_awdl | Out-Null

        Write-Host "`n[2/3] Removing mt7922_awdl driver packages from DriverStore..." -ForegroundColor Cyan
        $oemList = pnputil /enum-drivers | Select-String -Pattern "oem\d+\.inf" -Context 0, 6 |
            Where-Object { $_.Context.PostContext -match "mt7922_awdl|WinDrop" } |
            ForEach-Object {
                if ($_.Line -match "(oem\d+\.inf)") { $matches[1] }
            }

        if ($oemList) {
            foreach ($oem in $oemList) {
                Write-Host "    Uninstalling $oem (/uninstall /force)..." -ForegroundColor Yellow
                pnputil /delete-driver $oem /uninstall /force | Out-Null
            }
        } else {
            Write-Host "    No mt7922_awdl driver packages found in DriverStore." -ForegroundColor Green
        }

        Write-Host "`n[3/3] Rescanning hardware to rebind stock MediaTek Wi-Fi driver..." -ForegroundColor Cyan
        pnputil /scan-devices | Out-Null
        Start-Sleep -Seconds 3

        $wifiDev = Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.HardwareID -match "14C3" -and ($_.HardwareID -match "7922" -or $_.HardwareID -match "0616") } |
            Select-Object -First 1

        if ($wifiDev) {
            Write-Host "    Device Name   : $($wifiDev.FriendlyName)" -ForegroundColor Green
            Write-Host "    Device Status : $($wifiDev.Status)" -ForegroundColor Green
            Write-Host "    Service       : $($wifiDev.Service)" -ForegroundColor Green
            Write-Host "`n=== Successfully restored stock Wi-Fi driver! ===" -ForegroundColor Green
        } else {
            Write-Host "    Device not found after rescan." -ForegroundColor Yellow
        }
    }

    "WinDrop" {
        Assert-Administrator
        Write-Host "`nInvoking Clean-Install-Driver.ps1 to bind mt7922_awdl.sys..." -ForegroundColor Cyan
        $installScript = Join-Path $PSScriptRoot "Clean-Install-Driver.ps1"
        if (Test-Path $installScript) {
            & $installScript
        } else {
            Write-Host "    Cannot find Clean-Install-Driver.ps1 at $installScript" -ForegroundColor Red
        }
    }

    "Status" {
        # Clean up temporary disasm files from repo root
        $repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
        $disasmFiles = Get-ChildItem -Path $repoRoot -Filter "*disasm*.txt" -ErrorAction SilentlyContinue
        if ($disasmFiles) {
            Write-Host "`nCleaning up temporary disassembly dumps..." -ForegroundColor Cyan
            foreach ($f in $disasmFiles) {
                Write-Host "    Removing $($f.Name) ($($f.Length) bytes)..." -ForegroundColor Gray
                Remove-Item $f.FullName -Force -ErrorAction SilentlyContinue
            }
        }

        Write-Host "`n[Device PnP Status]" -ForegroundColor Cyan
        $dev = Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.HardwareID -match "14C3" -and ($_.HardwareID -match "7922" -or $_.HardwareID -match "0616") } |
            Select-Object -First 1

        if ($dev) {
            $isWinDrop = ($dev.Service -eq "mt7922_awdl")
            $modeName = if ($isWinDrop) { "Win-Drop AWDL Driver (mt7922_awdl.sys)" } else { "Stock Wi-Fi Driver ($($dev.Service))" }
            $statusColor = if ($dev.Status -eq "OK") { "Green" } else { "Red" }

            $modeColor = if ($isWinDrop) { "Cyan" } else { "Yellow" }
            $probColor = if ($dev.Problem -eq 0) { "Green" } else { "Red" }

            Write-Host "    Device Name   : $($dev.FriendlyName)" -ForegroundColor Green
            Write-Host "    Active Mode   : $modeName" -ForegroundColor $modeColor
            Write-Host "    Device Status : $($dev.Status)" -ForegroundColor $statusColor
            Write-Host "    Problem Code  : $($dev.Problem)" -ForegroundColor $probColor
            Write-Host "    Driver Service: $($dev.Service)" -ForegroundColor Green

            if ($isWinDrop -and $dev.Status -eq "OK") {
                Write-Host "`nQuerying mt7922_awdl radio status..." -ForegroundColor Cyan
                $injectTest = Join-Path $repoRoot "userland\inject_test\inject_test.exe"
                if (Test-Path $injectTest) {
                    & $injectTest status
                }
            }
        } else {
            Write-Host "    MediaTek MT7922 / AMD RZ616 PCIe device not detected." -ForegroundColor Yellow
        }
    }
}
