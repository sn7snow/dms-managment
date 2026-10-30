cmake_minimum_required(VERSION 3.20)
project(dms LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

add_library(dms_core
    src/permission_provider.cpp
    src/storage.cpp
)
target_include_directories(dms_core PUBLIC include)
target_link_libraries(dms_core
    PUBLIC
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        Threads::Threads
)
target_compile_options(dms_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dms-server
    src/server_api.cpp
    src/server_http.cpp
    src/server_main.cpp
    src/server_runtime.cpp
)
target_link_libraries(dms-server PRIVATE dms_core)

add_executable(dms-cron src/cron_main.cpp)
target_link_libraries(dms-cron PRIVATE dms_core)

install(TARGETS dms-server dms-cron RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(dms-core-tests tests/core_tests.cpp)
    target_link_libraries(dms-core-tests PRIVATE dms_core)
    add_test(NAME dms-core-tests COMMAND dms-core-tests)
endif()
