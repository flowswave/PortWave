param([ValidateSet('msvc-debug','msvc-release','core-debug')][string]$Preset='msvc-release',[ValidateRange(1,128)][int]$Jobs=6,[switch]$CleanFirst)
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
$arguments = @('--build','--preset',$Preset,'--parallel',$Jobs)
if ($CleanFirst) { $arguments += '--clean-first' }
cmake @arguments
Require-Success 'Build'
