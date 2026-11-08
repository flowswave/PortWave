param([ValidateSet('msvc-debug','msvc-release','core-debug')][string]$Preset='msvc-debug')
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
if (-not (Get-Command clang-tidy -ErrorAction SilentlyContinue)) { throw 'clang-tidy missing. Install LLVM or pip install clang-tidy.' }
$files = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'src') -Recurse -Filter '*.cpp'
$analysisArgs = @('-p',(Join-Path $ProjectRoot "out/build/$Preset"))
foreach ($file in $files) { clang-tidy @analysisArgs $file.FullName; Require-Success "Analyze $($file.Name)" }
