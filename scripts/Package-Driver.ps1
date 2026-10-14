#
# Package-Driver.ps1
# Stalls, catalog-hashes, and signs the mt7922_awdl release package
#

$ErrorActionPreference = "Stop"

$pkgDir = "c:\Users\ROG ZEPHYRUS\Documents\PROJECTS\win-drop\driver\mt7922_awdl\x64\Release\mt7922_awdl"
$infSrc = "c:\Users\ROG ZEPHYRUS\Documents\PROJECTS\win-drop\driver\mt7922_awdl\mt7922_awdl.inf"
$inf2cat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\inf2cat.exe"

Write-Host "Copying updated INF and SYS to package directory..." -ForegroundColor Cyan
Copy-Item $infSrc $pkgDir -Force
Copy-Item "c:\Users\ROG ZEPHYRUS\Documents\PROJECTS\win-drop\driver\mt7922_awdl\x64\Release\mt7922_awdl.sys" $pkgDir -Force

Write-Host "Generating catalog with inf2cat..." -ForegroundColor Cyan
& $inf2cat "/driver:$pkgDir" /os:10_X64,Server10_X64
if ($LASTEXITCODE -ne 0) {
    throw "inf2cat failed with exit code $LASTEXITCODE"
}

Write-Host "Signing catalog with WinDrop Test Cert..." -ForegroundColor Cyan
$cert = Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Subject -match 'WinDrop' } | Select-Object -First 1
if (-not $cert) {
    throw "WinDrop certificate not found in LocalMachine\Root"
}

$catPath = Join-Path $pkgDir "mt7922_awdl.cat"
$sysPath = Join-Path $pkgDir "mt7922_awdl.sys"

Set-AuthenticodeSignature -FilePath $catPath -Certificate $cert | Out-Null
Set-AuthenticodeSignature -FilePath $sysPath -Certificate $cert | Out-Null

Write-Host "Validation check:" -ForegroundColor Green
Get-AuthenticodeSignature $catPath, $sysPath | Format-Table -AutoSize
Write-Host "Driver package successfully created and signed!" -ForegroundColor Green
