cmake_minimum_required(VERSION 3.20)
project(dms LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Werror)
add_link_options(-Wl,-z,relro,-z,now)

option(DMS_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(DMS_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "DMS_SANITIZE requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

find_program(DMS_CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
if(DMS_CLANG_TIDY_EXECUTABLE)
    set(CMAKE_CXX_CLANG_TIDY "${DMS_CLANG_TIDY_EXECUTABLE}")
endif()

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
add_executable(dms-server
    src/server_api.cpp
    src/server_api_support.cpp
    src/server_archive.cpp
    src/server_version_store.cpp
    src/server_http.cpp
    src/server_main.cpp
    src/server_runtime.cpp
)
target_link_libraries(dms-server PRIVATE dms_core)

add_executable(dms-cron src/cron_main.cpp)
target_link_libraries(dms-cron PRIVATE dms_core)

install(TARGETS dms-server dms-cron RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(DMS_CURL_EXECUTABLE NAMES curl REQUIRED)

    add_executable(dms-core-tests tests/core_tests.cpp)
    target_link_libraries(dms-core-tests PRIVATE dms_core)
    add_test(NAME dms-core-tests COMMAND dms-core-tests)
    add_test(
        NAME dms-e2e
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh"
                "$<TARGET_FILE:dms-server>"
                "${Python3_EXECUTABLE}"
                "${DMS_CURL_EXECUTABLE}"
    )
    set_tests_properties(dms-e2e PROPERTIES TIMEOUT 60)
endif()
