add_executable(torrent_client
	main.cpp
	bencode.cpp
)
target_include_directories(torrent_client PRIVATE ${CMAKE_SOURCE_DIR}/include)
