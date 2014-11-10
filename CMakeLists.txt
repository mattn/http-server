CMAKE_MINIMUM_REQUIRED(VERSION 2.8)
CMAKE_POLICY(SET CMP0003 NEW)

PROJECT(http-server)

IF (CMAKE_COMPILER_IS_GNUCC)
SET(CMAKE_C_FLAGS "-O2 -g -Wall -Wno-unused-function ${CMAKE_C_FLAGS}")
ENDIF()

INCLUDE_DIRECTORIES(include deps/picohttpparser deps/klib)

ADD_EXECUTABLE(http-server server.c request.c deps/picohttpparser/picohttpparser.c)
TARGET_LINK_LIBRARIES(http-server uv)

IF(WIN32)
    TARGET_LINK_LIBRARIES(http-server ws2_32 psapi iphlpapi)
ELSE()
    TARGET_LINK_LIBRARIES(http-server pthread rt)
ENDIF()
