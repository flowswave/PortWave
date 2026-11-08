add_library(portwave_features connection_tabs.cpp terminal_view.cpp terminal_pane.cpp log_workspace.cpp log_document_view.cpp window_caption.cpp find_dialog.cpp snap_layout.cpp split_grip.cpp)
target_link_libraries(portwave_features PUBLIC portwave_services portwave_editor Qt6::Widgets)
