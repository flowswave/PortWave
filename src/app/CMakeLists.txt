add_executable(PortWave WIN32 main.cpp)
if (WIN32)
    # Embeds the application icon so Explorer, the taskbar, the Start Menu shortcut
    # and the uninstall entry show the PortWave mark instead of a blank default.
    target_sources(PortWave PRIVATE PortWave.rc)
  # RC's referenced ICO is not reliably discovered by Ninja on Windows.
  # Recompile the EXE resource whenever the selected artwork changes.
  set_source_files_properties(PortWave.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/PortWave.ico")
endif()
target_link_libraries(PortWave PRIVATE portwave_ui)
target_compile_definitions(PortWave PRIVATE PORTWAVE_VERSION="${PROJECT_VERSION}")
install(TARGETS PortWave RUNTIME DESTINATION .)
