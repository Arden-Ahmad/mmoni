[CmdletBinding()]
param(
    [int]$SteadySeconds = 8
)

$ErrorActionPreference = "Stop"

$NativeDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BenchExe = Join-Path $NativeDir "build\release\MmoniBench.exe"
$ReleaseExe = Join-Path $NativeDir "build\release\Mmoni.exe"

if (-not (Test-Path $BenchExe)) {
    throw "MmoniBench.exe not found. Run native\scripts\build-native.ps1 first."
}

# Ensure no existing Mmoni instance is running during benchmark.
& $BenchExe --exit | Out-Null
Start-Sleep -Milliseconds 400

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $BenchExe -PassThru

# Poll until main window HWND appears (startup-to-window latency)
$startupMs = -1.0
while ($sw.ElapsedMilliseconds -lt 5000) {
    $proc.Refresh()
    if ($proc.HasExited) {
        throw "MmoniBench exited prematurely with code $($proc.ExitCode)"
    }
    if ($proc.MainWindowHandle -ne 0) {
        $startupMs = $sw.Elapsed.TotalMilliseconds
        break
    }
    Start-Sleep -Milliseconds 10
}

# Wait for first sensor snapshot + working-set trim (2s), then sample steady-state over SteadySeconds.
Start-Sleep -Seconds 2
$proc.Refresh()
$cpuStart = $proc.TotalProcessorTime.TotalMilliseconds
$wallStart = [System.Diagnostics.Stopwatch]::StartNew()

$wsSamples = @()
$privSamples = @()
for ($i = 0; $i -lt $SteadySeconds; $i++) {
    Start-Sleep -Seconds 1
    $proc.Refresh()
    if ($proc.HasExited) { throw "MmoniBench exited unexpectedly during steady-state sample." }
    $wsSamples += ($proc.WorkingSet64 / 1MB)
    $privSamples += ($proc.PrivateMemorySize64 / 1MB)
}

$wallMs = $wallStart.Elapsed.TotalMilliseconds
$cpuDeltaMs = $proc.TotalProcessorTime.TotalMilliseconds - $cpuStart
$cpuCorePct = ($cpuDeltaMs / $wallMs) * 100.0

# Signal clean graceful exit via native IPC --exit!
$exitSw = [System.Diagnostics.Stopwatch]::StartNew()
& $BenchExe --exit | Out-Null
$exitedClean = $proc.WaitForExit(4000)
$exitMs = $exitSw.Elapsed.TotalMilliseconds
if (-not $exitedClean) {
    $proc.Kill()
}

$exeKb = (Get-Item $ReleaseExe).Length / 1KB
$avgWs = ($wsSamples | Measure-Object -Average).Average
$maxWs = ($wsSamples | Measure-Object -Maximum).Maximum
$avgPriv = ($privSamples | Measure-Object -Average).Average
$maxPriv = ($privSamples | Measure-Object -Maximum).Maximum

Write-Host "`n=== Mmoni Native v2 Live GUI Benchmark (Liquid Glass Pill, 1 Hz) ===" -ForegroundColor Cyan
Write-Host ("  Release Binary Size : {0:N1} KB ({1:N2} MB)" -f $exeKb, ($exeKb / 1024.0))
Write-Host ("  Cold Startup -> HWND: {0:N1} ms" -f $startupMs)
Write-Host ("  Working Set (avg/max): {0:N2} MB / {1:N2} MB" -f $avgWs, $maxWs)
Write-Host ("  Private Bytes(avg/max): {0:N2} MB / {1:N2} MB" -f $avgPriv, $maxPriv)
Write-Host ("  App CPU (1-core pct) : {0:N2}%" -f $cpuCorePct)
Write-Host ("  IPC --exit clean     : {0} ({1:N1} ms)" -f $exitedClean, $exitMs)
