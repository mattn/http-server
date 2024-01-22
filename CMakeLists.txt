cmake_minimum_required(VERSION 2.8)
project(http-server)
include(ExternalProject)
include(CheckLibraryExists)

if (CMAKE_COMPILER_IS_GNUCC)
	set(CMAKE_C_FLAGS "-O2 -g -Wall -Wno-unused-function ${CMAKE_C_FLAGS}")
endif()

include_directories(
	http-server
	${PROJECT_SOURCE_DIR}/deps/picohttpparser
	${PROJECT_SOURCE_DIR}/deps/libuv/include
	${PROJECT_SOURCE_DIR}/deps/klib
)

if (CMAKE_COMPILER_IS_GNUCC)
    set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -iquote ${PROJECT_SOURCE_DIR}/deps/libuv/include")
endif()

add_executable(http-server server.c deps/picohttpparser/picohttpparser.c)

set(LIBUV_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/build/libuv.a)
add_custom_target(libuv DEPENDS ${LIBUV_LIBRARIES})
add_custom_command(
    OUTPUT ${LIBUV_LIBRARIES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv
    COMMAND sh
    ARGS -c 'cmake -B build && cmake --build build -t uv_a'
    )
add_dependencies(http-server libuv)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv)
target_link_libraries(http-server ${LIBUV_LIBRARIES})
if(WIN32)
    target_link_libraries(http-server ws2_32 userenv psapi iphlpapi secur32)
else()
	target_link_libraries(http-server pthread rt)
endif()
