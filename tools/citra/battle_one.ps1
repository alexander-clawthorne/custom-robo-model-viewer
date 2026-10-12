# One battle autotest, built and run on its own: one arena, optionally one
# body / leg, optionally one phase, printing only the verdict lines.
#   powershell -ExecutionPolicy Bypass -File tools\citra\battle_one.ps1 -Arena 30
#   ... battle_one.ps1 -Arena 4 -Phase lava          # the lava drop only, ~2 s of game
#   ... battle_one.ps1 -Arena 12 -Body 0 -Leg 0      # the whole script, one robot
# It clears the autotest build first (make does not see the AUTO* flags
# change), builds with docker, runs Citra unthrottled with no screenshots
# (autotest.ps1 -Fast -Shots 0) and prints the "# ..." lines of
# build\cr_autotest.log. -Log also prints the per-frame lines.
param([Parameter(Mandatory = $true)][int]$Arena, [int]$Body = -1, [int]$Leg = -1,
    [ValidateSet('', 'lava')][string]$Phase = '', [switch]$Log)
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
Push-Location $repo
try {
    Remove-Item -Recurse -Force build3ds_auto, custom-robo-viewer-autotest.elf, custom-robo-viewer-autotest.3dsx `
        -ErrorAction SilentlyContinue
    $flags = @('AUTOTEST=1', 'AUTOCAT=battle', "AUTOARENA=$Arena", 'TARGET=custom-robo-viewer-autotest',
        'BUILD=build3ds_auto')
    if ($Body -ge 0) { $flags += "AUTOBODY=$Body" }
    if ($Leg -ge 0) { $flags += "AUTOLEG=$Leg" }
    if ($Phase) { $flags += "AUTOPHASE=$Phase" }
    $mount = ($repo.Path -replace '\\', '/') + ':/proj'
    $out = docker run --rm -v $mount -w /proj mkw3ds-build:2026-10-09 make @flags 2>&1
    if (-not (Test-Path custom-robo-viewer-autotest.3dsx)) {
        $out | Select-String -Pattern 'error' | Select-Object -First 10 | ForEach-Object { $_.Line }
        'build failed'
        exit 1
    }
    & "$sp\autotest.ps1" -Fast -Shots 0 | Select-Object -Last 1
    $pattern = if ($Log) { '.' } else { '^#' }
    Select-String -Path build\cr_autotest.log -Pattern $pattern | ForEach-Object { $_.Line }
} finally {
    Pop-Location
}
