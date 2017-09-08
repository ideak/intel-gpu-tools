#!/bin/bash

cat > Makefile <<EOF

.PHONY: default docs
default: all

build/build.ninja:
	mkdir build
	meson

all: build/build.ninja
	ninja -C build

clean: build/build.ninja
	ninja -C build clean

test: build/build.ninja
	ninja -C build test

reconfigure: build/build.ninja
	ninja -C build reconfigure

check distcheck dist distclean:
	echo "This is the meson wrapper, not automake" && false

install uninstall:
	echo "meson install support not yet completed" && false

docs:
	echo "meson gtkdoc support not yet completed" && false

EOF

make $@
