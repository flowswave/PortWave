$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$ToolchainRoot = if ($env:PORTWAVE_TOOLCHAIN_ROOT) { [IO.Path]::GetFullPath($env:PORTWAVE_TOOLCHAIN_ROOT) } else { Join-Path $env:ProgramFiles 'PortWaveToolchain' }
function Require-Success([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed (exit $LASTEXITCODE)" }
}
# Only generated descendants of the named project directory may be removed.
# Reparse points are rejected rather than followed into an external directory.
function Remove-GeneratedDirectory([string]$Path, [string]$AllowedRoot) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $allowed = [IO.Path]::GetFullPath($AllowedRoot).TrimEnd('\')
    $projectPrefix = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\') + '\'
    if (-not $allowed.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not ($full -eq $allowed -or $full.StartsWith($allowed + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "Refusing removal outside generated project directory: $full"
    }
    if (Test-Path -LiteralPath $full) {
        $ancestor = $full
        while ($ancestor.Length -gt $projectPrefix.TrimEnd('\').Length) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point in cleanup path: $ancestor"
            }
            $ancestor = Split-Path $ancestor -Parent
        }
        $items = @((Get-Item -LiteralPath $full -Force)) + @(Get-ChildItem -LiteralPath $full -Recurse -Force)
        if ($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw "Reparse point in cleanup target: $full" }
        Write-Output "Removing generated directory: $full"
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
function Initialize-BuildTools {
    $tools = Join-Path $ToolchainRoot 'python/Scripts'
    if (-not (Test-Path -LiteralPath (Join-Path $tools 'cmake.exe'))) {
        throw "Build tools missing under $ToolchainRoot. See README.md (toolchain setup)."
    }
    $env:PATH = "$tools;$env:PATH"
    $env:PORTWAVE_NINJA = Join-Path $tools 'ninja.exe'
    $env:QT_MSVC_ROOT = Join-Path $ToolchainRoot 'Qt/6.8.3/msvc2022_64'
}
function Initialize-Toolchain([string]$Preset) {
    if ($Preset -notin @('msvc-debug','msvc-release','core-debug')) { throw "Unsupported MSVC preset: $Preset" }
    Initialize-BuildTools
    # MSVC localizes the /showIncludes note ("注意: 包含文件:"). CMake copies that text
    # into rules.ninja as msvc_deps_prefix; with a non-ASCII prefix Ninja matches
    # nothing and records zero header dependencies, so edited headers silently keep
    # stale objects. English diagnostics keep the prefix ASCII. The toolchain installs
    # both zh-CN and en-US resources, so this only switches cl's message language.
    $env:VSLANG = '1033'
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'MSVC not found. Install VS 2022 Build Tools with Desktop C++, MSVC v143 and Windows SDK, or run from x64 Developer PowerShell.' }
        $installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installation) { throw 'No Visual Studio instance with MSVC x64 tools found.' }
        & (Join-Path $installation 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    }
    if ($Preset -ne 'core-debug') {
        if (-not (Test-Path -LiteralPath (Join-Path $env:QT_MSVC_ROOT 'bin/qmake.exe'))) { throw "Qt SDK missing: $env:QT_MSVC_ROOT" }
        $env:PATH = "$env:QT_MSVC_ROOT\bin;$env:PATH"
    }
    foreach ($tool in 'cmake','ninja') { if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is missing. See README.md." } }
    Set-Location -LiteralPath $ProjectRoot
}
