#!/bin/bash
#
# Copyright © 2014 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.


ROOT="`dirname $0`"
ROOT="`readlink -f $ROOT/..`"
IGT_CONFIG_PATH="`readlink -f ${IGT_CONFIG_PATH:-$HOME/.igtrc}`"
RESULTS="$ROOT/results"
PIGLIT=`which piglit 2> /dev/null`
IGT_RUNNER=
IGT_RESUME=
USE_PIGLIT=0
RUNNER=
RESUME=
BLACKLIST=

function find_file # basename <possible paths>
{
	base=$1
	shift

	while [ -n "$1" ]; do
		if [ -f "$1/$base" ]; then
			echo "$1/$base";
			return 0
		fi
		shift
	done

	return 1
}

if [ -z "$IGT_TEST_ROOT" ]; then
	p=$(find_file test-list.txt \
		    "$ROOT/build/tests" \
		    "$ROOT/tests" )
	if [ -f "$p" ]; then
		echo "Found test list: \"$p\"" >&2
		IGT_TEST_ROOT=$(dirname "$p")
	fi
fi

if [ -z "$IGT_TEST_ROOT" ]; then
	echo "Error: test list not found."
	echo "Please build tests to generate the test list or use IGT_TEST_ROOT env var."
	exit 1
fi

IGT_TEST_ROOT="`readlink -f ${IGT_TEST_ROOT}`"

function find_runner_binaries
{
	IGT_RUNNER=$(find_file igt_runner "$ROOT/build/runner" "$ROOT/runner")
	IGT_RESUME=$(find_file igt_resume "$ROOT/build/runner" "$ROOT/runner")
}

function download_piglit {
	git clone https://anongit.freedesktop.org/git/piglit.git "$ROOT/piglit"
}

function execute_runner # as-root <runner> <args>
{
	local need_root=$1
	shift
	local runner=$1
	shift
	local sudo

	export IGT_TEST_ROOT IGT_CONFIG_PATH

	if [ "$need_root" -ne 0 -a "$EUID" -ne 0 ]; then
		sudo="sudo --preserve-env=IGT_TEST_ROOT,IGT_CONFIG_PATH"
	fi

	$sudo $runner "$@"
}

function print_help {
	echo "Usage: run-tests.sh [options]"
	echo "Available options:"
	echo "  -d              download Piglit to $ROOT/piglit"
	echo "  -h              display this help message"
	echo "  -l              list all available tests"
	echo "  -r <directory>  store the results in directory"
	echo "                  (default: $RESULTS)"
	echo "  -s              create html summary"
	echo "  -t <regex>      only include tests that match the regular expression"
	echo "                  (can be used more than once)"
	echo "  -T <filename>   run tests listed in testlist"
	echo "                  (overrides -t and -x when running with piglit)"
	echo "  -v              enable verbose mode"
	echo "  -x <regex>      exclude tests that match the regular expression"
	echo "                  (can be used more than once)"
	echo "  -b              blacklist file to use for filtering"
	echo "                  (can be used more than once)"
	echo "                  (not supported by Piglit)"
	echo "  -R              resume interrupted test where the partial results"
	echo "                  are in the directory given by -r"
	echo "  -n              do not retry incomplete tests when resuming a"
	echo "                  test run with -R"
	echo "                  (only valid for Piglit)"
	echo "  -p              use Piglit instead of igt_runner"
	echo ""
	echo "Useful patterns for test filtering are described in the API documentation."
}

while getopts ":dhlr:st:T:vx:Rnpb:" opt; do
	case $opt in
		d) download_piglit; exit ;;
		h) print_help; exit ;;
		l) LIST_TESTS="true" ;;
		r) RESULTS="$OPTARG" ;;
		s) SUMMARY="html" ;;
		t) FILTER="$FILTER -t $OPTARG" ;;
		T) FILTER="$FILTER --test-list $OPTARG" ;;
		v) VERBOSE="-l verbose" ;;
		x) EXCLUDE="$EXCLUDE -x $OPTARG" ;;
		R) RESUME_RUN="true" ;;
		n) NORETRY="--no-retry" ;;
		p) USE_PIGLIT=1 ;;
		b) BLACKLIST="$BLACKLIST -b $OPTARG" ;;
		:)
			echo "Option -$OPTARG requires an argument."
			exit 1
			;;
		\?)
			echo "Unknown option: -$OPTARG"
			print_help
			exit 1
			;;
	esac
done
shift $(($OPTIND-1))

if [ "x$1" != "x" ]; then
	echo "Unknown option: $1"
	print_help
	exit 1
fi

if [ "x$PIGLIT" == "x" ]; then
	PIGLIT="$ROOT/piglit/piglit"
fi

RUN_ARGS=
RESUME_ARGS=
LIST_ARGS=
if [ "$USE_PIGLIT" -eq "1" ]; then
	if [ ! -x "$PIGLIT" ]; then
		echo "Could not find Piglit."
		echo "Please install Piglit or use -d to download Piglit locally."
		exit 1
	fi

	RUNNER=$PIGLIT
	RESUME=$PIGLIT
	RUN_ARGS="run igt --ignore-missing"
	RESUME_ARGS="resume $NORETRY"
	LIST_ARGS="print-cmd igt --format {name}"
else
	find_runner_binaries
	if [ ! -x "$IGT_RUNNER" -o ! -x "$IGT_RESUME" ]; then
		echo "Could not find igt_runner binaries."
		echo "Please build the runner, or use Piglit with the -p flag."
		exit 1
	fi

	RUNNER=$IGT_RUNNER
	RESUME=$IGT_RESUME
	RUN_ARGS="$BLACKLIST"
	LIST_ARGS="-L $BLACKLIST"
fi

if [ "x$LIST_TESTS" != "x" ]; then
	execute_runner 0 $RUNNER $LIST_ARGS
	exit
fi

if [ "x$RESUME_RUN" != "x" ]; then
	execute_runner 1 $RESUME $RESUME_ARGS "$RESULTS"
else
	mkdir -p "$RESULTS"
	execute_runner 1 $RUNNER $RUN_ARGS -o -s "$RESULTS" $VERBOSE $EXCLUDE $FILTER
fi

if [ "$SUMMARY" == "html" ]; then
	if [ ! -x "$PIGLIT" ]; then
		echo "Could not find Piglit, required for HTML generation."
		echo "Please install Piglit or use -d to download Piglit locally."
		exit 1
	fi

	execute_runner 0 $PIGLIT summary html --overwrite "$RESULTS/html" "$RESULTS"
	echo "HTML summary has been written to $RESULTS/html/index.html"
fi
