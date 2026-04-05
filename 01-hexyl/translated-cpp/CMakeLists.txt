cmake_minimum_required(VERSION 3.20)
project(hexyl CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic -Wswitch-enum)

# --- Dependencies via FetchContent ---
include(FetchContent)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(cli11)

# --- hexyl library ---
add_library(hexyl_lib
    src/colors.cpp
    src/input.cpp
    src/hexyl.cpp
    src/byte_offset.cpp
)
target_include_directories(hexyl_lib PUBLIC src)

# --- hexyl executable ---
add_executable(hexyl src/main.cpp)
target_link_libraries(hexyl PRIVATE hexyl_lib CLI11::CLI11)

# --- Tests ---
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
    enable_testing()

    add_executable(test_printer tests/test_printer.cpp)
    target_link_libraries(test_printer PRIVATE hexyl_lib Catch2::Catch2WithMain)

    add_executable(test_byte_offset tests/test_byte_offset.cpp)
    target_link_libraries(test_byte_offset PRIVATE hexyl_lib Catch2::Catch2WithMain)

    include(CTest)
    include(Catch)
    catch_discover_tests(test_printer)
    catch_discover_tests(test_byte_offset)
endif()
