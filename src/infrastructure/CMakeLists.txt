add_library(portwave_infrastructure settings.cpp session_worker.cpp log_writer.cpp workspace_archive.cpp named_layout_store.cpp log_workspace_store.cpp)
target_link_libraries(portwave_infrastructure PUBLIC portwave_core miniz Qt6::Network Qt6::SerialPort)
target_include_directories(portwave_infrastructure PUBLIC "${PROJECT_SOURCE_DIR}/src")
