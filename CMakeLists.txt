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

include_directories(SYSTEM ${LIBUV_INCLUDE_DIRS})

include_directories(include deps/picohttpparser deps/libuv/include)

add_executable(http-server server.c deps/picohttpparser/picohttpparser.c)

if(WIN32)
    set(LIBUV_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/libuv.a)
	add_custom_command(
		OUTPUT ${LIBUV_LIBRARIES}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}\\deps\\libuv
		COMMAND mingw32-make
		ARGS -f Makefile.mingw CC=gcc
		)
	add_custom_target(libuv DEPENDS ${LIBUV_LIBRARIES})
	add_dependencies(http-server libuv)
	link_directories(${CMAKE_CURRENT_SOURCE_DIR}\\deps\\libuv)
	target_link_libraries(http-server ${LIBUV_LIBRARIES})
	target_link_libraries(http-server ws2_32 userenv psapi iphlpapi)
else()
    set(LIBUV_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/.libs/libuv.a)
	add_custom_command(
		OUTPUT ${LIBUV_LIBRARIES}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv
		COMMAND sh
		ARGS -c '[ ! -f .libs/libuv.a ] && (./autogen.sh && ./configure --enable-static --disable-shared) || make'
		)
	add_custom_target(libuv DEPENDS ${LIBUV_LIBRARIES})
	add_dependencies(http-server libuv)
	link_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/.libs)
	target_link_libraries(http-server ${LIBUV_LIBRARIES})
	target_link_libraries(http-server pthread rt)
endif()
