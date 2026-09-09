add_library(bencode
	bencode.cpp
)

add_library(torrent_file
	torrent_file.cpp
)
target_link_libraries(torrent_file PRIVATE bencode OpenSSL::Crypto)

add_executable(torrent_client
	main.cpp
)
target_link_libraries(torrent_client PRIVATE torrent_file)
