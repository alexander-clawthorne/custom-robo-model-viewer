# Decompile main.dol functions with the local Ghidra (headless).
#   powershell -File tools\ghidra\decompile.ps1 -Out build\ghidra\motion.c 0x8001eb48 0x80054238
# The first run imports and auto-analyses build\main_ghidra.elf into build\ghidra\proj
# (several minutes); later runs reuse the project. Headless Ghidra locks its
# project, so parallel runs each need their own copy: -Copy build\ghidra_gun
# copies build\ghidra's project there on first use. (PowerShell names
# ignore case, so the parameter must not be called $proj.)
param([Parameter(Mandatory)][string]$Out, [Alias('Proj')][string]$Copy = '', [Parameter(ValueFromRemainingArguments)][string[]]$Addresses)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$ghidra = if ($env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR } else { [Environment]::GetEnvironmentVariable('GHIDRA_INSTALL_DIR', 'User') }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'User') }
$proj = Join-Path $root 'build\ghidra'
if ($Copy) {
    $dest = if ([IO.Path]::IsPathRooted($Copy)) { $Copy } else { Join-Path $root $Copy }
    if (-not (Test-Path (Join-Path $dest 'proj.gpr')) -and (Test-Path (Join-Path $proj 'proj.gpr'))) {
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item (Join-Path $proj 'proj.gpr') $dest
        Copy-Item -Recurse (Join-Path $proj 'proj.rep') $dest
    }
    $proj = $dest
}
New-Item -ItemType Directory -Force $proj | Out-Null
$elf = Join-Path $root 'build\main_ghidra.elf'
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$common = @('-scriptPath', "$PSScriptRoot", '-postScript', 'DecompileTo.java', $outPath) + $Addresses
if (Test-Path (Join-Path $proj 'proj.gpr')) {
    & "$ghidra\support\analyzeHeadless.bat" $proj proj -process main_ghidra.elf -noanalysis @common
} else {
    & "$ghidra\support\analyzeHeadless.bat" $proj proj -import $elf -processor 'PowerPC:BE:32:default' @common
}
