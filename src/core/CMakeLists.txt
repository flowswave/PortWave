add_library(portwave_core protocol.cpp commands.cpp ansi.cpp)
target_include_directories(portwave_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(portwave_core PUBLIC portwave_options)
