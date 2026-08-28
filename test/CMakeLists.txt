add_executable(unit_tests
    test_bencode.cpp
)
target_link_libraries(
    unit_tests PRIVATE bencode GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)