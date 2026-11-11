cmake_minimum_required(VERSION 3.24)
project(dionisio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(DIONISIO_TUI "Build the dionisio admin TUI (FTXUI)" ON)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
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

# Server: every src/server/*.cpp (gcs.cpp included). Guarded so phases can land independently.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/server/main.cpp)
  # cpp-httplib is header-only; SOURCE_SUBDIR skips its CMakeLists (no optional OpenSSL/zlib probing).
  FetchContent_Declare(httplib URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.66.0.tar.gz
                       SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(httplib)
  find_package(SQLite3 REQUIRED)
  find_package(Threads REQUIRED)
  if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
  endif()
  find_package(OpenSSL REQUIRED)
  file(GLOB SERVER_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/server/*.cpp)
  add_executable(dionisio-server ${SERVER_SRC})
  target_include_directories(dionisio-server SYSTEM PRIVATE ${httplib_SOURCE_DIR})
  target_link_libraries(dionisio-server PRIVATE core SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(t api gcs)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/${t}_test.py)
      add_test(NAME ${t} COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/${t}_test.py
                                 $<TARGET_FILE:dionisio-server>)
    endif()
  endforeach()
endif()

if(DIONISIO_TUI AND EXISTS ${CMAKE_SOURCE_DIR}/src/tui/main.cpp)
  FetchContent_Declare(ftxui URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz)
  FetchContent_MakeAvailable(ftxui)
  add_executable(dionisio src/tui/main.cpp)
  target_link_libraries(dionisio PRIVATE core ftxui::component ftxui::dom ftxui::screen)
endif()
