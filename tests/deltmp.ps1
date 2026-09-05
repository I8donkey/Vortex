# ============================================================
# deltmp.ps1 - Clean the Vortex tests dir (kept inside tests/)
#
# Removes temp artifacts produced by running runtest.ps1:
#   _cc_*.exe / _cc_dbg_*.exe / _cc_**.ll /.o intermediates,
#   leftover _vf_* (files/dirs), i.txt, c.txt.
# Note: vortexcc only emits .exe; *.o/*.ll are cleaned too if present.
# Keeps: *.vt, runtest.ps1, deltmp.ps1, etc.
#
# Usage:  powershell -ExecutionPolicy Bypass -File deltmp.ps1
#   optional: ... -File deltmp.ps1 -DryRun      (preview only)
# ============================================================
param(
    [switch]$DryRun
)
$ErrorActionPreference = "SilentlyContinue"
cd $PSScriptRoot

$patterns  = @('^_cc_','^_cc\.','^_cct','^_vf_')
$legacyExe = @('test_p1.exe','test_p2.exe','test_p3_try.exe','test_tc5.exe','c_cl3.exe','c_closure3.exe','thread_test.exe')
$txtSet    = @('i.txt','c.txt')

$targets = @()
foreach ($f in (Get-ChildItem -File)) {
    $n = $f.Name
    if ($legacyExe -contains $n) { $targets += $f; continue }
    if ($txtSet -contains $n)    { $targets += $f; continue }
    foreach ($p in $patterns) { if ($n -match $p) { $targets += $f; break } }
}
$dirs = Get-ChildItem -Directory | Where-Object { $_.Name -match '^_vf_' }

if ($DryRun) { Write-Host "== [DryRun] would delete ==" -ForegroundColor Cyan }
Write-Host ("files: " + $targets.Count + ", dirs: " + $dirs.Count)

foreach ($t in ($targets | Sort-Object Name | Group-Object Name | ForEach-Object { $_.Group[0] })) {
    Write-Host ("  del  " + $t.Name)
    if (-not $DryRun) { Remove-Item -LiteralPath $t.FullName -Force }
}
foreach ($d in $dirs) {
    Write-Host ("  del-dir  " + $d.Name)
    if (-not $DryRun) { Remove-Item -LiteralPath $d.FullName -Recurse -Force }
}

if ($DryRun) { Write-Host "== preview only; nothing deleted ==" -ForegroundColor Cyan }
else { Write-Host "Done." -ForegroundColor Green }