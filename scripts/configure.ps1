param([ValidateSet('msvc-debug','msvc-release','core-debug')][string]$Preset='msvc-release',[switch]$Fresh)
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
$arguments = @('--preset',$Preset)
if ($Fresh) { $arguments += '--fresh' }
cmake @arguments
Require-Success 'CMake configure'
