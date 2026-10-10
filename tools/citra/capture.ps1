# Capture Citra's window without taking focus.
# usage: capture.ps1 -Out name.png            (capture only, never focuses)
#        capture.ps1 -SendToBack               (push Citra behind other windows)
param([string]$Out, [switch]$SendToBack)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class CW {
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
 public struct RECT { public int L,T,R,B; } }
'@
$p = Get-Process citra-qt -ErrorAction Stop | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
$h = $p.MainWindowHandle
if ($SendToBack) {
    # HWND_BOTTOM, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
    [CW]::SetWindowPos($h, [IntPtr]1, 0, 0, 0, 0, 0x13) | Out-Null
    "sent to back"
}
if ($Out) {
    $r = New-Object CW+RECT; [CW]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT reads the window even when it is covered.
    [CW]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
    $bmp.Save($Out); $g.Dispose(); $bmp.Dispose()
    "saved $Out"
}
