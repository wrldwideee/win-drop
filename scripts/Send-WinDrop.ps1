<#
.SYNOPSIS
    Send-WinDrop.ps1 — Native Windows AirDrop File Sender
.DESCRIPTION
    Transfers any file to nearby Apple devices (iPad, iPhone, Mac) via WIN-DROP
    AirDrop client pipeline (Discover -> Ask -> Upload .cpio.gz).
.PARAMETER FilePath
    Path to the file to send. If omitted, a file picker dialog will open.
.PARAMETER Target
    Destination peer hostname, IP[:port], or "auto" to detect nearby Apple devices.
#>

param(
    [Parameter(Mandatory = $false, Position = 0, ValueFromPipeline = $true)]
    [string]$FilePath = "",

    [Parameter(Mandatory = $false, Position = 1)]
    [string]$Target = "auto"
)

$ErrorActionPreference = "Stop"

function Show-WinDropToast {
    param(
        [string]$Title,
        [string]$Message
    )
    try {
        [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        $template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)
        $textNodes = $template.GetElementsByTagName("text")
        $textNodes.Item(0).AppendChild($template.CreateTextNode($Title)) | Out-Null
        $textNodes.Item(1).AppendChild($template.CreateTextNode($Message)) | Out-Null
        $toast = [Windows.UI.Notifications.ToastNotification]::new($template)
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("WIN-DROP").Show($toast)
    } catch {
        # Optional toast notification fallback
    }
}

# 1. Resolve and validate FilePath
if (-not $FilePath -or $FilePath.Trim() -eq "") {
    Write-Host "[*] No file path specified. Opening Windows file picker..." -ForegroundColor Cyan
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Select File to AirDrop (WIN-DROP)"
    $dialog.Filter = "All Files (*.*)|*.*"
    $dialog.Multiselect = $false
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $FilePath = $dialog.FileName
    } else {
        Write-Host "[-] Transfer canceled: no file selected." -ForegroundColor Yellow
        exit 0
    }
}

# Strip surrounding quotes if passed from cmd wrapper
$FilePath = $FilePath.Trim('"', "'", ' ')

if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
    Write-Host "[!] File not found: $FilePath" -ForegroundColor Red
    exit 1
}

$fileItem = Get-Item -LiteralPath $FilePath
$fileName = $fileItem.Name
$fileSize = $fileItem.Length
$sizeFormatted = "{0:N2} MB" -f ($fileSize / 1MB)

Write-Host ""
Write-Host "=== WIN-DROP AirDrop Sender ===" -ForegroundColor Cyan
Write-Host ("  File   : " + $fileName + " (" + $sizeFormatted + ")") -ForegroundColor White

# 2. Check and ensure awdl-daemon is running
$daemonUrl = "http://127.0.0.1:8770"
$daemonRunning = $false
try {
    $null = Invoke-RestMethod -Uri "$daemonUrl/api/status" -Method Get -TimeoutSec 2 -ErrorAction Stop
    $daemonRunning = $true
} catch {
    $daemonRunning = $false
}

if (-not $daemonRunning) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
    $possibleDaemons = @(
        (Join-Path $scriptDir "..\daemon\awdl-core\target\release\awdl-daemon.exe"),
        (Join-Path $scriptDir "..\daemon\awdl-core\target\debug\awdl-daemon.exe"),
        (Join-Path $scriptDir "awdl-daemon.exe")
    )
    $daemonExe = $possibleDaemons | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

    if ($daemonExe) {
        Write-Host "[*] WIN-DROP background daemon is not active. Starting daemon..." -ForegroundColor Yellow
        Start-Process -FilePath $daemonExe -WindowStyle Hidden
        for ($i = 1; $i -le 10; $i++) {
            Start-Sleep -Milliseconds 500
            try {
                $null = Invoke-RestMethod -Uri "$daemonUrl/api/status" -Method Get -TimeoutSec 1 -ErrorAction Stop
                $daemonRunning = $true
                Write-Host "[+] WIN-DROP daemon is now online." -ForegroundColor Green
                break
            } catch {}
        }
    }
}

if (-not $daemonRunning) {
    Write-Host "[!] Could not connect to WIN-DROP daemon at $daemonUrl." -ForegroundColor Red
    Write-Host "    Please ensure awdl-daemon is running." -ForegroundColor Yellow
    exit 1
}

# 3. Peer Selection & Target Resolution
if ($Target -eq "auto" -or $Target -eq "prompt" -or $Target -eq "select" -or (-not $Target) -or $Target.Trim() -eq "") {
    try {
        $status = Invoke-RestMethod -Uri "$daemonUrl/api/status" -Method Get -TimeoutSec 3 -ErrorAction Stop
        $peers = if ($status.peers) { @($status.peers) } else { @() }

        if ($peers.Count -eq 1) {
            $p = $peers[0]
            Write-Host ("[+] Discovered Apple device: " + $p.hostname + " [" + $p.mac + "] (" + $p.rssi + " dBm)") -ForegroundColor Green
            $Target = $p.hostname
        } elseif ($peers.Count -gt 1) {
            Write-Host ""
            Write-Host "Discovered Apple Peers in range:" -ForegroundColor Green
            for ($i = 0; $i -lt $peers.Count; $i++) {
                $p = $peers[$i]
                $sig = if ($p.rssi) { "$($p.rssi) dBm" } else { "N/A" }
                Write-Host ("  [{0}] {1,-30} [{2}] ({3})" -f ($i + 1), $p.hostname, $p.mac, $sig) -ForegroundColor White
            }
            Write-Host "  [L] Local loopback self-test [127.0.0.1:8770]" -ForegroundColor DarkGray
            $promptText = "Select recipient [1-$($peers.Count)] (default: 1): "
            $choice = Read-Host $promptText
            if ($choice -eq "L" -or $choice -eq "l" -or $choice -eq "loopback") {
                $Target = "127.0.0.1:8770"
            } elseif ($choice -match '^\d+$' -and [int]$choice -ge 1 -and [int]$choice -le $peers.Count) {
                $Target = $peers[[int]$choice - 1].hostname
            } else {
                $Target = $peers[0].hostname
            }
        } else {
            Write-Host "[i] No nearby Apple AirDrop devices discovered yet via BLE." -ForegroundColor Yellow
            Write-Host "    (Ensure Wi-Fi & Bluetooth are ON, screen unlocked, and AirDrop set to 'Everyone')" -ForegroundColor DarkGray
            $choice = Read-Host "Enter recipient IP:port or press [Enter] for local self-test [127.0.0.1:8770]"
            if ($choice -and $choice.Trim() -ne "") {
                $Target = $choice.Trim()
            } else {
                $Target = "127.0.0.1:8770"
            }
        }
    } catch {
        Write-Warning "Could not query active peers from awdl-daemon; using loopback target."
        $Target = "127.0.0.1:8770"
    }
}

Write-Host ("  Target : " + $Target) -ForegroundColor Yellow

# 4. Read file payload
Write-Host "[*] Reading file payload into memory..." -ForegroundColor Gray
$fileBytes = [System.IO.File]::ReadAllBytes($fileItem.FullName)

# 5. Dispatch to AirDrop Client via Daemon API
Write-Host "[*] Dispatching to WIN-DROP AirDrop Client..." -ForegroundColor Cyan

$headers = @{
    "X-File-Name"   = [System.Uri]::EscapeDataString($fileName)
    "X-Target-Peer" = $Target
    "Content-Type"  = "application/octet-stream"
}

try {
    $response = Invoke-RestMethod -Uri "$daemonUrl/api/send" -Method Post -Headers $headers -Body $fileBytes -TimeoutSec 120 -ErrorAction Stop

    Write-Host "[+] AirDrop transfer succeeded!" -ForegroundColor Green
    Write-Host ("    Target     : " + $response.target) -ForegroundColor White
    Write-Host ("    Bytes Sent : " + $response.bytes_sent + " bytes") -ForegroundColor White

    Show-WinDropToast -Title "AirDrop Sent Successfully" -Message ("Sent {0} to {1}" -f $fileName, $response.target)

    Start-Sleep -Seconds 1
} catch {
    $err = $_.Exception.Message
    $details = ""

    if ($_.ErrorDetails -and $_.ErrorDetails.Message) {
        $details = $_.ErrorDetails.Message
    } elseif ($_.Exception -is [System.Net.WebException] -and $_.Exception.Response) {
        try {
            $stream = $_.Exception.Response.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $details = $reader.ReadToEnd()
            $reader.Close()
        } catch {}
    }

    if ($details) {
        try {
            $json = $details | ConvertFrom-Json
            if ($json.error) {
                $err = $json.error
            }
        } catch {
            $err = "$err ($details)"
        }
    }

    Write-Host ""
    Write-Host ("[!] AirDrop transfer failed: " + $err) -ForegroundColor Red
    
    Show-WinDropToast -Title "AirDrop Transfer Failed" -Message ("Failed to send {0}: {1}" -f $fileName, $err)

    exit 1
}
