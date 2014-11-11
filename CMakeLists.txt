cmake_minimum_required(VERSION 2.8)
project(http-server)
include(ExternalProject)
include(CheckLibraryExists)

#set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

if (CMAKE_COMPILER_IS_GNUCC)
  set(CMAKE_C_FLAGS "-O2 -g -Wall -Wno-unused-function ${CMAKE_C_FLAGS}")
endif()

ExternalProject_Add(libuv
  #GIT_REPOSITORY https://github.com/joyent/libuv
  SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps/libuv
  CONFIGURE_COMMAND cd ${CMAKE_CURRENT_BINARY_DIR}/deps/libuv && ./autogen.sh && ./configure
  BUILD_COMMAND cd ${CMAKE_CURRENT_BINARY_DIR}/deps/libuv && make
  INSTALL_COMMAND ""
)
include_directories(SYSTEM ${LIBUV_INCLUDE_DIRS})

include_directories(include deps/picohttpparser deps/libuv/include)

add_executable(http-server server.c request.c deps/picohttpparser/picohttpparser.c)
target_link_libraries(http-server uv)

if(WIN32)
  target_link_libraries(http-server ws2_32 psapi iphlpapi)
else()
  target_link_libraries(http-server pthread rt)
endif()
