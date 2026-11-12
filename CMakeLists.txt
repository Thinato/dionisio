cmake_minimum_required(VERSION 3.24)
project(dionisio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(DIONISIO_TUI "Build the dionisio admin TUI (FTXUI)" ON)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
                     URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(json)
find_package(CURL REQUIRED)

add_library(core STATIC src/core/core.cpp src/core/http.cpp)
target_include_directories(core PUBLIC src/core)
target_link_libraries(core PUBLIC nlohmann_json::nlohmann_json CURL::libcurl)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE core)
target_compile_definitions(core_test PRIVATE FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME core COMMAND core_test)

# cpp-httplib is header-only; SOURCE_SUBDIR skips its CMakeLists (no optional OpenSSL/zlib probing).
FetchContent_Declare(httplib URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.66.0.tar.gz
                     URL_HASH SHA256=537a77cdb96d3b4ad64d0b7103971de52eac75cd4ee03a2fdb84417b0d742459
                     SOURCE_SUBDIR none)
FetchContent_MakeAvailable(httplib)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
add_executable(dionisio-server src/server/main.cpp src/server/db.cpp src/server/slack.cpp src/server/gcs.cpp)
target_include_directories(dionisio-server SYSTEM PRIVATE ${httplib_SOURCE_DIR})
# Slack interaction payloads (view_submission echoes the whole modal) exceed httplib's 8 KB form default.
target_compile_definitions(dionisio-server PRIVATE CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576)
if(TARGET SQLite3::SQLite3)  # CMake >= 4.3 renamed it
  set(SQLITE_TARGET SQLite3::SQLite3)
else()
  set(SQLITE_TARGET SQLite::SQLite3)
endif()
target_link_libraries(dionisio-server PRIVATE core ${SQLITE_TARGET} OpenSSL::Crypto Threads::Threads)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(t api gcs)
  add_test(NAME ${t} COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/${t}_test.py $<TARGET_FILE:dionisio-server>)
endforeach()

if(DIONISIO_TUI)
  FetchContent_Declare(ftxui URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
                        URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88)
  FetchContent_MakeAvailable(ftxui)
  add_executable(dionisio src/tui/main.cpp)
  target_link_libraries(dionisio PRIVATE core ftxui::component ftxui::dom ftxui::screen)
endif()
