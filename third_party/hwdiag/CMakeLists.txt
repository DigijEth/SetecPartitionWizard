# libspw_hwdiag — Hardware Diagnostics Support Library
#
# This CMakeLists is used in TWO modes:
#
# 1. LIBRARY BUILD MODE (standalone — run by developer to produce .lib):
#    cd third_party/hwdiag && cmake -B build && cmake --build build
#    Produces lib/spw_hwdiag.lib which is committed to the repo.
#
# 2. CONSUMER MODE (from main project):
#    The main project just links against the pre-built .lib in lib/.
#    This CMakeLists is NOT add_subdirectory'd by the main project.

cmake_minimum_required(VERSION 3.25)
project(spw_hwdiag VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

# Internal sources (the secret implementations)
set(INTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/internal")

set(HWDIAG_SOURCES
    hwdiag_impl.cpp
    ${INTERNAL_DIR}/AstroChicken.cpp
    ${INTERNAL_DIR}/Vohaul.cpp
    ${INTERNAL_DIR}/Arnoid.cpp
    ${INTERNAL_DIR}/StarGenerator.cpp
    ${INTERNAL_DIR}/OratDecoder.cpp
    # Common dependencies needed by internal code
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/common/Logging.cpp
)

set(HWDIAG_HEADERS
    include/hwdiag.h
    ${INTERNAL_DIR}/AstroChicken.h
    ${INTERNAL_DIR}/Vohaul.h
    ${INTERNAL_DIR}/Arnoid.h
    ${INTERNAL_DIR}/StarGenerator.h
    ${INTERNAL_DIR}/OratDecoder.h
)

add_library(spw_hwdiag STATIC ${HWDIAG_SOURCES} ${HWDIAG_HEADERS})

target_include_directories(spw_hwdiag PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build/default/generated  # EmbeddedKey.h
)

target_link_libraries(spw_hwdiag PRIVATE
    Qt6::Widgets
    Qt6::Core
)

if(WIN32)
    target_link_libraries(spw_hwdiag PRIVATE setupapi wbemuuid ole32 oleaut32)
endif()

# Copy the built library to lib/ for distribution
add_custom_command(TARGET spw_hwdiag POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:spw_hwdiag>"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/$<TARGET_FILE_NAME:spw_hwdiag>"
    COMMENT "Copying library to lib/ for distribution..."
)
