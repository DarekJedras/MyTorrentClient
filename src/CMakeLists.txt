add_library(bencode
	bencode.cpp
)
target_include_directories(bencode PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(torrent_client
	main.cpp
)
target_link_libraries(torrent_client PRIVATE bencode)
