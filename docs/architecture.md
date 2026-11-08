# Architecture

PortWave uses independent CMake targets with this dependency direction:

```text
app -> ui -> features -> services -> infrastructure -> core
                  \-> editor (EditorInterface / QScintilla adapter)
```

`core` is standard C++20 without Qt: framing/CRC, command conversion, ANSI and
bounded queues. `infrastructure` owns device workers, log writing and persistence.
`services` exposes cancellable asynchronous session, file and search operations.
`features` coordinates terminal/log workspaces, search results, tabs and layouts.
`ui` binds those operations to the main/settings windows and semantic themes;
`app` owns process startup, command-line parsing and native resources.

All QWidget operations belong to the GUI thread. Device and disk tasks run in
workers; stop/close finishes asynchronously. RX/TX queues and result counts are
bounded, with observable loss/rejection, rather than one GUI signal per message.
Changing a theme updates views without reconnecting or mutating send drafts.

Each connection retains its editor, session and draft. MainWindow coordinates
shared sending controls and status presentation, never protocol or persistence.
Each log workspace owns its documents and independent search state; log focus
does not change the terminal send target.

Initial log loading streams UTF-8/ANSI in a worker into a detached Scintilla
ILoader. Successful completion adopts the document on the GUI thread. Refresh
uses a committed source-byte checkpoint and decoder state, reads at most 256 KiB
per batch and appends only the new range to the same document. Existing selection,
scroll and markers remain intact. Cancellation/error does not advance the cursor.
Truncation/replacement preserves the old page and asks the user to reopen it.

Settings use staged Apply/Cancel semantics. Appearance is stored per theme;
diagnostics are bounded and exclude received/transmitted bodies or credentials.
Editor-specific operations remain behind EditorInterface.

See [module map](../src/README.md), [coding conventions](coding-style.md),
[incremental refresh decision](decisions/0017-incremental-log-refresh.md) and
[toolchain/release decision](decisions/0018-shared-msvc-toolchain-and-release-layout.md).
