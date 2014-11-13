cmake_minimum_required(VERSION 2.8)
project(http-server)
include(ExternalProject)
include(CheckLibraryExists)

include_directories(
	deps/picohttpparser
	deps/libuv
	deps/klib
)

if (CMAKE_COMPILER_IS_GNUCC)
	set(CMAKE_C_FLAGS "-O2 -g -Wall -Wno-unused-function ${CMAKE_C_FLAGS}")
endif()

set(LIBUV_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/lib/libuv.a)
include_directories(SYSTEM ${LIBUV_INCLUDE_DIRS})

include_directories(include deps/picohttpparser deps/libuv/include)

add_executable(http-server server.c deps/picohttpparser/picohttpparser.c)
if(WIN32)
	target_link_libraries(http-server libuv.a)
	target_link_libraries(http-server ws2_32 psapi iphlpapi)
else()
	target_link_libraries(http-server libuv.a)
	target_link_libraries(http-server pthread rt)
endif()

if(WIN32)
	link_directories(${CMAKE_CURRENT_SOURCE_DIR}\\deps\\libuv)
	add_custom_command(TARGET http-server PRE_BUILD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}\\deps\\libuv
		COMMAND mingw32-make
		ARGS -f Makefile.mingw
		)
else()
	link_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/.libs)
	add_custom_command(TARGET http-server PRE_BUILD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv
		COMMAND sh
		ARGS -c '[ ! -f .libs/libuv.a ] && (./autogen.sh && ./configure --enable-static --disable-shared) || make'
		)
endif()
