param([string]$Python='python')
. "$PSScriptRoot\common.ps1"
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Installing/updating shared Program Files tools requires an elevated PowerShell. Normal builds do not.'
}
Set-Location -LiteralPath $ProjectRoot
$pythonTool = Join-Path $ToolchainRoot 'python/python.exe'
if (-not (Test-Path -LiteralPath $pythonTool)) { $pythonTool = Join-Path $ToolchainRoot 'python/Scripts/python.exe' }
if (-not (Test-Path -LiteralPath $pythonTool)) {
    # Bootstrap with an installed system Python (also under Program Files).
    $bootstrap = (Get-Command $Python -ErrorAction Stop).Source
    if (-not $bootstrap.StartsWith($env:ProgramFiles + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Pass -Python with a Python installation under Program Files. Run setup from an elevated PowerShell.'
    }
    & $bootstrap -m venv (Join-Path $ToolchainRoot 'python')
    Require-Success 'System build-tool Python environment'
    $pythonTool = Join-Path $ToolchainRoot 'python/Scripts/python.exe'
}
& $pythonTool -m pip install aqtinstall==3.3.0 cmake==4.4.4 ninja==1.13.2 clang-format==23.1.3 clang-tidy==23.1.4
Require-Success 'Portable build utilities'
if (-not (Test-Path -LiteralPath (Join-Path $ToolchainRoot 'Qt/6.8.3/msvc2022_64/bin/qmake.exe'))) {
& $pythonTool -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O (Join-Path $ToolchainRoot 'Qt') -m qtserialport
Require-Success 'Qt SDK download'
}
$archive = Join-Path $ProjectRoot 'third_party/QScintilla_src-2.14.1.tar.gz'
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot 'third_party/QScintilla_src-2.14.1/src/Qsci/qsciscintilla.h'))) {
Invoke-WebRequest -Uri 'https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz' -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'DFE13C6ACC9D85DFCBA76CCC8061E71A223957A6C02F3C343B30A9D43A4CDD4D') { throw 'QScintilla checksum mismatch' }
& $pythonTool -m tarfile -e $archive third_party
Require-Success 'QScintilla extraction'
}
$nsis = Join-Path $ToolchainRoot 'nsis-3.11'
if (-not (Test-Path -LiteralPath (Join-Path $nsis 'makensis.exe'))) {
    $metadata = Get-Content -LiteralPath (Join-Path $ProjectRoot 'third_party/dependencies.json') -Raw | ConvertFrom-Json
    $downloads = Join-Path $ToolchainRoot 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $nsisArchive = Join-Path $downloads 'nsis-3.11.zip'
    Invoke-WebRequest -Uri $metadata.nsis.mirror -OutFile $nsisArchive
    if ((Get-FileHash -LiteralPath $nsisArchive).Hash -ne $metadata.nsis.sha256) { throw 'NSIS checksum mismatch.' }
    Expand-Archive -LiteralPath $nsisArchive -DestinationPath $ToolchainRoot -Force
}
