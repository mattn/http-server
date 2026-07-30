cmake_minimum_required(VERSION 2.8)
project(http-server)
include(ExternalProject)
include(CheckLibraryExists)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -Wno-unused-function ${CMAKE_C_FLAGS_RELEASE}")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG -Wall -Wextra -Wno-unused-function ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

include_directories(
	http-server
	${PROJECT_SOURCE_DIR}/deps/picohttpparser
	${PROJECT_SOURCE_DIR}/deps/libuv/include
	${PROJECT_SOURCE_DIR}/deps/klib
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -iquote ${PROJECT_SOURCE_DIR}/deps/libuv/include")
endif()

add_executable(http-server server.c deps/picohttpparser/picohttpparser.c)

set(LIBUV_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/build/libuv.a)
add_custom_target(libuv DEPENDS ${LIBUV_LIBRARIES})
add_custom_command(
    OUTPUT ${LIBUV_LIBRARIES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv
    COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" -B build
    COMMAND ${CMAKE_COMMAND} --build build -t uv_a
    )
add_dependencies(http-server libuv)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv)
target_link_libraries(http-server ${LIBUV_LIBRARIES})
if(WIN32)
    target_link_libraries(http-server ws2_32 userenv psapi dbghelp iphlpapi secur32)
else()
	target_link_libraries(http-server pthread rt)
endif()
