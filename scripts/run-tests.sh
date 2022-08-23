#!/bin/sh
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
IGT_KERNEL_TREE=
COV_ARGS=
COV_PER_TEST=
LCOV_CMD="lcov"
KERNEL_TREE=
USE_PIGLIT=0
RUNNER=
RESUME=

find_file() # basename <possible paths>
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

find_runner_binary() # basename
{
	base=$1
	shift

	binary=$(find_file "$base" "$ROOT/build/runner" "$ROOT/runner")
	if [ -x "$binary" ]; then
		echo "$binary"
		return 0
	elif binary=$(which "$base"); then
		echo "$binary"
		return 0
	fi

	return 1
}

find_lcov_binary() # basename
{
	if command -v $LCOV_CMD > /dev/null 2>&1; then
		return 0
	fi

	return 1
}

download_piglit() {
	git clone https://anongit.freedesktop.org/git/piglit.git "$ROOT/piglit"
}

execute_runner() # as-root <runner> <args>
{
	need_root=$1
	shift
	runner=$1
	shift

	export IGT_TEST_ROOT IGT_CONFIG_PATH IGT_KERNEL_TREE

	if [ "$need_root" -ne 0 -a "$(id -u)" -ne 0 ]; then
		if command -v sudo > /dev/null 2>&1; then
			sudo="sudo --preserve-env=IGT_TEST_ROOT,IGT_CONFIG_PATH,IGT_KERNEL_TREE"
		else
			echo "$0: Could not start runner: Permission denied."
			exit 1
		fi
	fi

	$sudo $runner "$@"
}

print_help() {
	echo "Usage: run-tests.sh [options]"
	echo "Available options:"
	echo "  -c <capture_script>"
	echo "                  capture gcov code coverage using the <capture_script>."
	echo "  -P              store code coverage results per each test. Should be"
	echo "                  used together with -k option"
	echo "  -d              download Piglit to $ROOT/piglit"
	echo "  -h              display this help message"
	echo "  -k <kernel_dir> Linux Kernel source code directory used to generate code"
	echo "                  coverage builds."
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

while getopts ":c:dhk:lPr:st:T:vx:Rnpb:" opt; do
	case $opt in
		c) COV_ARGS="$COV_ARGS --collect-code-cov --collect-script $OPTARG " ;;
		d) download_piglit; exit ;;
		h) print_help; exit ;;
		k) IGT_KERNEL_TREE="$OPTARG" ;;
		l) LIST_TESTS="true" ;;
		P) COV_ARGS="$COV_ARGS --coverage-per-test"; COV_PER_TEST=1 ;;
		r) RESULTS="$OPTARG" ;;
		s) SUMMARY="html" ;;
		t) FILTER="$FILTER -t $OPTARG" ;;
		T) FILTER="$FILTER --test-list $OPTARG" ;;
		v) VERBOSE="-l verbose" ;;
		x) FILTER="$FILTER -x $OPTARG" ;;
		R) RESUME_RUN="true" ;;
		n) NORETRY="--no-retry" ;;
		p) USE_PIGLIT=1 ;;
		b) FILTER="$FILTER -b $OPTARG" ;;
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

if [ "x$PIGLIT" = "x" ]; then
	PIGLIT="$ROOT/piglit/piglit"
fi

if [ "x$COV_ARGS" != "x" ]; then
	if [ "$USE_PIGLIT" -eq "1" ]; then
		echo "Cannot collect code coverage when running tests with Piglit. Use igt_runner instead."
		exit 1
	fi

	if ! $(find_lcov_binary); then
		echo "Can't check code coverage, as 'lcov' is not installed"
		exit 1
	fi
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
	if ! IGT_RUNNER=$(find_runner_binary igt_runner) ||
	   ! IGT_RESUME=$(find_runner_binary igt_resume); then
		echo "Could not find igt_runner binaries."
		echo "Please build the runner, or use Piglit with the -p flag."
		exit 1
	fi

	RUNNER=$IGT_RUNNER
	RESUME=$IGT_RESUME
	LIST_ARGS="-L"
fi

if [ "x$LIST_TESTS" != "x" ]; then
	execute_runner 0 $RUNNER $LIST_ARGS $FILTER $COV_ARGS
	exit
fi

if [ "x$RESUME_RUN" != "x" ]; then
	if [ "x$COV_ARGS" != "x" -a "x$COV_PER_TEST" = "x" ]; then
		echo "Can't continue collecting coverage tests. Next time, run"
		echo "$0 with '-P' in order to generate separate code coverage results".
		exit 1
	fi
	execute_runner 1 $RESUME $RESUME_ARGS $COV_ARGS "$RESULTS"
else
	mkdir -p "$RESULTS"
	execute_runner 1 $RUNNER $RUN_ARGS -o -s "$RESULTS" $COV_ARGS $VERBOSE $FILTER
fi

if [ "$SUMMARY" = "html" ]; then
	if [ ! -x "$PIGLIT" ]; then
		echo "Could not find Piglit, required for HTML generation."
		echo "Please install Piglit or use -d to download Piglit locally."
		exit 1
	fi

	execute_runner 0 $PIGLIT summary html --overwrite "$RESULTS/html" "$RESULTS"
	echo "HTML summary has been written to $RESULTS/html/index.html"
fi
