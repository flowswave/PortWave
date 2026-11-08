param([ValidateSet('msvc-debug','msvc-release')][string]$Preset='msvc-debug')
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
$dependencies = & ninja -C (Join-Path $ProjectRoot "out/build/$Preset") -t deps 'src/ui/CMakeFiles/portwave_ui.dir/main_window.cpp.obj'
Require-Success 'Read Ninja dependency database'
if (($dependencies -join "`n").Replace('\','/') -notmatch '/src/infrastructure/settings\.h') {
    throw 'Ninja did not record settings.h as a MainWindow dependency. Do not trust incremental builds.'
}
Write-Output 'Ninja project header dependency tracking: PASS'
