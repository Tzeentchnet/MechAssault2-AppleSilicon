cmake_minimum_required(VERSION 3.20)
project(apple_silicon_dependencies C)

find_package(SDL2 2 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2_PC REQUIRED sdl2>=2)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(OPENSSL_PC REQUIRED openssl>=3)
find_package(OpenSSL 3 REQUIRED)

message(STATUS "SDL2 CMake config: ${SDL2_DIR}")
message(STATUS "SDL2 pkg-config: ${SDL2_PC_VERSION}")
message(STATUS "libepoxy pkg-config: ${EPOXY_VERSION}")
message(STATUS "OpenSSL pkg-config: ${OPENSSL_PC_VERSION}")

add_executable(apple_silicon_dependencies probe.c)
target_link_libraries(apple_silicon_dependencies PRIVATE
    SDL2::SDL2 PkgConfig::EPOXY OpenSSL::SSL OpenSSL::Crypto)
enable_testing()
add_test(NAME apple_silicon_dependencies COMMAND apple_silicon_dependencies)
