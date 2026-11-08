# Windows distribution

Run `scripts/release.ps1` to configure, build, run the release regression set,
deploy the portable application, create its ZIP, and compile the NSIS installer.
The binary payload is shared between both forms; SHA256SUMS.txt identifies assets.

- Portable directory: `release/portable/PortWave/`.
- Portable archive: `release/portable/PortWave-0.1.0-win64-portable.zip`.
- Installer: `release/installer/PortWave-Setup-0.1.0-x64.exe`.

The installer defaults to Program Files, requires administrator elevation and
adds Start Menu/uninstall entries. Close running instances before updating.
The application stores settings/logs in user-profile locations; uninstall does
not remove those files. The portable application is installation-free, but uses
the same user-profile settings rather than storing them beside the EXE.

NSIS 3.11 and the MSVC/Qt tools reside outside the repository, by default in
`C:\Program Files\PortWaveToolchain`. Ordinary build/package commands do not
require elevation; initial SDK installation does. See the root README.

Installers are currently unsigned. `scripts/test-installer.ps1` is an optional
administrator smoke test and refuses to overwrite an existing personal install.
Running the packaged EXE is a different check from installation/uninstallation.
