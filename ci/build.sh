#!/bin/bash

set -eu

if [ $COMPILER = "clang" ]; then
	export CC=clang-$LLVM_VER
else
	export CC=gcc
fi

# configure & build
if [ $CHECK_TYPE = "debug" ]; then
	CFLAGS="-O0" ./configure --enable-debug --enable-cassert --enable-tap-tests --with-icu
else
	./configure --disable-debug --disable-cassert --enable-tap-tests --with-icu
fi

make -sj4
cd contrib
make -sj4
cd ..
