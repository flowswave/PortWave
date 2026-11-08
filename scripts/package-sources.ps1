param([string]$Ref = 'HEAD')
. "$PSScriptRoot/common.ps1"
$changes = git -C $ProjectRoot status --porcelain --untracked-files=normal
Require-Success 'Read source status'
if ($changes) { throw 'Commit source changes before making a release source archive.' }
$commit = git -C $ProjectRoot rev-parse --verify ($Ref + '^{commit}')
Require-Success 'Resolve source commit'
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $ProjectRoot 'CMakeLists.txt') -Raw), 'project\(PortWave VERSION ([0-9.]+)').Groups[1].Value
$release = Join-Path $ProjectRoot 'release'
$sources = Join-Path $release 'sources'
$downloads = Join-Path $ProjectRoot 'out/dependency-sources'
New-Item -ItemType Directory -Force $sources,$downloads | Out-Null
$sourceZip = Join-Path $sources "PortWave-$version-source.zip"
git -C $ProjectRoot archive --format=zip --prefix=PortWave/ ("--output=" + $sourceZip) $commit
Require-Success 'Archive committed source'
$metadata = Get-Content -LiteralPath (Join-Path $ProjectRoot 'third_party/dependencies.json') -Raw | ConvertFrom-Json
$archives = @()
foreach ($item in $metadata.qt.sourceArchives) {
    $path = Join-Path $downloads $item.file
    if (-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash -ne $item.sha256) {
        Invoke-WebRequest -Uri $item.url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path).Hash -ne $item.sha256) { throw "Dependency source checksum mismatch: $($item.file)" }
    $archives += $path
}
$dependencyZip = Join-Path $sources "PortWave-$version-dependency-sources.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($dependencyZip, [IO.FileMode]::Create)
$zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($path in $archives + @((Join-Path $ProjectRoot 'docs/SOURCES.md'))) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $path, [IO.Path]::GetFileName($path), [IO.Compression.CompressionLevel]::NoCompression) | Out-Null
    }
} finally { $zip.Dispose(); $stream.Dispose() }
$artifacts = foreach ($directory in @('portable','installer','sources')) {
    Get-ChildItem -LiteralPath (Join-Path $release $directory) -File | Where-Object Extension -In '.zip','.exe'
}
$lines = foreach ($artifact in $artifacts) { "$( (Get-FileHash -LiteralPath $artifact.FullName).Hash.ToLowerInvariant() )  $($artifact.Name)" }
[IO.File]::WriteAllLines((Join-Path $release 'SHA256SUMS.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
Write-Output "Source archives generated from $commit"
