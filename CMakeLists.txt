cmake_minimum_required(VERSION 3.20)
project(arcbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
find_package(OpenSSL REQUIRED)

add_executable(arcbox
    src/main.cpp
    src/config.cpp
    src/util.cpp
    src/db.cpp
    src/adblock.cpp
    src/capture_http.cpp
    src/warc.cpp
    src/cgroup.cpp
    src/commands.cpp
)

target_include_directories(arcbox PRIVATE include)
target_compile_options(arcbox PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(arcbox PRIVATE
    PkgConfig::CURL
    PkgConfig::SQLITE3
    PkgConfig::ZLIB
    OpenSSL::Crypto
)

install(TARGETS arcbox RUNTIME DESTINATION bin)

enable_testing()
add_test(
    NAME smoke
    COMMAND ${CMAKE_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:arcbox>
)
