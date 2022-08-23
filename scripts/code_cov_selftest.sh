#!/bin/sh

set -e
trap 'catch $LINENO' EXIT
catch() {
	[ $? -eq 0 ] && exit
	echo "===> ERROR: Code coverage selftest failed on $0:$1" >&2
        exit 1
}

if [ -z "$IGT_KERNEL_TREE" ] ; then
        echo "Error! IGT_KERNEL_TREE environment var was not defined." >&2
        exit 1
fi

TEST="igt@debugfs_test@read_all_entries"

TESTLIST="my_tests.testlist"
GATHER="scripts/code_cov_gather_on_test.py"
LCOV_CAP="scripts/code_cov_capture.sh"
INFO_RESULTS="info_results"
TAR_RESULTS="tar_results"

sudo rm -rf results/ $INFO_RESULTS/ $TAR_RESULTS/ || true

echo "$TEST" > $TESTLIST

# run-tests.sh
echo "==> use lcov capture via run-tests.sh"
./scripts/run-tests.sh -T $TESTLIST -k $IGT_KERNEL_TREE -c $LCOV_CAP
echo "==> gather sysfs using run-tests.sh"
./scripts/run-tests.sh -T $TESTLIST -k $IGT_KERNEL_TREE -P -c $GATHER
echo "==> gather sysfs using run-tests.sh, capturing at the end"
./scripts/run-tests.sh -T $TESTLIST -k $IGT_KERNEL_TREE -c $GATHER

# igt_runner called directly
echo "==> use lcov capture via igt_runner"
sudo IGT_KERNEL_TREE=$IGT_KERNEL_TREE ./build/runner/igt_runner -o --test-list $TESTLIST --coverage-per-test --collect-script $LCOV_CAP  build/tests results
echo "==> gather sysfs running igt_runner"
sudo ./build/runner/igt_runner -o --test-list $TESTLIST --coverage-per-test --collect-script $GATHER build/tests results

# html report
echo "==> generate report from lcov info files"
scripts/code_cov_gen_report.sh -r results/code_cov/ -k $IGT_KERNEL_TREE -o $INFO_RESULTS -i --only-i915 --ignore-unused
echo "==> generate report from sysfs gather files"
scripts/code_cov_gen_report.sh -r results/code_cov/ -k $IGT_KERNEL_TREE -o $TAR_RESULTS -t --only-drm --ignore-unused

echo
echo "==> All tests passed. <=="
