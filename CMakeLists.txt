cmake_minimum_required(VERSION 3.25)
project(SetecPartitionWizard VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Platform defines — NOMINMAX prevents windows.h from defining min/max macros
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)

# CMake helpers
include(cmake/CompilerWarnings.cmake)
include(cmake/Version.cmake)
include(cmake/GenerateKey.cmake)

# Tests option (declared before third_party so FetchContent sees it)
option(SPW_BUILD_TESTS "Build tests" ON)

# Third-party dependencies
add_subdirectory(third_party)

# Main source tree
add_subdirectory(src)

# Tests
if(SPW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
