#!/bin/bash

echo TESTLIST > $MESON_BUILD_ROOT/tests/test-list.txt

while [[ $# -gt 0 ]] ; do
	echo $1 >> $MESON_BUILD_ROOT/tests/test-list.txt
	shift
done

echo END TESTLIST >> $MESON_BUILD_ROOT/tests/test-list.txt
