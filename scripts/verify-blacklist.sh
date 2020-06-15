#!/bin/sh

# Verify that all entries in a blacklist file are still valid

usage() {
    echo "Usage: $0 <path-to-igt-runner> <test-binary-directory> <blacklist-file>"
    echo
    echo " path-to-igt-runner: For example build/runner/igt_runner"
    echo " test-binary-directory: For example build/tests"
    echo " blacklist-file: For example tests/intel-ci/blacklist.txt"
    exit 2
}

if [ $# -ne 3 ]; then
    usage
fi

RUNNER="$1"
BINDIR="$2"
BLFILE="$3"

if [ ! -x "$RUNNER" ]; then
    echo "$RUNNER not found"
    echo
    usage
fi

if [ ! -f "$BINDIR/test-list.txt" ]; then
    echo "$BINDIR doesn't look like a test-binary directory"
    echo
    usage
fi

if [ ! -f "$BLFILE" ]; then
    echo "$BLFILE not found"
    echo
    usage
fi

STATUS=0

TESTLIST="$("$RUNNER" --list-all "$BINDIR")"

cat "$BLFILE" | while read line; do
    blentry=$(echo "$line" | sed 's/#.*//' | tr -d '[:space:]')
    if [ "$blentry" = "" ]; then continue; fi

    if ! (echo "$TESTLIST" | grep -Pq "$blentry") >/dev/null 2>/dev/null; then
	echo Useless blacklist entry: "$blentry"
	STATUS=1
    fi
done

exit $STATUS
