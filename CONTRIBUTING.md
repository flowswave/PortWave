# Contributing

Start with the root README, `src/README.md` and `docs/architecture.md`. Keep changes
within their owning module and preserve the documented thread/ownership/error
boundaries. Do not move protocol, disk or settings logic into MainWindow.

Use UTF-8, C++20 and the repository clang-format. For behavior fixes add a useful
regression; for UI work inspect real screenshots. New dependencies require pinned
version/source/license records. Keep upstream vendored sources unmodified.

Before submitting, run `scripts/format.ps1 -Check`, build and run the relevant
tests. State exactly what was tested, including hardware/GUI limitations. The
release test subset is not the historical full Qt test suite.

Do not include SDKs, build output, private logs, credentials or personal paths.
Contributions to PortWave-owned files are submitted under GPL-3.0-only; original
third-party terms remain intact. Open an issue first for broad architectural work.
