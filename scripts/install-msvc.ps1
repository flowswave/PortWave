param([string]$InstallPath = (Join-Path $env:ProgramFiles 'PortWaveToolchain/VSBuildTools'))

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$installerPath = Join-Path $env:TEMP 'PortWave-vs_BuildTools.exe'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswherePath) {
    $installed = & $vswherePath -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($installed) {
        Write-Output "VS 2022 C++ tools already installed: $installed"
        return
    }
}
if (Get-Process -Name vs_BuildTools,vs_setup_bootstrapper,setup -ErrorAction SilentlyContinue) {
    throw 'An installer is already running. Finish or close its window before starting another installation.'
}
if (-not (Test-Path -LiteralPath $installerPath)) {
    New-Item -ItemType Directory -Force (Split-Path $installerPath -Parent) | Out-Null
    Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $installerPath
}
$signature = Get-AuthenticodeSignature -LiteralPath $installerPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)CN=Microsoft Corporation(,|$)') {
    throw 'The installer must have a valid Microsoft Corporation Authenticode signature.'
}
Write-Output "Opening the signed Visual Studio 2022 Build Tools installer for $InstallPath."
Write-Output 'Windows may require you to approve its UAC prompt. Complete the C++ workload installation in the installer window.'
# Visible interactive window is intentional: the user must handle UAC and installation confirmation.
# --norestart must not be used here: Microsoft only permits it with --quiet or --passive.
Start-Process -FilePath $installerPath -ArgumentList @('--installPath', ('"' + $InstallPath + '"'), '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended') | Out-Null
