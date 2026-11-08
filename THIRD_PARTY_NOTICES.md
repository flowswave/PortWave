# Third-party notices

PortWave-owned code is GPL-3.0-only. Third-party components retain their own terms.

| Component | Version | Use / license location |
|---|---|---|
| QScintilla | 2.14.1 | Unmodified source, statically linked under GPLv3; `third_party/QScintilla_src-2.14.1/LICENSE` and original file notices |
| Scintilla and lexers | bundled in QScintilla 2.14.1 | Original permissive notices in `third_party/QScintilla_src-2.14.1/scintilla/License.txt` and individual files |
| miniz | 3.1.2 | Unmodified source, MIT; `third_party/miniz-3.1.2/LICENSE` |
| Qt | 6.8.3 | Dynamically linked Qt Base, SVG and SerialPort; original module/third-party terms and SBOM are shipped in binary `licenses/` |
| Microsoft VC runtime and platform DLLs | toolchain-supplied | Redistributable/platform binaries, not relicensed by PortWave |
| NSIS | 3.11 | Build tool only; `third_party/licenses/NSIS.txt` |

Compiler and developer tools are not included in the source repository or application.
Their version/source records are in `third_party/dependencies.json`.

The application icon was provided by the maintainer. The maintainer confirmed
permission to publish the UI resources carried over from the earlier PortWave
implementation. See `src/ui/resources/ORIGIN.md` for provenance.

Component source locations and rebuild instructions: [docs/SOURCES.md](docs/SOURCES.md).
