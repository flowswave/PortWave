[CmdletBinding(SupportsShouldProcess = $true)]
param([ValidateSet('msvc-debug','msvc-release','core-debug','all')][string]$Preset = 'all', [switch]$All)
. "$PSScriptRoot/common.ps1"
$generated = Join-Path $ProjectRoot 'out'
if ($All) {
    $targets = @($generated)
} elseif ($Preset -eq 'all') {
    $targets = @('build','package','installer') | ForEach-Object { Join-Path $generated $_ }
} else {
    $targets = @(Join-Path $generated "build/$Preset")
}
foreach ($target in $targets) {
    if ($PSCmdlet.ShouldProcess($target, 'Remove generated build files (release and user data are preserved)')) {
        Remove-GeneratedDirectory $target $generated
    }
}
