param([ValidateSet('msvc-release')][string]$Preset='msvc-release')
. "$PSScriptRoot\common.ps1"
Initialize-Toolchain $Preset
$destination = Join-Path $ProjectRoot 'out/package/msvc-release'
Remove-GeneratedDirectory $destination (Join-Path $ProjectRoot 'out')
cmake --install (Join-Path $ProjectRoot "out/build/$Preset") --prefix $destination
Require-Success 'Install application'
$qtRoot = $env:QT_MSVC_ROOT
& (Join-Path $qtRoot 'bin/windeployqt.exe') --release --no-translations --no-compiler-runtime (Join-Path $destination 'PortWave.exe')
Require-Success 'windeployqt'
if ($Preset.StartsWith('msvc')) {
    $crtDirectory = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
    if (-not (Test-Path -LiteralPath $crtDirectory)) { throw "MSVC redistributable CRT not found: $crtDirectory" }
    foreach ($runtimeFile in Get-ChildItem -LiteralPath $crtDirectory -Filter '*.dll' -File) {
        $runtimeTarget = Join-Path $destination $runtimeFile.Name
        # An unchanged CRT can stay loaded by the previous application instance.
        # Avoid forcing a write to that mapped DLL during an in-place EXE update.
        if ((Test-Path -LiteralPath $runtimeTarget) -and
            (Get-FileHash -LiteralPath $runtimeFile.FullName).Hash -eq (Get-FileHash -LiteralPath $runtimeTarget).Hash) {
            continue
        }
        Copy-Item -LiteralPath $runtimeFile.FullName -Destination $runtimeTarget -Force
    }
}
New-Item -ItemType Directory -Force (Join-Path $destination 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'LICENSE') -Destination (Join-Path $destination 'licenses/PortWave-GPL-3.0.txt')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'NOTICE') -Destination (Join-Path $destination 'licenses/PortWave-NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $destination 'licenses/THIRD_PARTY_NOTICES.md')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'docs/SOURCES.md') -Destination (Join-Path $destination 'licenses/SOURCES.md')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'third_party/QScintilla_src-2.14.1/LICENSE') -Destination (Join-Path $destination 'licenses/QScintilla-GPL3.txt')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'third_party/miniz-3.1.2/LICENSE') -Destination (Join-Path $destination 'licenses/miniz-MIT.txt')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'docs/licenses.md') -Destination (Join-Path $destination 'licenses/NOTICE.md')
Copy-Item -Path (Join-Path $ProjectRoot 'third_party/licenses/*.txt') -Destination (Join-Path $destination 'licenses') -Force
Copy-Item -LiteralPath (Join-Path $qtRoot 'sbom') -Destination (Join-Path $destination 'licenses/Qt-SBOM') -Recurse -Force
$licenses = Join-Path $qtRoot 'licenses'
if (Test-Path -LiteralPath $licenses) { Copy-Item -LiteralPath $licenses -Destination (Join-Path $destination 'licenses/Qt') -Recurse -Force }
$previousPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = Start-Process -FilePath (Join-Path $destination 'PortWave.exe') -ArgumentList '--smoke-test','--demo' -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) { $process.Kill(); throw 'Packaged EXE failed to exit within 20 seconds.' }
    if ($process.ExitCode -ne 0) { throw "Packaged EXE failed: $($process.ExitCode)" }
} finally { $env:PATH = $previousPath }
$portableRoot = Join-Path $ProjectRoot 'release/portable'
$portable = Join-Path $portableRoot 'PortWave'
Remove-GeneratedDirectory $portable $portableRoot
New-Item -ItemType Directory -Force $portableRoot | Out-Null
Copy-Item -LiteralPath $destination -Destination $portable -Recurse
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $ProjectRoot 'CMakeLists.txt') -Raw), 'project\(PortWave VERSION ([0-9.]+)').Groups[1].Value
$archive = Join-Path $portableRoot "PortWave-$version-win64-portable.zip"
Compress-Archive -LiteralPath $portable -DestinationPath $archive -Force
Write-Output "Portable startup verified: $portable"
Write-Output "Portable archive: $archive"
