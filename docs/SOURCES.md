# Source and rebuild materials

## PortWave, QScintilla and miniz

The public repository/tag and `PortWave-0.1.0-source.zip` contain PortWave source,
the unmodified QScintilla 2.14.1 and miniz 3.1.2 source trees, original notices,
CMake definitions and build/package scripts. See the root README to rebuild with
MSVC v143 and Qt 6.8.3. No private project or Git history is needed.

QScintilla original archive: [QScintilla_src-2.14.1.tar.gz](https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz),
SHA256 `dfe13c6acc9d85dfcba76ccc8061e71a223957a6c02f3c343b30a9d43a4cdd4d`.

## Qt runtime modules

Binary releases use the official, unmodified Qt 6.8.3 MSVC build of Qt Base,
SVG and SerialPort. Original module source archives are provided as the
`PortWave-0.1.0-dependency-sources.zip` attachment on the same Release. They are
also available at these official locations; hashes were verified against Qt's
published archive metadata:

| Module | Original archive | SHA256 |
|---|---|---|
| qtbase | [qtbase-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz) | `56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80` |
| qtsvg | [qtsvg-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtsvg-everywhere-src-6.8.3.tar.xz) | `35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438` |
| qtserialport | [qtserialport-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtserialport-everywhere-src-6.8.3.tar.xz) | `c164a0392dc7cab3d072fa8fdcacc8fc4f3e685234b021b3128020719cdf5fa5` |

Unpack the original Qt archives to obtain their CMake build instructions and
bundled third-party source/license notices. Qt's platform compiler/SDK requirements
also apply. Runtime SBOM and notices are in the binary package's `licenses/`.

## Platform tools

Microsoft compiler, Windows SDK and VC runtime/platform redistributables are not
PortWave-owned source. They keep Microsoft's terms. Developer tools and SDKs are
not committed or bundled as application source; their versions and source URLs
are recorded in `third_party/dependencies.json`.

## Integrity

The Release `SHA256SUMS.txt` covers the portable ZIP, installer, PortWave source
ZIP and dependency-source ZIP. Source archives are not executables. Keep notices
and corresponding source/build materials with redistributed binaries.
