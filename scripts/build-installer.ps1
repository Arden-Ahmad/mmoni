[CmdletBinding()]
param(
    [ValidateSet("both", "standalone", "net10")]
    [string]$Flavor = "both",

    [string]$Configuration = "Release",

    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ProjectFile = Join-Path $RepoRoot "Mmoni.csproj"
$IssFile = Join-Path $RepoRoot "installer\Mmoni.iss"
$PublishRoot = Join-Path $RepoRoot "publish"
$DistDir = Join-Path $RepoRoot "dist"

[xml]$csproj = Get-Content -Path $ProjectFile
$Version = ($csproj.Project.PropertyGroup | ForEach-Object { $_.Version } | Where-Object { $_ } | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = "1.0.0"
}

function Find-Iscc {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

$IsccPath = $null
if (-not $SkipInstaller) {
    $IsccPath = Find-Iscc
    if (-not $IsccPath) {
        throw "Inno Setup 6 (ISCC.exe) not found. Install it via: winget install --id JRSoftware.InnoSetup --scope user"
    }
    Write-Host "Using Inno Setup compiler: $IsccPath" -ForegroundColor Cyan
}

New-Item -ItemType Directory -Path $DistDir -Force | Out-Null

$FlavorsToBuild = if ($Flavor -eq "both") { @("standalone", "net10") } else { @($Flavor) }
$BuiltArtifacts = @()

foreach ($currentFlavor in $FlavorsToBuild) {
    $selfContained = if ($currentFlavor -eq "standalone") { "true" } else { "false" }
    $publishDir = Join-Path $PublishRoot "win-x64-$currentFlavor"

    Write-Host "`n=== Publishing Mmoni v$Version ($currentFlavor, self-contained=$selfContained) ===" -ForegroundColor Green
    if (Test-Path $publishDir) {
        Remove-Item -Path $publishDir -Recurse -Force
    }

    & dotnet publish $ProjectFile `
        -c $Configuration `
        -r win-x64 `
        --self-contained $selfContained `
        -o $publishDir `
        -p:DebugType=none `
        -p:DebugSymbols=false

    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish failed for flavor '$currentFlavor' (exit code $LASTEXITCODE)."
    }

    if (-not $SkipInstaller) {
        Write-Host "=== Building Inno Setup installer ($currentFlavor) ===" -ForegroundColor Green
        & $IsccPath `
            "/DMyAppVersion=$Version" `
            "/DPublishDir=$publishDir" `
            "/DInstallerFlavor=$currentFlavor" `
            "/DOutputDir=$DistDir" `
            $IssFile

        if ($LASTEXITCODE -ne 0) {
            throw "ISCC failed for flavor '$currentFlavor' (exit code $LASTEXITCODE)."
        }

        $installerPath = Join-Path $DistDir "Mmoni-Setup-$Version-win-x64-$currentFlavor.exe"
        if (Test-Path $installerPath) {
            $BuiltArtifacts += Get-Item $installerPath
        }
    }
}

Write-Host "`n=== Build Complete ===" -ForegroundColor Cyan
Write-Host "Publish folders: $PublishRoot"
if ($BuiltArtifacts.Count -gt 0) {
    Write-Host "Installers:"
    foreach ($file in $BuiltArtifacts) {
        $sizeMb = [Math]::Round($file.Length / 1MB, 2)
        Write-Host ("  - {0} ({1} MB)" -f $file.FullName, $sizeMb) -ForegroundColor Yellow
    }
}
