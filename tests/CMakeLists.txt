include(GoogleTest)

add_executable(spw_tests
    test_result.cpp
)

target_include_directories(spw_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(spw_tests PRIVATE
    spw_core
    GTest::gtest_main
)

gtest_discover_tests(spw_tests)
