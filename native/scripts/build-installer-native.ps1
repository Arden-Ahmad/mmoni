[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"

$NativeDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $NativeDir "..")).Path
$BuildScript = Join-Path $PSScriptRoot "build-native.ps1"
$IssFile = Join-Path $NativeDir "installer\Mmoni-Native.iss"
$ExePath = Join-Path $NativeDir ("build\" + $Configuration.ToLowerInvariant() + "\Mmoni.exe")
$DistDir = Join-Path $RepoRoot "dist"

& $BuildScript -Configuration $Configuration

if (-not $SkipInstaller) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        $cmd = Get-Command iscc -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Source }
    }
    if (-not $iscc) {
        throw "Inno Setup 6 (ISCC.exe) not found."
    }

    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    & $iscc "/DNativeExePath=$ExePath" "/DOutputDir=$DistDir" $IssFile
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE"
    }

    $setup = Join-Path $DistDir "Mmoni-Setup-2.0.0-win-x64-native.exe"
    if (Test-Path $setup) {
        $fi = Get-Item $setup
        Write-Host ("Built installer: {0} ({1:N2} MB / {2:N1} KB)" -f $fi.FullName, ($fi.Length / 1MB), ($fi.Length / 1KB)) -ForegroundColor Cyan
    }
}
