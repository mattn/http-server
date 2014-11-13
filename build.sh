#!/bin/sh

#[ ! -d build ] && mkdir build && cd build && cmake .. || cd build && make
if [ ! -d build ]; then
	mkdir build && cd build && cmake .. && make
else
    cd build && make
fi
