# 0018 Shared MSVC toolchain and release layout

SDKs and developer executables live outside the repository. The Windows default
is Program Files/PortWaveToolchain; PORTWAVE_TOOLCHAIN_ROOT can override a complete
environment. Ordinary builds do not change the system PATH or directory ACLs.
The supported Windows toolchain is MSVC v143 / Qt 6.8.3 msvc2022_64. core-debug
uses MSVC without Qt; the GitHub core CI also builds standard C++20 on Linux.

release.ps1 combines configure/build/release-regression/deploy/installer steps.
release/portable/PortWave is runnable as a directory and is also packaged as ZIP;
release/installer uses the same payload. package-sources.ps1 archives a committed
source revision and verified Qt module source archives. SHA256SUMS.txt uses flat
asset names for downloads from GitHub Releases.

out contains generated data. clean.ps1 defaults to compilation/packaging caches;
-All removes the complete out directory. Source, installed SDKs, release artifacts
and user settings are preserved; out-of-bound paths and reparse points are rejected.

The release regression subset is explicitly separate from historical
qt.integration. No tests were removed to hide legacy expectations. Existing
module responsibilities are preserved; no functional rewrite is implied by
publication or directory cleanup.
