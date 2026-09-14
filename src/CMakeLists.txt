add_library(bencode
	bencode.cpp
)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
	GIT_TAG v0.56.0
)
FetchContent_MakeAvailable(httplib)

add_library(torrent_core
	torrent_file.cpp
	tracker.cpp
)
target_link_libraries(torrent_core PRIVATE bencode OpenSSL::Crypto httplib::httplib)

add_executable(torrent_client
	main.cpp
)
target_link_libraries(torrent_client PRIVATE torrent_core)
