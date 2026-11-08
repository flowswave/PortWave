param([switch]$Check)
. "$PSScriptRoot\common.ps1"
Initialize-BuildTools
if (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) { throw 'clang-format missing. Install LLVM or pip install clang-format.' }
$files = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'src'),(Join-Path $ProjectRoot 'tests'),(Join-Path $ProjectRoot 'benchmarks') -Recurse -File | Where-Object Extension -In '.cpp','.h'
foreach ($file in $files) {
    if ($Check) { clang-format --dry-run --Werror $file.FullName } else { clang-format -i $file.FullName }
    Require-Success "Format $($file.Name)"
}
