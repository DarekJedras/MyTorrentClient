link_libraries(GTest::gtest_main)

add_executable(unit_tests
    test_bencode.cpp
    test_torrent_file.cpp
)
target_link_libraries(
    unit_tests PRIVATE bencode torrent_file
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
