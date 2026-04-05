cmake_minimum_required(VERSION 3.20)
project(cat_cpp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The cat utility — uses POSIX I/O, no external dependencies
add_executable(cat src/cat.cpp)

# Enable warnings
target_compile_options(cat PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
