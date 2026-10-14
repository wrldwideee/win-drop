#
# Setup-Wintun.ps1
# WIN-DROP Phase 2 - Wintun virtual adapter provisioning
#
# Creates the awdl0 IPv6 interface backed by a Wintun TUN adapter.
# Assigns a fe80::/64 link-local address derived from the AWDL MAC address.
#
# Prerequisites:
#   1. wintun.dll (from https://www.wintun.net/) in the same directory as this script
#   2. The mt7922_awdl driver loaded and \\.\AwdlRadioDevice accessible
#   3. inject_test.exe available in PATH or alongside this script
#
# Usage:
#   .\Setup-Wintun.ps1 [-AwdlMac "00:17:F2:AA:BB:CC"] [-Remove]
#
# SPDX-License-Identifier: MIT
#

#Requires -RunAsAdministrator

param(
    [string]$AwdlMac  = "00:17:F2:AB:CD:EF",   # Default AWDL MAC (Apple OUI)
    [switch]$Remove                              # Remove adapter if present
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$AdapterName = "awdl0"
$WintunDll   = "$PSScriptRoot\wintun.dll"

function Write-Step([string]$msg) { Write-Host "`n[>] $msg" -ForegroundColor Cyan }
function Write-OK([string]$msg)   { Write-Host "    [OK] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "    [!]  $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# Helper: derive IPv6 EUI-64 link-local from MAC
# MAC: AA:BB:CC:DD:EE:FF -> fe80::A8BB:CCFF:FEDD:EEFF/64
# (flip bit 6 of first octet, insert FF:FE in middle)
# ---------------------------------------------------------------------------
function Get-EUI64LinkLocal([string]$Mac) {
    $bytes = $Mac -split ":" | ForEach-Object { [Convert]::ToByte($_, 16) }
    # Flip universal/local bit (bit 1 of first octet in network order = bit 6)
    $bytes[0] = $bytes[0] -bxor 0x02
    $eui64 = "{0:X2}{1:X2}:{2:X2}FF:FE{3:X2}:{4:X2}{5:X2}" -f `
             $bytes[0], $bytes[1], $bytes[2], $bytes[3], $bytes[4], $bytes[5]
    return "fe80::$eui64"
}

# ---------------------------------------------------------------------------
# Check wintun.dll present
# ---------------------------------------------------------------------------
Write-Step "Checking Wintun library"
if (-not (Test-Path $WintunDll)) {
    Write-Warn "wintun.dll not found at $WintunDll"
    Write-Warn "Downloading from https://www.wintun.net/builds/wintun-0.14.1.zip ..."
    $zipPath = "$env:TEMP\wintun.zip"
    Invoke-WebRequest -Uri "https://www.wintun.net/builds/wintun-0.14.1.zip" `
                      -OutFile $zipPath -UseBasicParsing
    Expand-Archive -Path $zipPath -DestinationPath "$env:TEMP\wintun" -Force
    Copy-Item "$env:TEMP\wintun\wintun\bin\amd64\wintun.dll" $WintunDll -Force
    Write-OK "wintun.dll downloaded"
} else {
    Write-OK "wintun.dll found"
}

# ---------------------------------------------------------------------------
# Remove mode: delete existing awdl0 adapter
# ---------------------------------------------------------------------------
if ($Remove) {
    Write-Step "Removing adapter $AdapterName"
    $existing = Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue
    if ($existing) {
        Remove-NetAdapter -Name $AdapterName -Confirm:$false
        Write-OK "Adapter removed"
    } else {
        Write-Warn "Adapter $AdapterName not found"
    }
    exit 0
}

# ---------------------------------------------------------------------------
# Load wintun.dll and create the TUN adapter via its C API
# We invoke the WinTun API through a small inline C# P/Invoke wrapper
# ---------------------------------------------------------------------------
Write-Step "Creating Wintun TUN adapter '$AdapterName'"

$csCode = @"
using System;
using System.Runtime.InteropServices;

public class WintunHelper {
    [DllImport(@"$($WintunDll.Replace('\','\\'))", CallingConvention=CallingConvention.Cdecl)]
    public static extern IntPtr WintunCreateAdapter(
        [MarshalAs(UnmanagedType.LPWStr)] string Name,
        [MarshalAs(UnmanagedType.LPWStr)] string TunnelType,
        ref Guid RequestedGUID);

    [DllImport(@"$($WintunDll.Replace('\','\\'))", CallingConvention=CallingConvention.Cdecl)]
    public static extern void WintunCloseAdapter(IntPtr Adapter);

    [DllImport(@"$($WintunDll.Replace('\','\\'))", CallingConvention=CallingConvention.Cdecl)]
    public static extern void WintunGetAdapterLUID(IntPtr Adapter, out ulong Luid);
}
"@

Add-Type -TypeDefinition $csCode -Language CSharp

$adapterGuid = [Guid]"b5a4a6a0-8f3c-4e1d-9e2b-c71f3a880d44"

# Check if adapter already exists
$existingAdapter = Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue
if ($existingAdapter) {
    Write-Warn "Adapter '$AdapterName' already exists - skipping creation"
    $adapterHandle = [IntPtr]::Zero
} else {
    $adapterHandle = [WintunHelper]::WintunCreateAdapter($AdapterName, "AWDL", [ref]$adapterGuid)
    if ($adapterHandle -eq [IntPtr]::Zero) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "WintunCreateAdapter failed: Win32 error $err"
    }
    Write-OK "Wintun adapter handle: 0x$($adapterHandle.ToString('X'))"
}

# ---------------------------------------------------------------------------
# Configure IPv6 link-local address (EUI-64 from AWDL MAC)
# ---------------------------------------------------------------------------
Write-Step "Configuring IPv6 link-local address"

$ipv6Addr = Get-EUI64LinkLocal -Mac $AwdlMac
Write-OK "Derived EUI-64 link-local: $ipv6Addr"

# Wait for adapter to appear in Windows network stack
$retries = 20
$adapter  = $null
while ($retries-- -gt 0) {
    $adapter = Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue
    if ($adapter) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $adapter) {
    throw "Adapter '$AdapterName' did not appear in network stack after 10 seconds"
}

# Remove any existing IPv6 addresses on this interface (if different)
$existing = Get-NetIPAddress -InterfaceAlias $AdapterName -IPAddress $ipv6Addr -ErrorAction SilentlyContinue
if (-not $existing) {
    New-NetIPAddress -InterfaceAlias $AdapterName `
                     -IPAddress $ipv6Addr `
                     -PrefixLength 64 `
                     -ErrorAction SilentlyContinue | Out-Null
}
Write-OK "IPv6 address configured: $ipv6Addr/64"

# Disable IPv4 on this interface (AWDL is IPv6 only)
Set-NetIPInterface -InterfaceAlias $AdapterName `
                   -AddressFamily IPv4 `
                   -Dhcp Disabled `
                   -ErrorAction SilentlyContinue

# Bring the adapter up
Enable-NetAdapter -Name $AdapterName -Confirm:$false -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# Set MTU to 1484 (AWDL frames carry 14-byte AWDL overhead on 1500 MTU link)
# ---------------------------------------------------------------------------
$ifIdx = (Get-NetAdapter -Name $AdapterName).ifIndex
netsh interface ipv6 set subinterface "$ifIdx" mtu=1484 store=persistent | Out-Null
Write-OK "MTU set to 1484"

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
Write-Step "Verification"
$finalAdapter = Get-NetAdapter -Name $AdapterName -ErrorAction SilentlyContinue
if ($finalAdapter) {
    Write-OK "Adapter:  $($finalAdapter.Name)  Status=$($finalAdapter.Status)  MAC=$($finalAdapter.MacAddress)"
}
$finalAddr = Get-NetIPAddress -InterfaceAlias $AdapterName -AddressFamily IPv6 -ErrorAction SilentlyContinue
if ($finalAddr) {
    Write-OK "Address:  $($finalAddr.IPAddress)/$($finalAddr.PrefixLength)"
}

Write-Host ""
Write-Host "=== awdl0 interface ready ===" -ForegroundColor Magenta
Write-Host "The Rust AWDL daemon should bind to interface '$AdapterName' (index $ifIdx)"
Write-Host "and use the kernel driver via \\.\AwdlRadioDevice for frame I/O."
