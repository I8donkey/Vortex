# ============================================================
# runtest.ps1 - Vortex golden tests: interp vs compiled (incl --debug)
#
# Portable: locate everything relative to $PSScriptRoot (tests/)
# and the project root (tests/ ..). No hardcoded drive letters.
# Copy the whole project (incl tests/ and build_ninja/), build it,
# then run:
#     powershell -ExecutionPolicy Bypass -File tests\runtest.ps1
# ============================================================
$ErrorActionPreference = "Stop"

$testsDir = $PSScriptRoot
$root     = Split-Path $PSScriptRoot -Parent

# Always run from the tests dir so that relative-path file writes
# (file.write / sql.open / ...) resolve consistently on any machine.
Set-Location $testsDir

# Built binaries live in <root>\build_ninja\bin
$cc     = Join-Path $root "build_ninja\bin\vortexcc.exe"
$interp = Join-Path $root "build_ninja\bin\vortex.exe"

# Output artifacts go inside tests/ so they can be cleaned easily.
$ccExe  = Join-Path $testsDir "_cc_{0}.exe"
$dbgExe = Join-Path $testsDir "_cc_dbg_{0}.exe"

if (-not (Test-Path $cc) -or -not (Test-Path $interp)) {
    Write-Host ("[ERROR] Missing build outputs: " + $cc + " or " + $interp) -ForegroundColor Red
    Write-Host "        Build the project first (build_ninja is the build dir)." -ForegroundColor Red
    exit 1
}

$tests = @(
  "test_cast", "test_p1", "test_p1_full", "test_p2",
  "test_p3_ref", "test_p3_try", "test_p3_closure",
  "test_p4_b", "test_p4_mod", "test_p4_log", "test_p4_time",
  "test_p4_thread", "test_p4_channel_pool",
  "test_modules_r3d_g2d",
  "test_file",
  "test_zip",
  "test_xml",
  "test_html",
  "test_sql",
  "test_os",
  "test_regex",
  "test_json",
  "test_base64",
  "test_datetime",
  "test_csv",
  "test_hash",
  "test_str",
  "test_net",
  "test_math",
  "test_random",
  "test_sys",
  "test_stringlist"
)
$fail = 0

function Run2($exe, $argStr) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $argStr
    $psi.WorkingDirectory = $testsDir
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return @($p.ExitCode, $out, $err)
}

foreach ($t in $tests) {
    $vt    = Join-Path $testsDir "$t.vt"
    $cexe  = [string]::Format($ccExe,  $t)
    $dexe  = [string]::Format($dbgExe, $t)

    if (-not (Test-Path $vt)) {
        Write-Host ("[FAIL] " + $t + " : missing " + $vt) -ForegroundColor Red
        $fail++
        continue
    }

    $gi = Run2 $interp ("`"$vt`"")
    $g = $gi[1]; $gc = $gi[0]; $ge = $gi[2]

    # compile + run
    $ccout = & $cc build $vt -o $cexe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("[FAIL] " + $t + " : compile error") -ForegroundColor Red
        Write-Host $ccout
        $fail++
        continue
    }
    $ci = Run2 $cexe ""
    $r = $ci[1]; $rc = $ci[0]; $re = $ci[2]
    $mismatch = ($r -ne $g -or $re -ne $ge)

    # --debug (DWARF) build must also match
    $ccd = cmd /c "`"$cc`" build `"$vt`" -o `"$dexe`" --debug 2>&1" | Out-String
    $dcMismatch = $false
    if ($LASTEXITCODE -ne 0) {
        $dcMismatch = $true
    } else {
        $di = Run2 $dexe ""
        $dout = $di[1]; $derr = $di[2]
        $dcMismatch = ($dout -ne $g -or $derr -ne $ge)
        Remove-Item $dexe -ErrorAction SilentlyContinue
    }

    if ($mismatch -or $dcMismatch) {
        Write-Host ("[FAIL] " + $t + " (reportErr=" + $dcMismatch + " or outErr=" + $mismatch + ")") -ForegroundColor Red
        Write-Host ("--- interp(stdout," + $gc + ") ---"); Write-Host $g
        Write-Host ("--- cc(stdout," + $rc + ") ---"); Write-Host $r
        if ($ge -ne $re) {
            Write-Host "--- interp stderr ---"; Write-Host $ge
            Write-Host "--- cc stderr ---"; Write-Host $re
        }
        $fail++
    } else {
        Write-Host ("[ok] " + $t) -ForegroundColor Green
    }
    Remove-Item $cexe -ErrorAction SilentlyContinue
}
Write-Host "================================"
if ($fail -eq 0) { Write-Host "ALL PASS" -ForegroundColor Green }
else { Write-Host ("$fail FAILED") -ForegroundColor Red }