#!/bin/sh

# Verify that all entries in a blacklist file are still valid

usage() {
    echo "Usage: $0 <path-to-igt-runner> <test-binary-directory> <blacklist-files ...>"
    echo
    echo " path-to-igt-runner: For example build/runner/igt_runner"
    echo " test-binary-directory: For example build/tests"
    echo " blacklist-files: For example tests/intel-ci/blacklist.txt"
    exit 2
}

if [ $# -lt 3 ]; then
    usage
fi

RUNNER="$1"
shift
BINDIR="$1"
shift
BLFILES="$*"

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

for BLFILE in $BLFILES; do
    if [ ! -f "$BLFILE" ]; then
	echo "$BLFILE not found"
	echo
	usage
    fi
done

STATUS=0

TESTLIST="$("$RUNNER" --list-all "$BINDIR")"

for BLFILE in $BLFILES; do
    cat "$BLFILE" | while read line; do
	blentry=$(echo "$line" | sed 's/#.*//' | tr -d '[:space:]')
	if [ "$blentry" = "" ]; then continue; fi

	if ! (echo "$TESTLIST" | grep -Pq "$blentry") >/dev/null 2>/dev/null; then
	    echo "$BLFILE: Useless entry: $blentry"
	    STATUS=1
	fi
    done
done

exit $STATUS
