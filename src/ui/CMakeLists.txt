add_library(portwave_ui theme.cpp appearance_profile.cpp connection_dialog.cpp settings_dialog.cpp main_window.cpp resources.qrc)
target_link_libraries(portwave_ui PUBLIC portwave_features Qt6::Svg)
