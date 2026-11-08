. "$PSScriptRoot\common.ps1"
Initialize-Toolchain 'msvc-release'
Write-Output "Shared toolchain: $ToolchainRoot"
foreach ($tool in 'cmake','ninja','cl','clang-format','clang-tidy') {
    $command = Get-Command $tool -ErrorAction SilentlyContinue
    if ($command) { Write-Output "$tool : $($command.Source)" } else { Write-Output "$tool : NOT FOUND" }
}
foreach ($kit in 'msvc2022_64') {
    $qmake = Join-Path $ToolchainRoot "Qt/6.8.3/$kit/bin/qmake.exe"
    if (Test-Path -LiteralPath $qmake) { & $qmake -v }
}
if (Test-Path -LiteralPath (Join-Path $ProjectRoot 'third_party/QScintilla_src-2.14.1/src/Qsci/qsciscintilla.h')) { Write-Output 'QScintilla 2.14.1 source: PRESENT' }
