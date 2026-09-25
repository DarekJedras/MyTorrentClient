FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG v1.14.0
    FIND_PACKAGE_ARGS
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(unit_tests
    test_bencode.cpp
    test_torrent_file.cpp
    test_tracker.cpp
)
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    bencode
    torrent_core
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
