cmake_minimum_required(VERSION 3.25)
project(SetecPartitionWizard VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

# CMake helpers
include(cmake/CompilerWarnings.cmake)
include(cmake/Version.cmake)

# Third-party dependencies
add_subdirectory(third_party)

# Main source tree
add_subdirectory(src)

# Tests
option(SPW_BUILD_TESTS "Build tests" ON)
if(SPW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
