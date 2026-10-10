# Run the AUTOTEST viewer in Citra and capture its window every 2 s without
# ever focusing it (the user keeps using the desktop meanwhile).
#   powershell -ExecutionPolicy Bypass -File tools\citra\autotest.ps1 -Dir build\shots [-Shots 54]
# Then: python tools\contact_sheet.py build\sheet.png --dir build\shots --from 40 --to 52
param([string]$Dir, [int]$Shots = 72)
$sp = Split-Path $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Dir | Out-Null
Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force
$citra = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\citra-qt.exe'
$app = Join-Path (Resolve-Path "$sp\..\..") 'custom-robo-viewer-autotest.3dsx'
Start-Process $citra -ArgumentList "`"$app`""
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $p = Get-Process citra-qt -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0
    if ($p) { break }
}
& "$sp\capture.ps1" -SendToBack | Out-Null
for ($i = 0; $i -lt $Shots; $i++) {
    if (-not (Get-Process citra-qt -ErrorAction SilentlyContinue)) { "citra exited at shot $i"; break }
    & "$sp\capture.ps1" -Out (Join-Path $Dir ('shot_{0:D2}.png' -f $i)) | Out-Null
    Start-Sleep -Seconds 2
}
Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force
"done"
