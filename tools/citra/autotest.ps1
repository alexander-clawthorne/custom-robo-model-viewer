# Run the AUTOTEST viewer in Citra and capture its window every 2 s without
# ever focusing it (the user keeps using the desktop meanwhile).
#   powershell -ExecutionPolicy Bypass -File tools\citra\autotest.ps1 -Dir build\shots [-Shots 54]
# Then: python tools\contact_sheet.py build\sheet.png --dir build\shots --from 40 --to 52
#
# For the run only, Citra shows the two screens side by side (layout_option 3)
# in a window as wide as the desktop, so a shot is mostly 3DS pixels. The
# user's qt-config.ini is copied first and put back afterwards; Citra is
# killed rather than closed, so it never writes its own settings over it.
param([string]$Dir, [int]$Shots = 72)
$sp = Split-Path $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Dir | Out-Null
Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force
$root = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra'
$citra = Join-Path $root 'citra-qt.exe'
$config = Join-Path $root 'user\config\qt-config.ini'
$backup = "$config.autotest-bak"
$app = Join-Path (Resolve-Path "$sp\..\..") 'custom-robo-viewer-autotest.3dsx'

# A backup left by an interrupted run is the user's real config: restore it first.
if (Test-Path $backup) { Copy-Item $backup $config -Force }
Copy-Item $config $backup -Force
try {
    $text = [IO.File]::ReadAllText($config)
    $text = $text -replace '(?m)^layout_option\\default=true', 'layout_option\default=false'
    $text = $text -replace '(?m)^layout_option=\d+', 'layout_option=3'
    [IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding $false))

    Start-Process $citra -ArgumentList "`"$app`""
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $p = Get-Process citra-qt -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0
        if ($p) { break }
    }
    # Side by side is 720 x 240, 3:1; the extra height is the menu and status bars.
    Add-Type -AssemblyName System.Windows.Forms
    $area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $w = $area.Width
    $h = [Math]::Min($area.Height, [int]($w / 3) + 90)
    & "$sp\capture.ps1" -SendToBack -Width $w -Height $h | Out-Null
    for ($i = 0; $i -lt $Shots; $i++) {
        if (-not (Get-Process citra-qt -ErrorAction SilentlyContinue)) { "citra exited at shot $i"; break }
        & "$sp\capture.ps1" -Out (Join-Path $Dir ('shot_{0:D2}.png' -f $i)) | Out-Null
        Start-Sleep -Seconds 2
    }
} finally {
    Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Copy-Item $backup $config -Force
    Remove-Item $backup -Force
}
"done"
