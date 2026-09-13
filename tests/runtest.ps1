# ============================================================
# runtest.ps1 - Vortex golden tests: interp vs compiled (incl --debug)
# Multi-process parallel using built-in Start-Job.
# Ninja-style progress and verbose command output.
# Usage:
#     powershell -ExecutionPolicy Bypass -File tests\runtest.ps1 [-j N] [-v]
# ============================================================
param(
    [Alias('j')]
    [int]$MaxConcurrency = 8,
    [Alias('v')]
    [switch]$Verbose,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

Write-Host "=== [DIAG] Runtest script started ===" -ForegroundColor Cyan

if ($Help) {
    Write-Host "runtest.ps1`n"
    Write-Host "Run Vortex golden regression tests in parallel.`n"
    Write-Host "Options:"
    Write-Host "  -j, -MaxConcurrency <num>   Number of parallel workers (default: 8)"
    Write-Host "  -v, -Verbose               Show actual build/run commands for each test"
    Write-Host "  --help                      Show this help message`n"
    Write-Host "Example:"
    Write-Host "  .\runtest.ps1"
    Write-Host "  .\runtest.ps1 -j 12 -v"
    Write-Host "Press Enter to exit..."
    Read-Host
    exit 0
}

$testsDir = $PSScriptRoot
if (-not $testsDir) {
    Write-Host "[ERROR] `$PSScriptRoot is empty. Cannot determine script directory." -ForegroundColor Red
    Write-Host "Press Enter to exit..."
    Read-Host
    exit 1
}

$root     = Split-Path $testsDir -Parent

Write-Host "=== [DIAG] Script Root: $testsDir" -ForegroundColor Cyan
Write-Host "=== [DIAG] Project Root: $root" -ForegroundColor Cyan

Set-Location $testsDir

# Built binaries live in <root>\bin
$cc       = Join-Path $root "bin\vortexcc.exe"
$interp   = Join-Path $root "bin\vortex.exe"

# Output artifacts go inside tests/ so they can be cleaned easily.
$ccExe    = Join-Path $testsDir "_cc_{0}.exe"
$dbgExe   = Join-Path $testsDir "_cc_dbg_{0}.exe"

Write-Host "=== [DIAG] Checking binaries..." -ForegroundColor Cyan
if (-not (Test-Path $cc) -or -not (Test-Path $interp)) {
    Write-Host ("[ERROR] Missing build outputs: " + $cc + " or " + $interp) -ForegroundColor Red
    Write-Host "        Build the project first (build_ninja is the build dir)." -ForegroundColor Red
    Write-Host "Press Enter to exit..."
    Read-Host
    exit 1
}
Write-Host "=== [DIAG] Binaries found." -ForegroundColor Green

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

$jobs = @()
Write-Host "=== [DIAG] Creating jobs for $($tests.Count) tests..." -ForegroundColor Cyan

foreach ($t in $tests) {
    $vt    = Join-Path $testsDir "$t.vt"
    $cexe  = [string]::Format($ccExe,  $t)
    $dexe  = [string]::Format($dbgExe, $t)

    # Throttle concurrency based on jobs created by this script
    $runningCount = ($jobs | Where-Object { $_.State -eq 'Running' }).Count
    while ($runningCount -ge $MaxConcurrency) {
        Start-Sleep -Milliseconds 50
        $runningCount = ($jobs | Where-Object { $_.State -eq 'Running' }).Count
    }

    Write-Host "=== [DIAG] Starting job for: $t" -ForegroundColor DarkGray

    $jobObj = Start-Job -ScriptBlock {
        param(
            $testName,
            $vtPath,
            $ccBin,
            $interpBin,
            $outCcExe,
            $outDbgExe,
            $workDir
        )
        $ErrorActionPreference = "Stop"
        try {
            Set-Location $workDir

            function Run2Job($exe, $argStr, $wd) {
                $psi = New-Object System.Diagnostics.ProcessStartInfo
                $psi.FileName = $exe
                $psi.Arguments = $argStr
                $psi.WorkingDirectory = $wd
                $psi.RedirectStandardOutput = $true
                $psi.RedirectStandardError = $true
                $psi.UseShellExecute = $false
                $p = [System.Diagnostics.Process]::Start($psi)
                $out = $p.StandardOutput.ReadToEnd()
                $err = $p.StandardError.ReadToEnd()
                $p.WaitForExit()
                return @($p.ExitCode, $out, $err, "$exe $argStr")
            }

            if (-not (Test-Path $vtPath)) {
                return [PSCustomObject]@{
                    TestName      = $testName
                    Status        = "missing"
                    InterpCode    = $null
                    InterpOut     = $null
                    InterpErr     = $null
                    CcCode        = $null
                    CcOut         = $null
                    CcErr         = $null
                    DbgMismatch   = $null
                    CcCompileLog  = $null
                    InterpCmd     = $null
                    CcCompileCmd  = $null
                    CcRunCmd      = $null
                    DbgCompileCmd = $null
                    DbgRunCmd     = $null
                }
            }

            # Run interpreter
            $interpCmd = "`"$interpBin`" `"$vtPath`""
            $gi = Run2Job $interpBin "`"$vtPath`"" $workDir
            $gCode = $gi[0]; $gOut = $gi[1]; $gErr = $gi[2]

            # Compile normal executable
            $ccCompileCmd = "`"$ccBin`" build `"$vtPath`" -o `"$outCcExe`""
            $ccout = & $ccBin build $vtPath -o $outCcExe 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                Remove-Item $outCcExe -ErrorAction SilentlyContinue
                return [PSCustomObject]@{
                    TestName      = $testName
                    Status        = "cc_compile_fail"
                    InterpCode    = $gCode
                    InterpOut     = $gOut
                    InterpErr     = $gErr
                    CcCode        = $null
                    CcOut         = $null
                    CcErr         = $null
                    DbgMismatch   = $null
                    CcCompileLog  = $ccout
                    InterpCmd     = $interpCmd
                    CcCompileCmd  = $ccCompileCmd
                    CcRunCmd      = $null
                    DbgCompileCmd = $null
                    DbgRunCmd     = $null
                }
            }

            # Run normal executable
            $ccRunCmd = "`"$outCcExe`""
            $ci = Run2Job $outCcExe "" $workDir
            $rCode = $ci[0]; $rOut = $ci[1]; $rErr = $ci[2]
            $mismatch = ($rOut -ne $gOut -or $rErr -ne $gErr)

            # Compile debug executable
            $dbgCompileCmd = "`"$ccBin`" build `"$vtPath`" -o `"$outDbgExe`" --debug"
            $ccd = & $ccBin build $vtPath -o $outDbgExe --debug 2>&1 | Out-String
            $dcMismatch = $false
            $dbgRunCmd = $null
            if ($LASTEXITCODE -ne 0) {
                $dcMismatch = $true
            } else {
                $dbgRunCmd = "`"$outDbgExe`""
                $di = Run2Job $outDbgExe "" $workDir
                $dOut = $di[1]; $dErr = $di[2]
                $dcMismatch = ($dOut -ne $gOut -or $dErr -ne $gErr)
                Remove-Item $outDbgExe -ErrorAction SilentlyContinue
            }
            Remove-Item $outCcExe -ErrorAction SilentlyContinue

            return [PSCustomObject]@{
                TestName      = $testName
                Status        = if ($mismatch -or $dcMismatch) { "mismatch" } else { "ok" }
                InterpCode    = $gCode
                InterpOut     = $gOut
                InterpErr     = $gErr
                CcCode        = $rCode
                CcOut         = $rOut
                CcErr         = $rErr
                DbgMismatch   = $dcMismatch
                CcCompileLog  = $ccout
                InterpCmd     = $interpCmd
                CcCompileCmd  = $ccCompileCmd
                CcRunCmd      = $ccRunCmd
                DbgCompileCmd = $dbgCompileCmd
                DbgRunCmd     = $dbgRunCmd
            }
        } catch {
            return [PSCustomObject]@{
                TestName      = $testName
                Status        = "job_error"
                InterpCode    = $null
                InterpOut     = $null
                InterpErr     = $null
                CcCode        = $null
                CcOut         = $null
                CcErr         = $null
                DbgMismatch   = $null
                CcCompileLog  = $_.Exception.Message
                InterpCmd     = $null
                CcCompileCmd  = $null
                CcRunCmd      = $null
                DbgCompileCmd = $null
                DbgRunCmd     = $null
            }
        }
    } -ArgumentList $t,$vt,$cc,$interp,$cexe,$dexe,$testsDir

    $jobs += $jobObj
}

# ---- Real-time progress processing ----
$total = $tests.Count
$completed = 0
$failCount = 0
$jobList = $jobs

Write-Host "=== [DIAG] All jobs created. Waiting for completion..." -ForegroundColor Cyan

while ($jobList.Count -gt 0) {
    $finished = @()
    foreach ($j in $jobList) {
        if ($j.State -in @('Completed','Failed','Stopped')) {
            $res = Receive-Job $j -ErrorAction SilentlyContinue
            Remove-Job $j -ErrorAction SilentlyContinue

            $completed++
            
            # Safeguard against null results
            $status = if ($null -ne $res -and $null -ne $res.Status) { $res.Status } else { "job_error" }
            $testName = if ($null -ne $res -and $null -ne $res.TestName) { $res.TestName } else { "Unknown" }

            $statusSymbol = switch ($status) {
                "ok"                { "ok" }
                "missing"           { "FAIL (missing test file)" }
                "cc_compile_fail"   { "FAIL (compile error)" }
                "mismatch"          { "FAIL (output mismatch)" }
                "job_error"         { "FAIL (internal job error)" }
                default             { "FAIL (unknown)" }
            }
            $isOk = ($status -eq "ok")
            $color = if ($isOk) { "Green" } else { "Red" }

            # Real-time progress line
            Write-Host ("[{0}/{1}] {2} ... {3}" -f $completed, $total, $testName, $statusSymbol) -ForegroundColor $color

            # If verbose, show the actual commands executed
            if ($Verbose -and $null -ne $res -and $res.InterpCmd) {
                Write-Host "  Interp: $($res.InterpCmd)"
                if ($res.CcCompileCmd) { Write-Host "  Compile: $($res.CcCompileCmd)" }
                if ($res.CcRunCmd)     { Write-Host "  Run: $($res.CcRunCmd)" }
                if ($res.DbgCompileCmd) { Write-Host "  Debug Compile: $($res.DbgCompileCmd)" }
                if ($res.DbgRunCmd)    { Write-Host "  Debug Run: $($res.DbgRunCmd)" }
            }

            # Detailed diagnostics on failure
            if (-not $isOk -and $null -ne $res) {
                $failCount++
                switch ($status) {
                    "missing" {
                        Write-Host "  Test file not found: $testName.vt"
                    }
                    "cc_compile_fail" {
                        Write-Host "  Compiler output:"
                        Write-Host $res.CcCompileLog
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
                    "job_error" {
                        Write-Host "  Job error: $($res.CcCompileLog)"
                    }
                    default {
                        Write-Host "  Unknown failure."
                    }
                }
            }
            $finished += $j
        }
    }
    $jobList = $jobList | Where-Object { $_ -notin $finished }
    if ($jobList.Count -gt 0) {
        Start-Sleep -Milliseconds 100
    }
}

# ---- Summary ----
Write-Host "================================"
if ($failCount -eq 0) {
    Write-Host "ALL PASS" -ForegroundColor Green
} else {
    Write-Host ("$failCount FAILED") -ForegroundColor Red
}

Write-Host "Press Enter to exit..."
Read-Host
exit $failCount