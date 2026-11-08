add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE portwave_core)
add_test(NAME core.regressions COMMAND core_tests)
set_tests_properties(core.regressions PROPERTIES LABELS "release")
if(PORTWAVE_BUILD_GUI)
  add_executable(qt_tests qt_tests.cpp)
  target_link_libraries(qt_tests PRIVATE portwave_ui Qt6::Test portwave_qscintilla)
  add_test(NAME qt.integration COMMAND qt_tests -platform offscreen)
  set_tests_properties(qt.integration PROPERTIES TIMEOUT 60)
  # Current supported behavior. The historical full suite remains available;
  # obsolete three-theme/pause-button expectations are tracked in validation.md.
  add_test(NAME qt.release COMMAND qt_tests -platform windows
    twoThemesAndConnectDisconnect freshProfileDefaultsToChromaticAndTwelvePoint
    plainTextLogSettingsAreStaged plainTextLogPolicyAndStreaming
    logOpenPreferencesAreStaged logDoubleClickUsesSavedAction
    logIncrementalRefreshPreservesExistingDocument logTailReadCheckpointsAndBounds
    logTailCancellationAndClone logReaderAnsiAcrossChunksAndFailedRefresh
    logWorkspaceLayoutRefreshAndRestore streamingFileBoundaryErrorsAndCancel
    streamingDocumentAndLargeMode logAnsiColorToggle ansiSelectionPreservesMetrics
    searchOverlayKeepsAnsiAndMarks lightContrastAndChromaticIsolation
    bottomStatusPositionsMatchAcrossThemes lightButtonsAndCompactTogglesHaveNormalSize
    appAndLogWindowsShareNativeIcon testConnectionLifecycle rotateOnlyLogging)
  set_tests_properties(qt.release PROPERTIES TIMEOUT 120 LABELS "release")
  add_test(NAME app.smoke COMMAND PortWave --smoke-test --demo -platform offscreen)
  set_tests_properties(app.smoke PROPERTIES TIMEOUT 20)
  set_tests_properties(app.smoke PROPERTIES LABELS "release")
endif()
