FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG v1.14.0
    FIND_PACKAGE_ARGS
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(unit_tests
    unit/test_bencode.cpp
    unit/test_torrent_file.cpp
    unit/test_tracker.cpp
    unit/test_utils.cpp
)
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    bencode
    torrent_core
    asio
)

add_executable(integration_tests
    integration/test_announce_http.cpp
    integration/test_peer_communication.cpp
)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    httplib::httplib
    torrent_core
    asio
)

gtest_discover_tests(unit_tests)
gtest_discover_tests(integration_tests)
