add_library(bencode
	bencode.cpp
)

add_library(torrent_core
	torrent_file.cpp
	tracker.cpp
	udp_tracker.cpp
	peer.cpp
)

target_link_libraries(torrent_core PRIVATE
	bencode
	OpenSSL::Crypto
	OpenSSL::SSL
	httplib::httplib
	asio
)
target_compile_definitions(torrent_core PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

add_executable(torrent_client
	main.cpp
)
target_link_libraries(torrent_client PRIVATE torrent_core asio)
