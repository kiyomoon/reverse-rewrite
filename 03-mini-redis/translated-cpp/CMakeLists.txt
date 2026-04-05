cmake_minimum_required(VERSION 3.20)
project(mini-redis CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fetch standalone ASIO (header-only, no Boost dependency)
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asio)

# Create ASIO interface library
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_HAS_CO_AWAIT)

# Server executable
add_executable(mini-redis-server
    src/server_main.cpp
    src/server.cpp
)

# Shared library for common code
add_library(mini-redis-common STATIC
    src/connection.cpp
    src/frame.cpp
    src/parse.cpp
    src/command.cpp
    src/db.cpp
)
target_link_libraries(mini-redis-common PUBLIC asio)

# CLI executable
add_executable(mini-redis-cli
    src/cli_main.cpp
    src/client.cpp
)
target_link_libraries(mini-redis-cli PRIVATE mini-redis-common)

# Update server to use common library
target_link_libraries(mini-redis-server PRIVATE mini-redis-common)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(mini-redis-common PUBLIC Threads::Threads)
endif()
