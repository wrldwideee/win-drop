#
# Setup-WinDrop.ps1
# WIN-DROP Phase 1 Deployment Script
#
# Performs the following in order:
#   1. Enable test-signing mode (bcdedit)
#   2. Generate a self-signed test certificate and install it to TrustedRoot + TrustedPublisher
#   3. Sign the driver binary with signtool
#   4. Install the driver via pnputil
#   5. Start the service (sc start)
#   6. Verify device appears in Device Manager
#
# Run as Administrator in an elevated PowerShell session.
# SPDX-License-Identifier: MIT
#

#Requires -RunAsAdministrator

param(
    [string]$DriverBin  = "$PSScriptRoot\..\driver\mt7922_awdl\x64\Release\mt7922_awdl.sys",
    [string]$InfFile    = "$PSScriptRoot\..\driver\mt7922_awdl\mt7922_awdl.inf",
    [string]$CertSubject = "CN=WinDrop Test Cert"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step([string]$msg) {
    Write-Host "`n[>] $msg" -ForegroundColor Cyan
}
function Write-OK([string]$msg) {
    Write-Host "    [OK] $msg" -ForegroundColor Green
}
function Write-Warn([string]$msg) {
    Write-Host "    [!]  $msg" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Step 1 - Enable test-signing
# ---------------------------------------------------------------------------
Write-Step "Enabling test-signing boot option"
bcdedit /set testsigning on | Out-Null
$ts = (bcdedit /enum) -match "testsigning"
if ($ts -match "Yes") {
    Write-OK "Test-signing is ON (effective after next reboot)"
} else {
    Write-Warn "bcdedit shows testsigning may not be set - check Secure Boot"
}

# ---------------------------------------------------------------------------
# Step 2 - Self-signed certificate
# ---------------------------------------------------------------------------
Write-Step "Creating self-signed test certificate"
$certPath = "Cert:\CurrentUser\My"
$existingCert = Get-ChildItem $certPath | Where-Object { $_.Subject -eq $CertSubject }

if ($existingCert) {
    Write-Warn "Certificate already exists - reusing it"
    $cert = $existingCert | Select-Object -First 1
} else {
    $cert = New-SelfSignedCertificate `
        -Subject $CertSubject `
        -CertStoreLocation $certPath `
        -Type CodeSigningCert `
        -HashAlgorithm SHA256 `
        -KeyLength 2048 `
        -NotAfter (Get-Date).AddYears(3)
    Write-OK "Certificate created: $($cert.Thumbprint)"
}

# Export to .cer so we can install into TrustedRoot + TrustedPublisher
$cerExport = "$env:TEMP\windrop_test.cer"
Export-Certificate -Cert $cert -FilePath $cerExport -Force | Out-Null

Write-Step "Installing certificate into TrustedRoot store"
$rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root","LocalMachine")
$rootStore.Open("ReadWrite")
if (-not ($rootStore.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
    $rootStore.Add($cert)
    Write-OK "Installed to TrustedRoot"
} else {
    Write-Warn "Already in TrustedRoot"
}
$rootStore.Close()

Write-Step "Installing certificate into TrustedPublisher store"
$pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher","LocalMachine")
$pubStore.Open("ReadWrite")
if (-not ($pubStore.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
    $pubStore.Add($cert)
    Write-OK "Installed to TrustedPublisher"
} else {
    Write-Warn "Already in TrustedPublisher"
}
$pubStore.Close()

# ---------------------------------------------------------------------------
# Step 3 - Sign the driver binary
# ---------------------------------------------------------------------------
Write-Step "Signing driver binary with test certificate"

if (-not (Test-Path $DriverBin)) {
    Write-Warn "Driver binary not found yet: $DriverBin"
    Write-Warn "Build the driver in Visual Studio 2022 (mt7922_awdl.vcxproj), then re-run to sign & install."
} else {
    # Locate signtool.exe from installed Windows SDK or fallback to Set-AuthenticodeSignature
    $signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin", "C:\Program Files\Windows Kits\10\bin" `
        -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } |
        Select-Object -First 1 -ExpandProperty FullName

    if ($signtool) {
        Write-OK "Using signtool: $signtool"
        & $signtool sign `
            /fd SHA256 `
            /s My `
            /sha1 $cert.Thumbprint `
            /tr "http://timestamp.digicert.com" `
            /td SHA256 `
            $DriverBin

        if ($LASTEXITCODE -ne 0) {
            Write-Warn "Timestamping failed or timed out, retrying without timestamp..."
            & $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $DriverBin
            if ($LASTEXITCODE -ne 0) {
                throw "signtool failed (exit $LASTEXITCODE)"
            }
        }
    } else {
        Write-OK "Using native PowerShell Set-AuthenticodeSignature"
        Set-AuthenticodeSignature -FilePath $DriverBin -Certificate $cert -TimestampServer "http://timestamp.digicert.com" | Out-Null
    }
    Write-OK "Driver binary signed"

    # Also generate/sign the .sys companion catalog if Inf2Cat has been run or is available
    $driverDir = Split-Path -Parent $DriverBin
    $catFile = Join-Path $driverDir "mt7922_awdl.cat"
    if (-not (Test-Path $catFile)) {
        $inf2cat = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin", "C:\Program Files\Windows Kits\10\bin" `
            -Recurse -Filter "Inf2Cat.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($inf2cat) {
            Write-OK "Generating catalog with Inf2Cat"
            & $inf2cat /driver:$driverDir /os:10_X64,Server10_X64 | Out-Null
        }
    }

    if (Test-Path $catFile) {
        if ($signtool) {
            & $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint /tr "http://timestamp.digicert.com" /td SHA256 $catFile
            if ($LASTEXITCODE -ne 0) {
                & $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $catFile
            }
        } else {
            Set-AuthenticodeSignature -FilePath $catFile -Certificate $cert -TimestampServer "http://timestamp.digicert.com" | Out-Null
        }
        Write-OK "Catalog signed: $catFile"
    }
}

# ---------------------------------------------------------------------------
# Step 4 - Install driver via pnputil
# ---------------------------------------------------------------------------
Write-Step "Installing driver package via pnputil"

# Prefer the INF in the driver output directory alongside the .sys and .cat files
$driverDir = Split-Path -Parent $DriverBin
$stagedInf = Join-Path $driverDir (Split-Path -Leaf $InfFile)
$targetInf = if (Test-Path $stagedInf) { $stagedInf } else { $InfFile }

if (-not (Test-Path $targetInf)) {
    throw "INF file not found: $targetInf"
}

if (Test-Path $DriverBin) {
    pnputil /add-driver $targetInf /install | Tee-Object -Variable pnpOutput
    if ($LASTEXITCODE -eq 0) {
        Write-OK "Driver package added and installed"
    } elseif ($LASTEXITCODE -eq 259) {
        Write-OK "Driver package added to driver store (matching device is up-to-date or already present)"
    } elseif ($LASTEXITCODE -eq 3010) {
        Write-OK "Driver package added and installed (system reboot required to take effect)"
    } else {
        throw "pnputil failed (exit $LASTEXITCODE)"
    }
} else {
    Write-Warn "Driver binary not present - skipping pnputil install until compiled."
}

# ---------------------------------------------------------------------------
# Step 5 - Start service
# ---------------------------------------------------------------------------
Write-Step "Starting mt7922_awdl service"
try {
    Start-Service -Name "mt7922_awdl" -ErrorAction Stop
    Write-OK "Service started"
} catch {
    Write-Warn "Service start skipped or failed: $_"
    Write-Warn "(Normal if the PCI device is currently using the vendor Wi-Fi driver or running in VM)"
}

# ---------------------------------------------------------------------------
# Step 6 - Verification
# ---------------------------------------------------------------------------
Write-Step "Verification"
$svc = Get-Service -Name "mt7922_awdl" -ErrorAction SilentlyContinue
if ($svc) {
    Write-OK "Service status: $($svc.Status)"
} else {
    Write-Warn "Service 'mt7922_awdl' not currently active in SCM"
}

# Check Device Manager for our PCI device
$pciDev = Get-PnpDevice -ErrorAction SilentlyContinue |
          Where-Object { $_.HardwareID -match "VEN_14C3" -and ($_.HardwareID -match "DEV_7922" -or $_.HardwareID -match "DEV_0616") }
if ($pciDev) {
    Write-OK "PCI device found: $($pciDev.FriendlyName)"
    Write-OK "Device Status: $($pciDev.Status) | Active Service: $($pciDev.Service)"
    if ($pciDev.Service -ne "mt7922_awdl") {
        Write-Warn "Device is currently using vendor driver '$($pciDev.Service)'"
        Write-Warn "To switch to mt7922_awdl: Device Manager -> Update driver -> Browse my computer -> Let me pick -> select 'MediaTek MT7922 AWDL Radio Controller'"
    }
} else {
    Write-Warn "No MT7922/RZ616 PCI device found - ensure hardware is present or VM is configured"
}

Write-Host "`n=== Setup Complete ===" -ForegroundColor Magenta
Write-Host "If test-signing was just enabled, REBOOT before loading the driver." -ForegroundColor Yellow
Write-Host ""
Write-Host "WinDbg kernel debug attach:" -ForegroundColor Cyan
Write-Host "  bcdedit /debug on"
Write-Host "  bcdedit /dbgsettings net hostip:<HOST_IP> port:50000 key:1.2.3.4"
Write-Host ""
Write-Host "Verify chip ID in WinDbg:" -ForegroundColor Cyan
Write-Host "  bp mt7922_awdl!EvtDevicePrepareHardware"
Write-Host "  g"
Write-Host "  ? @@c++(ctx->ChipId)"
