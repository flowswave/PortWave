# Detect /showIncludes using the compiler's actual UTF-8 output. On Chinese Windows
# CMake's initial compiler probe can decode it with the ANSI codepage, which gives
# Ninja a mojibake prefix and silently loses every header dependency.
#
# CMake 4.x scans localized MSVC /showIncludes output itself and tracks header
# dependencies correctly. Publishing the legacy override there is actively
# harmful: the localized prefix is GBK text decoded as UTF-8, so the value is not
# valid UTF-8 and CMake then silently stops writing CMakeFiles/rules.ninja, which
# makes every following Ninja invocation fail with "unknown build rule".
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  if(NOT CMAKE_VERSION VERSION_LESS 4.0)
    message(STATUS "CMake ${CMAKE_VERSION} handles localized MSVC /showIncludes output; skipping legacy prefix override")
    return()
  endif()

  set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/portwave-include-probe.cpp")
  file(WRITE "${_probe}" "#include <cstddef>\n")
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c "${_probe}" "/Fo${CMAKE_BINARY_DIR}/CMakeFiles/portwave-include-probe.obj"
    OUTPUT_VARIABLE _include_output ERROR_VARIABLE _include_error
    RESULT_VARIABLE _include_result ENCODING UTF-8)
  if(NOT _include_result EQUAL 0)
    message(FATAL_ERROR "MSVC dependency probe failed: ${_include_error}")
  endif()
  string(REPLACE "\r" "" _include_output "${_include_output}")
  string(REPLACE "\n" ";" _include_lines "${_include_output}")
  set(_prefix_found FALSE)
  foreach(_line IN LISTS _include_lines)
    if(_line MATCHES "^(.+:[ \t]+)[A-Za-z]:[/\\\\]")
      set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
      set(_prefix_found TRUE)
      break()
    endif()
  endforeach()
  if(NOT _prefix_found)
    message(FATAL_ERROR "Could not detect MSVC /showIncludes prefix: ${_include_output}")
  endif()
endif()
