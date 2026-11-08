param([ValidateSet('msvc-debug','msvc-release','core-debug')][string]$Preset='msvc-release', [ValidateSet('all','release')][string]$Suite='all')
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
$arguments = @('--preset',$Preset)
if ($Suite -eq 'release') { $arguments += @('-L','release') }
ctest @arguments
Require-Success 'CTest'
