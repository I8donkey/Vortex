$ErrorActionPreference = "Stop"
$cc = "E:\newcodering\build_ninja\bin\vortexcc.exe"
$interp = "E:\newcodering\build_ninja\bin\vortex.exe"
$tests = @(
  "test_cast", "test_p1", "test_p1_full", "test_p2",
  "test_p3_ref", "test_p3_try", "test_p3_closure",
  "test_p4_b", "test_p4_mod", "test_p4_log", "test_p4_time",
  "test_p4_thread", "test_p4_channel_pool"
)
$fail = 0
function Run2($exe, $argStr) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $argStr
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
    $vt = "E:\newcodering\$t.vt"
    $gi = Run2 $interp ("`"$vt`"")
    $g = $gi[1]; $gc = $gi[0]; $ge = $gi[2]
    $ccout = & $cc build $vt -o "E:\newcodering\_cc_$t.exe" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $t : compile error" -ForegroundColor Red
        Write-Host $ccout
        $fail++
        continue
    }
    $ci = Run2 "E:\newcodering\_cc_$t.exe" ""
    $r = $ci[1]; $rc = $ci[0]; $re = $ci[2]
    $mismatch = ($r -ne $g -or $re -ne $ge)
    # P3：同样校验 --debug 编译（DWARF 场景）下语义一致
    $dc = "E:\newcodering\_cc_dbg_$t.exe"
    $ccd = & $cc build $vt -o $dc --debug 2>&1 | Out-String
    $dcMismatch = $false
    if ($LASTEXITCODE -ne 0) {
        $dcMismatch = $true
    } else {
        $di = Run2 $dc ""
        $dout = $di[1]; $derr = $di[2]
        $dcMismatch = ($dout -ne $g -or $derr -ne $ge)
        Remove-Item $dc -ErrorAction SilentlyContinue
    }
    if ($mismatch -or $dcMismatch) {
        Write-Host "[FAIL] $t (reportErr=$dcMismatch or outErr=$mismatch)" -ForegroundColor Red
        Write-Host "--- interp(stdout,$gc) ---"; Write-Host $g
        Write-Host "--- cc(stdout,$rc) ---"; Write-Host $r
        if ($ge -ne $re) {
            Write-Host "--- interp stderr ---"; Write-Host $ge
            Write-Host "--- cc stderr ---"; Write-Host $re
        }
        $fail++
    } else {
        Write-Host "[ok] $t" -ForegroundColor Green
    }
    Remove-Item "E:\newcodering\_cc_$t.exe" -ErrorAction SilentlyContinue
}
Write-Host "================================"
if ($fail -eq 0) { Write-Host "ALL PASS" -ForegroundColor Green }
else { Write-Host "$fail FAILED" -ForegroundColor Red }