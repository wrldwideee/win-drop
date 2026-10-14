$sys = 'C:\Windows\System32\drivers\mt7922_awdl.sys'
$bytes = [System.IO.File]::ReadAllBytes($sys)
for ($i = 0; $i -lt $bytes.Length - 8; $i++) {
    if ($bytes[$i] -eq 1 -and $bytes[$i+1] -eq 0 -and $bytes[$i+2] -eq 0 -and $bytes[$i+3] -eq 0) {
        $minor = [System.BitConverter]::ToInt32($bytes, $i+4)
        if ($minor -ge 15 -and $minor -le 40) {
            Write-Host "Found Major=1 Minor=$minor at offset 0x$($i.ToString('X'))"
        }
    }
}
