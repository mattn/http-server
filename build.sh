#!/bin/sh

#[ ! -d build ] && mkdir build && cd build && cmake .. || cd build && make
if [ ! -d build ]; then
	echo 2
	mkdir build && cd build && cmake .. && make
else
    cd build && make
fi
