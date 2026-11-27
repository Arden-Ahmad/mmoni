[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$OutputDirectory,
    [string]$InnoCompiler,
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"

$NativeDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $NativeDir "..")).Path
$BuildScript = Join-Path $PSScriptRoot "build-native.ps1"
$IssFile = Join-Path $NativeDir "installer\Mmoni-Native.iss"
$ExePath = Join-Path $NativeDir ("build\" + $Configuration.ToLowerInvariant() + "\Mmoni.exe")
$DistDir = if ($OutputDirectory) {
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
} else {
    Join-Path $RepoRoot "dist"
}

& $BuildScript -Configuration $Configuration

if (-not $SkipInstaller) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $iscc = if ($InnoCompiler) {
        (Get-Item -LiteralPath $InnoCompiler).FullName
    } else {
        $installed = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if ($installed) { $installed } else { (Get-Command iscc -ErrorAction SilentlyContinue).Source }
    }
    if (-not $iscc) {
        throw "Inno Setup 6 (ISCC.exe) not found."
    }

    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($ExePath).ProductVersion
    if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "Invalid product version in ${ExePath}: '$version'."
    }

    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    & $iscc "/DMyAppVersion=$version" "/DNativeExePath=$ExePath" "/DOutputDir=$DistDir" $IssFile
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC failed with exit code $LASTEXITCODE"
    }

    $setup = Get-Item -LiteralPath (Join-Path $DistDir "Mmoni-Setup-$version-win-x64-native.exe")
    Write-Host ("Built installer: {0} ({1:N2} MB / {2:N1} KB)" -f $setup.FullName, ($setup.Length / 1MB), ($setup.Length / 1KB)) -ForegroundColor Cyan
}
