param([ValidateRange(1,128)][int]$Jobs = 6, [switch]$Clean)
. "$PSScriptRoot/common.ps1"
if ($Clean) { & "$PSScriptRoot/clean.ps1" -Preset msvc-release }
& "$PSScriptRoot/configure.ps1" -Preset msvc-release
& "$PSScriptRoot/build.ps1" -Preset msvc-release -Jobs $Jobs
& "$PSScriptRoot/test.ps1" -Preset msvc-release -Suite release
& "$PSScriptRoot/package.ps1" -Preset msvc-release
& "$PSScriptRoot/installer.ps1"
$root = Join-Path $ProjectRoot 'release'
$artifacts = @(Get-ChildItem -LiteralPath (Join-Path $root 'portable') -Filter '*.zip' -File) + @(Get-ChildItem -LiteralPath (Join-Path $root 'installer') -Filter '*.exe' -File)
$lines = foreach ($artifact in $artifacts) {
    $relative = $artifact.Name
    "$( (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant() )  $relative"
}
[IO.File]::WriteAllLines((Join-Path $root 'SHA256SUMS.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
Write-Output "Release artifacts and SHA256SUMS.txt: $root"
