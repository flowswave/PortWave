add_library(portwave_editor scintilla_editor.cpp)
target_link_libraries(portwave_editor PUBLIC portwave_options Qt6::Widgets PRIVATE portwave_qscintilla)
target_include_directories(portwave_editor PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(portwave_editor PRIVATE "${QSCI_ROOT}/scintilla/include")
