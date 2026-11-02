cmake_minimum_required(VERSION 3.20)
project(dms-qt-client VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)

qt_standard_project_setup()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../etc/server-connection.json"
    "${CMAKE_CURRENT_BINARY_DIR}/server-connection.json"
    COPYONLY
)

qt_add_executable(dms-qt-client
    src/main.cpp
    src/main_window_files.cpp
    src/main_window.hpp
    src/main_window_internal.h
    src/main_window_menu.cpp
    src/main_window_network.cpp
    src/main_window_notifications.cpp
    src/main_window_session.cpp
    src/main_window_tree.cpp
    src/main_window_versions.cpp
)

target_link_libraries(dms-qt-client PRIVATE Qt6::Widgets Qt6::Network)

if(WIN32)
    set_target_properties(dms-qt-client PROPERTIES WIN32_EXECUTABLE ON)
endif()

if(MSVC)
    target_compile_options(dms-qt-client PRIVATE /W4 /permissive-)
else()
    target_compile_options(dms-qt-client PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS dms-qt-client RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../etc/server-connection.json"
        DESTINATION bin)
