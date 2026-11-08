# Public release validation

Date: 2026-11-08. Platform: Windows x64, MSVC v143 (19.44), Qt 6.8.3 msvc2022_64.
Tools are outside the source tree, under the documented Program Files location.

The independent public source checkout was configured and compiled from scratch.
The release CTest set passed 3/3: core.regressions, qt.release and app.smoke.
The Qt release set contains 22 current-behavior cases plus initialization/cleanup.
It covers connection lifecycle, default appearance, log saving/open preferences,
incremental refresh and cancellation/clone boundaries, editor styles/marks and UI
status/control/icon behavior. The selected documentation screenshots use synthetic
connections/logs and were visually reviewed.

This is not a full historical Qt-suite result, a physical-device acceptance test,
or an administrator installation/uninstallation test. Legacy full-suite issues
remain documented in parity.md and the tests remain in the source tree.

Source privacy preparation excludes private Git history, internal chronological
reports, personal development paths, reference-only scripts, SDKs/build caches and
real device logs. Original third-party notices and source trees are preserved.
Only public account identity is used for the new public Git commit.
