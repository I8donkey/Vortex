# ============================================================
# runtest.ps1 - Vortex tests: correctness (.expected) + golden (interp vs compiled)
# Multi-process parallel using built-in Start-Job.
#
# Usage:
#     powershell -ExecutionPolicy Bypass -File tests\runtest.ps1 [-j N] [-v] [-a] [-t 20]
#
# Test discovery:
#   Every tests/<name>.vt is a candidate. How it is verified:
#     * If tests/<name>.expected exists -> CORRECTNESS test.
#         The interpreter must exit 0 and its stdout must equal the
#         .expected file content (CRLF-insensitive, trailing-newline-insensitive).
#         Recommended way to add new test cases: drop a Foo.vt + Foo.expected pair.
#     * Else if <name> is in the default whitelist, or -All is given ->
#         GOLDEN test: interpreter vs compiler vs compiler(-debug) must agree.
#     * Otherwise the file is skipped (only reported). Use -All to force-run.
#
# Timeout: every single run/compile step is killed after -TimeoutSec seconds
#          (default 20) and reported as "run time exceeded", so a hanging test
#          cannot stall the whole suite.
#
# Exit code = number of failures.
# ============================================================
param(
    [Alias('j')]
    [int]$MaxConcurrency = 8,
    [Alias('v')]
    [switch]$Verbose,
    [Alias('a')]
    [switch]$All,
    [Alias('t')]
    [int]$TimeoutSec = 20,
    # Interpreter-only mode: skip vortexcc and compare nothing (CI on Linux/macOS).
    [switch]$InterpOnly,
    # Write a JUnit XML report to this path.
    [string]$JUnit = "",
    # Test names to skip (e.g. platform-specific ones).
    [string[]]$Skip = @(),
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$isCI = ($env:CI -eq "true")

# Accept both "-Skip a,b" and "-Skip a -Skip b" (comma-separated values come
# through as one string when the script is launched with -File).
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

if ($Help) {
    Write-Host "runtest.ps1`n"
    Write-Host "Run Vortex tests in parallel.`n"
    Write-Host "  - correctness:  tests/<name>.vt + tests/<name>.expected"
    Write-Host "                  -> assert interp stdout == expected, exit 0."
    Write-Host "  - golden:       interp, compiler, compiler (--debug) must agree.`n"
    Write-Host "Options:"
    Write-Host "  -j, -MaxConcurrency <num>   Parallel workers (default: 8)"
    Write-Host "  -t, -TimeoutSec <sec>       Kill a step after N seconds (default: 20)"
    Write-Host "  -v, -Verbose                Show actual build/run commands"
    Write-Host "  -a, -All                    Also golden-verify *.vt not in whitelist"
    Write-Host "  -InterpOnly                 Only run the interpreter (no vortexcc)"
    Write-Host "  -JUnit <path>               Write a JUnit XML report"
    Write-Host "  -Skip <name[,name...]>      Skip the listed tests"
    Write-Host "Example:"
    Write-Host "  .\runtest.ps1"
    Write-Host "  .\runtest.ps1 -j 12 -v"
    Write-Host "  .\runtest.ps1 -InterpOnly -a -JUnit report.xml"
    if (-not $isCI) { Write-Host "Press Enter to exit..."; Read-Host }
    exit 0
}

$testsDir = $PSScriptRoot
if (-not $testsDir) {
    Write-Host "[ERROR] `$PSScriptRoot is empty. Cannot determine script directory." -ForegroundColor Red
    if (-not $isCI) { Write-Host "Press Enter to exit..."; Read-Host }
    exit 1
}

$root = Split-Path $testsDir -Parent
Set-Location $testsDir

# --- Platform-aware binary names ---
$isWin = ($env:OS -eq "Windows_NT") -or ([System.Environment]::OSVersion.Platform -eq 'Win32NT')
$exeSuffix = if ($isWin) { ".exe" } else { "" }
$binDir = Join-Path $root "bin"
$cc     = Join-Path $binDir ("vortexcc" + $exeSuffix)
$interp = Join-Path $binDir ("vortex" + $exeSuffix)

# Lib-O3 (Windows only): compiled tests that link a DLL need the DLL next to the exe.
if ($isWin) {
    $depDll = Join-Path $binDir "libzlib.dll"
    if (Test-Path $depDll) { Copy-Item $depDll $testsDir -Force -ErrorAction SilentlyContinue }
} else {
    # extdef links Windows DLLs; the interpreter rejects it elsewhere.
    if ($Skip -notcontains "test_ffi") { $Skip = @($Skip) + "test_ffi" }
}

if ($InterpOnly) {
    if (-not (Test-Path $interp)) {
        Write-Host ("[ERROR] Missing interpreter: " + $interp) -ForegroundColor Red
        Write-Host "        Build the project first." -ForegroundColor Red
        if (-not $isCI) { Write-Host "Press Enter to exit..."; Read-Host }
        exit 1
    }
} elseif (-not (Test-Path $cc) -or -not (Test-Path $interp)) {
    Write-Host ("[ERROR] Missing build outputs: " + $cc + " or " + $interp) -ForegroundColor Red
    Write-Host "        Build the project first (or use -InterpOnly)." -ForegroundColor Red
    if (-not $isCI) { Write-Host "Press Enter to exit..."; Read-Host }
    exit 1
}

# Default golden whitelist (backwards compatible with the original suite).
$whitelist = @(
  "test_cast", "test_p1", "test_p1_full", "test_p2",
  "test_p3_ref", "test_p3_try", "test_p3_closure",
  "test_p4_b", "test_p4_mod", "test_p4_log", "test_p4_time",
  "test_p4_thread", "test_p4_channel_pool",
  "test_modules_r3d_g2d",
  "test_file", "test_zip", "test_xml", "test_html", "test_sql",
  "test_os", "test_regex", "test_json", "test_base64",
  "test_datetime", "test_csv", "test_hash", "test_str", "test_net",
  "test_math", "test_random", "test_sys", "test_stringlist",
  "test_class", "test_class_inherit", "test_operator", "test_auto", "test_type", "test_raise",
  "test_extfloat", "test_ffi", "test_mathpow"
)

$ccExe  = Join-Path $testsDir "_cc_{0}.exe"
$dbgExe = Join-Path $testsDir "_cc_dbg_{0}.exe"

# --- Discover tests ---
$discovered = @(
    Get-ChildItem -Path $testsDir -Filter *.vt -File |
        Sort-Object BaseName |
        ForEach-Object { $_.BaseName }
)

$tests   = @()   # tests that will actually run
$skipped = @()   # *.vt present but not verified (no .expected, not whitelisted, no -All)
$skipSet = @{}
foreach ($s in $Skip) { if ($s) { $skipSet[$s] = $true } }
foreach ($n in $discovered) {
    if ($skipSet.ContainsKey($n)) { continue }
    $hasExpected = Test-Path (Join-Path $testsDir "$n.expected")
    if ($hasExpected) { $tests += $n; continue }
    if ($whitelist -contains $n) { $tests += $n; continue }
    if ($All) { $tests += $n } else { $skipped += $n }
}
$tests = @($tests | Sort-Object -Unique)

Write-Host "Discovered $($discovered.Count) .vt files."
Write-Host "Testing $($tests.Count); skipped $($skipped.Count) (use -All to run skipped)."
if ($Skip.Count -gt 0) { Write-Host ("Explicitly skipped: " + ($Skip -join ", ")) -ForegroundColor DarkGray }
Write-Host "Timeout per step: $TimeoutSec s"
Write-Host ("Mode: " + ($(if ($InterpOnly) { "interpreter-only" } else { "golden (interp vs compiler)" })))

$jobs = @()
foreach ($t in $tests) {
    $vt   = Join-Path $testsDir "$t.vt"
    $cexe = [string]::Format($ccExe,  $t)
    $dexe = [string]::Format($dbgExe, $t)

    $runningCount = ($jobs | Where-Object { $_.State -eq 'Running' }).Count
    while ($runningCount -ge $MaxConcurrency) {
        Start-Sleep -Milliseconds 50
        $runningCount = ($jobs | Where-Object { $_.State -eq 'Running' }).Count
    }

    $jobObj = Start-Job -ScriptBlock {
        param($testName, $vtPath, $ccBin, $interpBin, $outCcExe, $outDbgExe,
              $workDir, $timeoutSec, $interpOnly)
        $ErrorActionPreference = "Stop"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            Set-Location $workDir

            # Run a process with a hard timeout. Kills on timeout.
            # Returns: @(ExitCode, stdout, stderr, cmdline). ExitCode = -1 on timeout.
            function Run2Job($exe, $argStr, $wd, $timeoutSec) {
                $psi = New-Object System.Diagnostics.ProcessStartInfo
                $psi.FileName = $exe
                $psi.Arguments = $argStr
                $psi.WorkingDirectory = $wd
                $psi.RedirectStandardOutput = $true
                $psi.RedirectStandardError = $true
                $psi.UseShellExecute = $false
                $p = [System.Diagnostics.Process]::Start($psi)
                if (-not $p.WaitForExit($timeoutSec * 1000)) {
                    # timeout: kill before reading streams to avoid blocking
                    try { $p.Kill() } catch {}
                    $p.WaitForExit()
                    $out = $p.StandardOutput.ReadToEnd()
                    $err = $p.StandardError.ReadToEnd()
                    return @(-1, $out, "run time exceeded (> $timeoutSec s): $err", "$exe $argStr")
                }
                $out = $p.StandardOutput.ReadToEnd()
                $err = $p.StandardError.ReadToEnd()
                return @($p.ExitCode, $out, $err, "$exe $argStr")
            }

            $interpCmd = "`"$interpBin`" `"$vtPath`""
            $ccBuildCmd   = "build `"$vtPath`" -o `"$outCcExe`""
            $ccBuildDbgCmd = "build `"$vtPath`" -o `"$outDbgExe`" --debug"

            $mkStatus = {
                param($n, $s, $code, $out, $err)
                [PSCustomObject]@{
                    TestName = $n; Status = $s; IsExpected = $false
                    InterpCode = $code; InterpOut = $out; InterpErr = $err
                    CcCode = $null; CcOut = $null; CcErr = $null
                    DbgMismatch = $null; CcCompileLog = $err
                    InterpCmd = $interpCmd; CcCompileCmd = $ccBuildCmd
                    CcRunCmd = $null; DbgCompileCmd = $null; DbgRunCmd = $null
                }
            }

            # ---------------- CORRECTNESS test -------------------
            $expPath = Join-Path $workDir "$testName.expected"
            if (Test-Path $expPath) {
                $gi = Run2Job $interpBin "`"$vtPath`"" $workDir $timeoutSec
                $gCode = $gi[0]; $gOut = $gi[1]; $gErr = $gi[2]
                if ($gCode -lt 0) {
                    return [PSCustomObject]@{
                        TestName = $testName; Status = "timeout"; IsExpected = $true
                        InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                        CcCode = $null; CcOut = $null; CcErr = $null
                        DbgMismatch = $null; CcCompileLog = $null
                        InterpCmd = $interpCmd; CcCompileCmd = $null; CcRunCmd = $null
                        DbgCompileCmd = $null; DbgRunCmd = $null
                    }
                }
                $expected = [System.IO.File]::ReadAllText($expPath).Replace("`r`n","`n").TrimEnd("`n")
                $actual   = ([string]$gOut).Replace("`r`n","`n").TrimEnd("`n")
                $ok = ($gCode -eq 0) -and ($actual -eq $expected)
                return [PSCustomObject]@{
                    TestName = $testName
                    Status   = if ($ok) { "ok" } else { "expected_mismatch" }
                    IsExpected = $true
                    InterpCode = $gCode
                    InterpOut  = $gOut
                    InterpErr  = $gErr
                    ExpectedContent = $expected
                    ActualContent   = $actual
                    InterpCmd  = $interpCmd
                }
            }

            # ---------------- INTERPRETER-ONLY mode (CI on Linux/macOS) ----
            # No compiler backend is available there, so only require a clean exit.
            if ($interpOnly) {
                if (-not (Test-Path $vtPath)) {
                    return & $mkStatus $testName "missing" $null $null $null
                }
                $gi = Run2Job $interpBin "`"$vtPath`"" $workDir $timeoutSec
                $gCode = $gi[0]; $gOut = $gi[1]; $gErr = $gi[2]
                if ($gCode -lt 0) { return & $mkStatus $testName "timeout" $gCode $gOut $gErr }
                $st = if ($gCode -eq 0) { "ok" } else { "interp_fail" }
                return [PSCustomObject]@{
                    TestName = $testName; Status = $st; IsExpected = $false
                    InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                    CcCode = $null; CcOut = $null; CcErr = $null
                    DbgMismatch = $null; CcCompileLog = $null
                    InterpCmd = $interpCmd; CcCompileCmd = $null; CcRunCmd = $null
                    DbgCompileCmd = $null; DbgRunCmd = $null
                    DurationMs = $sw.ElapsedMilliseconds
                }
            }

            # ---------------- GOLDEN test (interp vs cc vs dbg) ----
            if (-not (Test-Path $vtPath)) {
                return & $mkStatus $testName "missing" $null $null $null
            }

            $gi = Run2Job $interpBin "`"$vtPath`"" $workDir $timeoutSec
            $gCode = $gi[0]; $gOut = $gi[1]; $gErr = $gi[2]
            if ($gCode -lt 0) { return & $mkStatus $testName "timeout" $gCode $gOut $gErr }

            # compile normal executable
            $cres = Run2Job $ccBin $ccBuildCmd $workDir $timeoutSec
            $ccCode = $cres[0]; $ccOut = $cres[1]; $ccErr = $cres[2]
            if ($ccCode -lt 0) {
                Remove-Item $outCcExe -ErrorAction SilentlyContinue
                return & $mkStatus $testName "timeout" $gCode $gOut $gErr
            }
            if ($ccCode -ne 0) {
                Remove-Item $outCcExe -ErrorAction SilentlyContinue
                return & $mkStatus $testName "cc_compile_fail" $gCode $gOut $gErr
            }

            # run normal executable
            $ci = Run2Job $outCcExe "" $workDir $timeoutSec
            $rCode = $ci[0]; $rOut = $ci[1]; $rErr = $ci[2]
            if ($rCode -lt 0) {
                Remove-Item $outCcExe -ErrorAction SilentlyContinue
                return [PSCustomObject]@{
                    TestName = $testName; Status = "timeout"; IsExpected = $false
                    InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                    CcCode = $rCode; CcOut = $rOut; CcErr = $rErr
                    DbgMismatch = $null; CcCompileLog = $ccErr
                    InterpCmd = $interpCmd; CcCompileCmd = $ccBuildCmd; CcRunCmd = "`"$outCcExe`""
                    DbgCompileCmd = $null; DbgRunCmd = $null
                }
            }
            $mismatch = ($rOut -ne $gOut -or $rErr -ne $gErr)

            # compile debug executable
            $dres = Run2Job $ccBin $ccBuildDbgCmd $workDir $timeoutSec
            $dcCode = $dres[0]; $dcOut = $dres[1]; $dcErr = $dres[2]
            $dcMismatch = $false
            $dbgRunCmd = $null
            if ($dcCode -lt 0) {
                Remove-Item $outCcExe, $outDbgExe -ErrorAction SilentlyContinue
                return [PSCustomObject]@{
                    TestName = $testName; Status = "timeout"; IsExpected = $false
                    InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                    CcCode = $rCode; CcOut = $rOut; CcErr = $rErr
                    DbgMismatch = $null; CcCompileLog = $dcErr
                    InterpCmd = $interpCmd; CcCompileCmd = $ccBuildCmd; CcRunCmd = "`"$outCcExe`""
                    DbgCompileCmd = $ccBuildDbgCmd; DbgRunCmd = $null
                }
            }
            if ($dcCode -ne 0) {
                $dcMismatch = $true
            } else {
                $dbgRunCmd = "`"$outDbgExe`""
                $di = Run2Job $outDbgExe "" $workDir $timeoutSec
                $dOut = $di[1]; $dErr = $di[2]
                if ($di[0] -lt 0) {
                    Remove-Item $outCcExe, $outDbgExe -ErrorAction SilentlyContinue
                    return [PSCustomObject]@{
                        TestName = $testName; Status = "timeout"; IsExpected = $false
                        InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                        CcCode = $rCode; CcOut = $rOut; CcErr = $rErr
                        DbgMismatch = $null; CcCompileLog = $dErr
                        InterpCmd = $interpCmd; CcCompileCmd = $ccBuildCmd; CcRunCmd = "`"$outCcExe`""
                        DbgCompileCmd = $ccBuildDbgCmd; DbgRunCmd = $dbgRunCmd
                    }
                }
                $dcMismatch = ($dOut -ne $gOut -or $dErr -ne $gErr)
                Remove-Item $outDbgExe -ErrorAction SilentlyContinue
            }
            Remove-Item $outCcExe -ErrorAction SilentlyContinue

            return [PSCustomObject]@{
                TestName = $testName
                Status   = if ($mismatch -or $dcMismatch) { "mismatch" } else { "ok" }
                IsExpected = $false
                InterpCode = $gCode; InterpOut = $gOut; InterpErr = $gErr
                CcCode = $rCode; CcOut = $rOut; CcErr = $rErr
                DbgMismatch = $dcMismatch; CcCompileLog = $ccErr
                InterpCmd = $interpCmd; CcCompileCmd = $ccBuildCmd; CcRunCmd = "`"$outCcExe`""
                DbgCompileCmd = $ccBuildDbgCmd; DbgRunCmd = $dbgRunCmd
            }
        } catch {
            return [PSCustomObject]@{
                TestName = $testName; Status = "job_error"; IsExpected = $false
                InterpCode = $null; InterpOut = $null; InterpErr = $null
                CcCode = $null; CcOut = $null; CcErr = $null
                DbgMismatch = $null; CcCompileLog = $_.Exception.Message
                InterpCmd = $null; CcCompileCmd = $null; CcRunCmd = $null
                DbgCompileCmd = $null; DbgRunCmd = $null
            }
        }
    } -ArgumentList $t,$vt,$cc,$interp,$cexe,$dexe,$testsDir,$TimeoutSec,$InterpOnly

    $jobs += $jobObj
}

# ---- Real-time progress ----
$total = $tests.Count
$completed = 0
$failCount = 0
$results = @()
$jobList = $jobs

while ($jobList.Count -gt 0) {
    $finished = @()
    foreach ($j in $jobList) {
        if ($j.State -in @('Completed','Failed','Stopped')) {
            $res = Receive-Job $j -ErrorAction SilentlyContinue
            Remove-Job $j -ErrorAction SilentlyContinue

            $completed++
            $status = if ($null -ne $res -and $null -ne $res.Status) { $res.Status } else { "job_error" }
            $testName = if ($null -ne $res -and $null -ne $res.TestName) { $res.TestName } else { "Unknown" }

            $statusSymbol = switch ($status) {
                "ok"                { "ok" }
                "missing"           { "FAIL (missing test file)" }
                "cc_compile_fail"   { "FAIL (compile error)" }
                "mismatch"          { "FAIL (golden mismatch)" }
                "expected_mismatch" { "FAIL (output != expected)" }
                "interp_fail"       { "FAIL (interpreter error)" }
                "timeout"           { "FAIL (timeout: run time exceeded)" }
                "job_error"         { "FAIL (internal job error)" }
                default             { "FAIL (unknown)" }
            }
            $isOk = ($status -eq "ok")
            $color = if ($isOk) { "Green" } else { "Red" }
            Write-Host ("[{0}/{1}] {2} ... {3}" -f $completed, $total, $testName, $statusSymbol) -ForegroundColor $color

            if ($Verbose -and $null -ne $res -and $res.InterpCmd) {
                Write-Host "  Interp: $($res.InterpCmd)"
                if ($res.CcCompileCmd) { Write-Host "  Compile: $($res.CcCompileCmd)" }
                if ($res.CcRunCmd)     { Write-Host "  Run: $($res.CcRunCmd)" }
                if ($res.DbgCompileCmd) { Write-Host "  Debug Compile: $($res.DbgCompileCmd)" }
                if ($res.DbgRunCmd)    { Write-Host "  Debug Run: $($res.DbgRunCmd)" }
            }

            if (-not $isOk -and $null -ne $res) {
                $failCount++
                switch ($status) {
                    "missing" { Write-Host "  Test file not found: $testName.vt" }
                    "cc_compile_fail" { Write-Host "  Compiler output:"; Write-Host $res.CcCompileLog }
                    "timeout" {
                        Write-Host ("  Step took longer than $TimeoutSec s and was killed.")
                        if ($res.InterpErr) { Write-Host "  --- stderr ---"; Write-Host $res.InterpErr }
                        if ($res.CcErr) { Write-Host "  --- cc stderr ---"; Write-Host $res.CcErr }
                    }
                    "expected_mismatch" {
                        Write-Host ("  Exit code: " + $res.InterpCode)
                        Write-Host "  --- expected (tests\$testName.expected) ---"
                        Write-Host $res.ExpectedContent
                        Write-Host "  --- actual (stdout) ---"
                        Write-Host $res.ActualContent
                        if ($res.InterpErr) { Write-Host "  --- interp stderr ---"; Write-Host $res.InterpErr }
                    }
                    "mismatch" {
                        Write-Host ("  dbgMismatch = " + $res.DbgMismatch)
                        Write-Host ("  --- interp(stdout, " + $res.InterpCode + ") ---")
                        Write-Host $res.InterpOut
                        Write-Host ("  --- cc(stdout, " + $res.CcCode + ") ---")
                        Write-Host $res.CcOut
                        if ($res.InterpErr -ne $res.CcErr) {
                            Write-Host "  --- interp stderr ---"
                            Write-Host $res.InterpErr
                            Write-Host "  --- cc stderr ---"
                            Write-Host $res.CcErr
                        }
                    }
                    "job_error" { Write-Host "  Job error: $($res.CcCompileLog)" }
                    "interp_fail" {
                        Write-Host ("  Exit code: " + $res.InterpCode)
                        if ($res.InterpOut) { Write-Host "  --- stdout ---"; Write-Host $res.InterpOut }
                        if ($res.InterpErr) { Write-Host "  --- stderr ---"; Write-Host $res.InterpErr }
                    }
                    default { Write-Host "  Unknown failure." }
                }
            }
            if ($null -ne $res) { $results += $res }
            $finished += $j
        }
    }
    $jobList = $jobList | Where-Object { $_ -notin $finished }
    if ($jobList.Count -gt 0) { Start-Sleep -Milliseconds 100 }
}

# ---- Summary ----
Write-Host "================================"
if ($failCount -eq 0) {
    Write-Host "ALL PASS" -ForegroundColor Green
} else {
    Write-Host ("$failCount FAILED") -ForegroundColor Red
}
if ($skipped.Count -gt 0) {
    Write-Host ("Skipped (add .expected or pass -All): " + ($skipped -join ", ")) -ForegroundColor DarkGray
}

# ---- JUnit XML report ----
if ($JUnit -ne "") {
    function XmlEsc([string]$s) {
        if ($null -eq $s) { return "" }
        return $s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace('"', "&quot;")
    }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('<?xml version="1.0" encoding="UTF-8"?>')
    [void]$sb.AppendLine(('<testsuites name="vortex" tests="{0}" failures="{1}" skipped="{2}">' -f $total, $failCount, $skipped.Count))
    $suiteName = if ($InterpOnly) { "vortex-interpreter" } else { "vortex-golden" }
    [void]$sb.AppendLine(('  <testsuite name="{0}" tests="{1}" failures="{2}" skipped="{3}">' -f $suiteName, $total, $failCount, $skipped.Count))
    foreach ($r in $results) {
        $nm = if ($r.TestName) { $r.TestName } else { "Unknown" }
        $st = if ($r.Status) { $r.Status } else { "job_error" }
        $dur = if ($null -ne $r.DurationMs) { [double]$r.DurationMs / 1000.0 } else { 0.0 }
        [void]$sb.AppendLine(('    <testcase classname="{0}" name="{1}" time="{2:F3}">' -f $suiteName, (XmlEsc $nm), $dur))
        if ($st -ne "ok") {
            $msg = switch ($st) {
                "missing"           { "missing test file" }
                "cc_compile_fail"   { "compile error" }
                "mismatch"          { "golden mismatch" }
                "expected_mismatch" { "output != expected" }
                "interp_fail"       { "interpreter error" }
                "timeout"           { "timeout" }
                default             { "internal job error" }
            }
            $detail = ""
            if ($r.ExpectedContent) { $detail = "expected:`n" + $r.ExpectedContent + "`nactual:`n" + $r.ActualContent }
            elseif ($r.CcCompileLog) { $detail = $r.CcCompileLog }
            elseif ($r.InterpErr) { $detail = $r.InterpErr }
            [void]$sb.AppendLine(('      <failure message="{0}">{1}</failure>' -f (XmlEsc $msg), (XmlEsc $detail)))
        }
        [void]$sb.AppendLine('    </testcase>')
    }
    foreach ($s in $skipped) {
        [void]$sb.AppendLine(('    <testcase classname="{0}" name="{1}"><skipped/></testcase>' -f $suiteName, (XmlEsc $s)))
    }
    [void]$sb.AppendLine('  </testsuite>')
    [void]$sb.AppendLine('</testsuites>')
    $outPath = if ([System.IO.Path]::IsPathRooted($JUnit)) { $JUnit } else { Join-Path $testsDir $JUnit }
    [System.IO.File]::WriteAllText($outPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("JUnit report: " + $outPath) -ForegroundColor DarkGray
}

if (-not $isCI) { Write-Host "Press Enter to exit..."; Read-Host }
exit $failCount