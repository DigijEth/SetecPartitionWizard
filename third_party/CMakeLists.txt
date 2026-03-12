# Third-party dependencies managed via FetchContent
include(FetchContent)

# Google Test (for testing only)
if(SPW_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# zstd (compression for disk images)
# For now, we'll use system zstd or fetch it later
# FetchContent_Declare(
#     zstd
#     GIT_REPOSITORY https://github.com/facebook/zstd.git
#     GIT_TAG v1.5.5
#     SOURCE_SUBDIR build/cmake
# )
# FetchContent_MakeAvailable(zstd)
