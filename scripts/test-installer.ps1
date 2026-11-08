param([string]$Installer = (Join-Path (Split-Path $PSScriptRoot -Parent) 'release/installer/PortWave-Setup-0.1.0-x64.exe'))
. "$PSScriptRoot/common.ps1"
# Never overwrite an existing personal installation or its Start Menu entries.
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) { throw 'Per-machine installer smoke test needs an elevated PowerShell session.' }
$registration = 'HKLM:/Software/PortWaveCppInstaller'
$uninstallKey = 'HKLM:/Software/Microsoft/Windows/CurrentVersion/Uninstall/PortWaveCpp'
$shortcuts = Join-Path $env:ProgramData 'Microsoft/Windows/Start Menu/Programs/PortWave'
if ((Test-Path -LiteralPath $registration) -or (Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath $shortcuts)) {
    throw 'Existing installation or shortcuts found. Installer smoke test will not overwrite them.'
}
$target = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'out/installer-smoke/PortWave 安装验证'))
$allowed = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'out/installer-smoke')) + [IO.Path]::DirectorySeparatorChar
if (-not $target.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid smoke target.' }
if (Test-Path -LiteralPath $target) { throw 'Smoke target already exists; inspect it before retrying.' }
$install = Start-Process -FilePath $Installer -ArgumentList ('/S /D=' + $target) -WindowStyle Hidden -PassThru
if (-not $install.WaitForExit(60000)) { throw 'Installer timed out.' }
if ($install.ExitCode -ne 0) { throw "Install failed: $($install.ExitCode)" }
$app = Join-Path $target 'PortWave.exe'
if (-not (Test-Path -LiteralPath $app)) { throw 'Installed EXE missing.' }
if ((Get-FileHash -LiteralPath $app).Hash -ne (Get-FileHash -LiteralPath (Join-Path $ProjectRoot 'release/portable/PortWave/PortWave.exe')).Hash) { throw 'Installed binary differs from portable build.' }
if (-not (Test-Path -LiteralPath $uninstallKey)) { throw 'Uninstall registration missing.' }
if (-not (Test-Path -LiteralPath (Join-Path $shortcuts 'PortWave.lnk'))) { throw 'Start Menu shortcut missing.' }
$pathBefore = $env:PATH
try {
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $run = Start-Process -FilePath $app -ArgumentList '--smoke-test','--demo' -WindowStyle Hidden -PassThru
    if (-not $run.WaitForExit(20000)) { $run.Kill(); throw 'Installed app timed out.' }
    if ($run.ExitCode -ne 0) { throw "Installed app failed: $($run.ExitCode)" }
} finally { $env:PATH = $pathBefore }
$sentinel = Join-Path $target 'user-data-must-survive.txt'
Set-Content -LiteralPath $sentinel -Value 'installer smoke sentinel' -Encoding utf8
$uninstall = Start-Process -FilePath (Join-Path $target 'Uninstall.exe') -ArgumentList '/S' -WindowStyle Hidden -PassThru
if (-not $uninstall.WaitForExit(30000)) { throw 'Uninstaller timed out.' }
$deadline = [DateTime]::UtcNow.AddSeconds(20)
while (((Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath (Join-Path $target 'Uninstall.exe')) -or (Test-Path -LiteralPath $uninstallKey)) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 200 }
if ((Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath (Join-Path $target 'Uninstall.exe')) -or (Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath $registration) -or (Test-Path -LiteralPath $shortcuts)) { throw 'Uninstall did not remove deployed files/registration/shortcuts.' }
if (-not (Test-Path -LiteralPath $sentinel)) { throw 'Uninstaller removed user data.' }
# Exact test-owned file; never recursively delete the installation directory.
Remove-Item -LiteralPath $sentinel
if (@(Get-ChildItem -LiteralPath $target -Force).Count -ne 0) { throw 'Unexpected files remain after uninstall.' }
Remove-Item -LiteralPath $target
Write-Output 'Installer smoke passed: Unicode path install, exact binary, no-SDK startup, shortcut/registration, uninstall and user-data preservation.'
