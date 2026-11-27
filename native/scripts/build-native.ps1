[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$NativeDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $NativeDir ("build\" + $Configuration.ToLowerInvariant())

function Find-VcVars64 {
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio","C:\Program Files (x86)\Microsoft Visual Studio" -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "Could not find vcvars64.bat. Install Visual Studio C++ Build Tools."
}

$VcVars = Find-VcVars64
$VsRoot = Split-Path (Split-Path (Split-Path (Split-Path $VcVars)))
$CmakeBin = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$NinjaBin = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

$env:PATH = "$CmakeBin;$NinjaBin;$env:PATH"
$cmd = "call `"$VcVars`" >nul && cmake -S `"$NativeDir`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration && cmake --build `"$BuildDir`""
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $BuildDir "Mmoni.exe"
$info = Get-Item $exe
Write-Host ("Built {0} ({1:N1} KB)" -f $info.FullName, ($info.Length / 1KB)) -ForegroundColor Green
