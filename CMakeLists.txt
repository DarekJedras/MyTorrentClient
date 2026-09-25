cmake_minimum_required(VERSION 3.20)
project(TorrentClient VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

if(MSVC)
	add_compile_options(/W4)
else()
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)
include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
	GIT_TAG v0.56.0
	FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-2
)

FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
    
    add_library(asio INTERFACE)
    
    target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src)
if (BUILD_TESTING)
	add_subdirectory(test)
endif()
