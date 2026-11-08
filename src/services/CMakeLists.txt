add_library(portwave_services session.cpp file_service.cpp search_service.cpp)
target_link_libraries(portwave_services PUBLIC portwave_infrastructure Qt6::Concurrent)
