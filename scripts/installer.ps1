param([string]$NsisRoot = '')
. "$PSScriptRoot/common.ps1"
if (-not $NsisRoot) { $NsisRoot = Join-Path $ToolchainRoot 'nsis-3.11' }
$compiler = Join-Path $NsisRoot 'makensis.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'NSIS 3.11 portable compiler missing. See docs/installer.md.' }
$release = Join-Path $ProjectRoot 'release/portable/PortWave'
if (-not (Test-Path -LiteralPath (Join-Path $release 'PortWave.exe'))) { throw 'Run package.ps1 -Preset msvc-release first.' }
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $ProjectRoot 'CMakeLists.txt') -Raw), 'project\(PortWave VERSION ([0-9.]+)').Groups[1].Value
if (-not $version) { throw 'Project version not found.' }
$stage = Join-Path $ProjectRoot 'out/installer'
New-Item -ItemType Directory -Force $stage | Out-Null
# Only deployed binaries and known runtime/license directories, never user logs or settings.
$files = @(Get-ChildItem -LiteralPath $release -File | Where-Object { $_.Extension -eq '.dll' -or $_.Name -eq 'PortWave.exe' })
foreach ($directory in @('platforms','styles','generic','iconengines','imageformats','networkinformation','tls','licenses')) {
    $path = Join-Path $release $directory
    if (Test-Path -LiteralPath $path) { $files += Get-ChildItem -LiteralPath $path -File -Recurse }
}
$files = @($files | Sort-Object FullName)
function Escape-Nsis([string]$Value) { return $Value.Replace('$', '$$').Replace('"', '$\"') }
# PowerShell 5.1 has no [IO.Path]::GetRelativePath (that is .NET Core / pwsh 7), and
# Set-Content -Encoding utf8 writes a BOM that NSIS would read as part of the first
# command. Both helpers keep the script runnable on the stock Windows PowerShell.
function Get-ReleaseRelativePath([string]$Base, [string]$Path) {
    $root = [IO.Path]::GetFullPath($Base).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Path outside release folder: $Path" }
    return $full.Substring($root.Length)
}
function Write-Utf8NoBom([string]$Path, [string[]]$Lines) {
    [IO.File]::WriteAllLines($Path, $Lines, (New-Object System.Text.UTF8Encoding($false)))
}
$payload = @()
$remove = @()
$directories = @()
foreach ($file in $files) {
    $relative = Get-ReleaseRelativePath $release $file.FullName
    $parent = Split-Path $relative -Parent
    $suffix = if ($parent) { '\' + (Escape-Nsis $parent) } else { '' }
    $payload += 'SetOutPath "$INSTDIR' + $suffix + '"'
    $payload += 'File "' + (Escape-Nsis $file.FullName) + '"'
    $remove += 'Delete "$INSTDIR\' + (Escape-Nsis $relative) + '"'
    while ($parent) { $directories += $parent; $parent = Split-Path $parent -Parent }
}
foreach ($directory in @($directories | Sort-Object -Unique | Sort-Object Length -Descending)) {
    $remove += 'RMDir "$INSTDIR\' + (Escape-Nsis $directory) + '"'
}
$payloadFile = Join-Path $stage 'payload.nsh'
$removeFile = Join-Path $stage 'remove.nsh'
Write-Utf8NoBom $payloadFile $payload
Write-Utf8NoBom $removeFile $remove
$output = Join-Path $stage "PortWave-Setup-$version-x64.exe"
$icon = Join-Path $ProjectRoot 'src/app/PortWave.ico'
if (-not (Test-Path -LiteralPath $icon)) { throw 'Application icon missing. Run src/app/make-icon.ps1 first.' }
& $compiler /V2 /INPUTCHARSET UTF8 "/DVERSION=$version" "/DOUTPUT=$output" "/DPAYLOAD=$payloadFile" "/DREMOVEFILES=$removeFile" "/DICON=$icon" (Join-Path $ProjectRoot 'installer/PortWave.nsi')
Require-Success 'Build installer'
$installerDirectory = Join-Path $ProjectRoot 'release/installer'
New-Item -ItemType Directory -Force $installerDirectory | Out-Null
$destination = Join-Path $installerDirectory ([IO.Path]::GetFileName($output))
Copy-Item -LiteralPath $output -Destination $destination -Force
Write-Output "Installer: $destination"
