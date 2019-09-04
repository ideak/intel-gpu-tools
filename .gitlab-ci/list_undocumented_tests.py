#!/usr/bin/env python3

import re
import sys
import os.path
import subprocess

from collections import namedtuple

Subtest = namedtuple("Subtest", "name description")

def get_testlist(path):
    "read binaries' names from test-list.txt"
    with open(path, 'r') as f:
        assert(f.readline() == "TESTLIST\n")
        tests = f.readline().strip().split(" ")
        assert(f.readline() == "END TESTLIST\n")

    return tests

def get_subtests(testdir, test):
    "execute test and get subtests with their descriptions via --describe"
    output = []
    full_test_path = os.path.join(testdir, test)
    proc = subprocess.run([full_test_path, "--describe"], stdout=subprocess.PIPE)
    description = ""
    current_subtest = None

    for line in proc.stdout.decode().splitlines():
        if line.startswith("SUB "):
            output += [Subtest(current_subtest, description)]
            description = ""
            current_subtest = line.split(' ')[1]
        else:
            description += line

    output += [Subtest(current_subtest, description)]

    return output

def main():
    testlist_file = sys.argv[1]
    testdir       = os.path.abspath(os.path.dirname(testlist_file))

    tests = get_testlist(testlist_file)

    for test in tests:
        subtests = get_subtests(testdir, test)

        if subtests and subtests[0].name:
            # no top level description
            print(test)

        for name, description in subtests:
            if not name:
                continue

            if "NO DOCUMENTATION!" in description:
                print("{}@{}".format(test, name))

main()
