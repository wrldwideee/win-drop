param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromPipeline = $true)]
    [string]$FilePath,

    [Parameter(Mandatory = $false, Position = 1)]
    [string]$Target = "loopback"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -Path $FilePath -PathType Leaf)) {
    Write-Error "[!] File not found: $FilePath"
    exit 1
}

$fileItem = Get-Item -LiteralPath $FilePath
$fileName = $fileItem.Name
$fileSize = $fileItem.Length
$sizeFormatted = "{0:N2} MB" -f ($fileSize / 1MB)

Write-Host ""
Write-Host "=== WIN-DROP AirDrop Sender ===" -ForegroundColor Cyan
Write-Host ("  File   : " + $fileName + " (" + $sizeFormatted + ")") -ForegroundColor White
Write-Host ("  Target : " + $Target) -ForegroundColor Yellow

# Query active peers from daemon if target is prompt or select
if ($Target -eq "prompt" -or $Target -eq "select") {
    try {
        $status = Invoke-RestMethod -Uri "http://localhost:8770/api/status" -Method Get -TimeoutSec 3
        if ($status.peers -and $status.peers.Count -gt 0) {
            Write-Host ""
            Write-Host "Discovered Apple Peers in range:" -ForegroundColor Green
            for ($i = 0; $i -lt $status.peers.Count; $i++) {
                $p = $status.peers[$i]
                Write-Host ("  [" + ($i+1) + "] " + $p.hostname + " [" + $p.mac + "] (" + $p.rssi + " dBm)") -ForegroundColor White
            }
            $promptText = "Enter peer number (1-" + $status.peers.Count + ") or Enter for loopback"
            $choice = Read-Host $promptText
            if ($choice -match '^\d+$' -and [int]$choice -le $status.peers.Count) {
                $selected = $status.peers[[int]$choice - 1]
                $Target = $selected.hostname
            } else {
                $Target = "loopback"
            }
        }
    } catch {
        Write-Warning "Could not query active peers from awdl-daemon -- using default target."
    }
}

Write-Host "[*] Reading file payload into memory..." -ForegroundColor Gray
$fileBytes = [System.IO.File]::ReadAllBytes($fileItem.FullName)

Write-Host "[*] Dispatching to WIN-DROP AirDrop Client (POST http://localhost:8770/api/send)..." -ForegroundColor Cyan

$headers = @{
    "X-File-Name"   = $fileName
    "X-Target-Peer" = $Target
    "Content-Type"  = "application/octet-stream"
}

try {
    $response = Invoke-RestMethod -Uri "http://localhost:8770/api/send" -Method Post -Headers $headers -Body $fileBytes -TimeoutSec 60

    Write-Host "[+] AirDrop transfer succeeded!" -ForegroundColor Green
    Write-Host ("    Target     : " + $response.target) -ForegroundColor Gray
    Write-Host ("    Bytes Sent : " + $response.bytes_sent) -ForegroundColor Gray

    try {
        [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        $template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)
        $textNodes = $template.GetElementsByTagName("text")
        $textNodes.Item(0).AppendChild($template.CreateTextNode("AirDrop Sent Successfully")) | Out-Null
        $msg = "Sent " + $fileName + " to " + $response.target
        $textNodes.Item(1).AppendChild($template.CreateTextNode($msg)) | Out-Null
        $toast = [Windows.UI.Notifications.ToastNotification]::new($template)
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("WIN-DROP").Show($toast)
    } catch {
        # Optional toast
    }

} catch {
    $err = $_.Exception.Message
    Write-Host ""
    Write-Host ("[!] AirDrop transfer failed: " + $err) -ForegroundColor Red
    if ($_.ErrorDetails) {
        $details = $_.ErrorDetails.Message
        Write-Host ("    Details: " + $details) -ForegroundColor Red
    }
    exit 1
}
