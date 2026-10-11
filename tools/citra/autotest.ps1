# Run the AUTOTEST viewer in Citra and capture its window every 2 s without
# ever focusing it (the user keeps using the desktop meanwhile).
#   powershell -ExecutionPolicy Bypass -File tools\citra\autotest.ps1 -Dir build\shots [-Shots 54]
# Then: python tools\contact_sheet.py build\sheet.png --dir build\shots --from 40 --to 52
#
# Battle tests judge themselves into a log, so they need no screenshots:
#   ... autotest.ps1 -Fast -Shots 0      # unthrottled, wait for the app to exit
# and read build\cr_autotest.log (copied from Citra's SD card). -Fast is
# safe for the results: the game steps once per frame, not by the clock, so
# only the on-screen fps reading changes.
#
# For the run only, Citra shows the two screens side by side (layout_option 3)
# in a window as wide as the desktop, so a shot is mostly 3DS pixels; -Fast
# also lifts the speed limit (frame_limit 0) and turns vsync off. The
# user's qt-config.ini is copied first and put back afterwards; Citra is
# killed rather than closed, so it never writes its own settings over it.
param([string]$Dir = 'build\shots', [int]$Shots = 72, [switch]$Fast, [int]$Timeout = 600)
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
New-Item -ItemType Directory -Force $Dir | Out-Null
Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force
$root = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra'
$citra = Join-Path $root 'citra-qt.exe'
$config = Join-Path $root 'user\config\qt-config.ini'
$backup = "$config.autotest-bak"
$sdLog = Join-Path $root 'user\sdmc\cr_autotest.log'
$app = Join-Path $repo 'custom-robo-viewer-autotest.3dsx'

# A backup left by an interrupted run is the user's real config: restore it first.
if (Test-Path $backup) { Copy-Item $backup $config -Force }
Copy-Item $config $backup -Force
Remove-Item $sdLog -Force -ErrorAction SilentlyContinue
$start = Get-Date
try {
    $text = [IO.File]::ReadAllText($config)
    $text = $text -replace '(?m)^layout_option\\default=true', 'layout_option\default=false'
    $text = $text -replace '(?m)^layout_option=\d+', 'layout_option=3'
    if ($Fast) {
        $text = $text -replace '(?m)^frame_limit\\default=true', 'frame_limit\default=false'
        $text = $text -replace '(?m)^frame_limit=\d+', 'frame_limit=0'
        $text = $text -replace '(?m)^use_vsync_new\\default=true', 'use_vsync_new\default=false'
        $text = $text -replace '(?m)^use_vsync_new=true', 'use_vsync_new=false'
    }
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
    if ($Shots -le 0) {
        # No screenshots: wait for the app to write its closing "# DONE" line
        # (Citra itself stays open when the app returns, so its process is no
        # signal), then close Citra here.
        $finished = $false
        while ((Get-Process citra-qt -ErrorAction SilentlyContinue) -and ((Get-Date) - $start).TotalSeconds -lt $Timeout) {
            Start-Sleep -Milliseconds 500
            if ((Test-Path $sdLog) -and (Select-String -Path $sdLog -Pattern '^# DONE' -Quiet)) { $finished = $true; break }
        }
        if (-not $finished) { "no DONE line after {0:N0} s" -f ((Get-Date) - $start).TotalSeconds }
    }
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
if (Test-Path $sdLog) { Copy-Item $sdLog (Join-Path $repo 'build\cr_autotest.log') -Force }
"done in {0:N0} s" -f ((Get-Date) - $start).TotalSeconds
