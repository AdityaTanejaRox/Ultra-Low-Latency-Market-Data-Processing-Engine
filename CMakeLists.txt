cmake_minimum_required(VERSION 3.20)
project(batonics_trading_challenge LANGUAGES CXX)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --- Fetch GoogleTest once, here ---
include(FetchContent)
# Helps MSVC link CRT correctly for gtest
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# Your code
add_subdirectory(src)

# Tests (now GTest is available)
enable_testing()
add_subdirectory(tests)
