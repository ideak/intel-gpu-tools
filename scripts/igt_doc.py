#!/usr/bin/env python3
# pylint: disable=C0301
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

## Copyright (C) 2023    Intel Corporation                 ##
## Author: Mauro Carvalho Chehab <mchehab@kernel.org>      ##
##                                                         ##
## Allow keeping inlined test documentation and validate   ##
## if the documentation is kept updated.                   ##

"""Maintain test plan and test implementation documentation on IGT."""

import argparse
import sys

from test_list import TestList

IGT_BUILD_PATH = 'build'
IGT_RUNNER = 'runner/igt_runner'

parser = argparse.ArgumentParser(description = "Print formatted kernel documentation to stdout.",
                                 formatter_class = argparse.ArgumentDefaultsHelpFormatter,
                                 epilog = 'If no action specified, assume --rest.')
parser.add_argument("--config", required = True,
                    help="JSON file describing the test plan template")
parser.add_argument("--rest",
                    help="Output documentation from the source files in REST file.")
parser.add_argument("--per-test", action="store_true",
                    help="Modifies ReST output to print subtests per test.")
parser.add_argument("--to-json",
                    help="Output test documentation in JSON format as TO_JSON file")
parser.add_argument("--show-subtests", action="store_true",
                    help="Shows the name of the documented subtests in alphabetical order.")
parser.add_argument("--sort-field",
                    help="modify --show-subtests to sort output based on SORT_FIELD value")
parser.add_argument("--filter-field",
                    help="modify --show-subtests to filter output based a regex given by FILTER_FIELD=~'regex'")
parser.add_argument("--check-testlist", action="store_true",
                    help="Compare documentation against IGT runner testlist.")
parser.add_argument("--include-plan", action="store_true",
                    help="Include test plans, if any.")
parser.add_argument("--igt-build-path",
                    help="Path where the IGT runner is sitting. Used by --check-testlist.",
                    default=IGT_BUILD_PATH)
parser.add_argument("--gen-testlist",
                    help="Generate documentation at the GEN_TESTLIST directory, using SORT_FIELD to split the tests. Requires --sort-field.")
parser.add_argument("--igt-runner",
                    help="Path where the IGT runner is sitting. Used by --check-testlist.",
                    default=IGT_RUNNER)
parser.add_argument('--files', nargs='+',
                    help="File name(s) to be processed")

parse_args = parser.parse_args()

tests = TestList(parse_args.config, parse_args.include_plan, parse_args.files,
                 parse_args.igt_build_path, parse_args.igt_runner)

RUN = 0
if parse_args.show_subtests:
    RUN = 1
    tests.show_subtests(parse_args.sort_field, parse_args.filter_field)

if parse_args.check_testlist:
    RUN = 1
    tests.check_tests()

if parse_args.gen_testlist:
    RUN = 1
    if not parse_args.sort_field:
        sys.exit("Need a field to split the testlists")
    tests.gen_testlist(parse_args.gen_testlist, parse_args.sort_field, parse_args.filter_field)

if parse_args.to_json:
    RUN = 1
    tests.print_json(parse_args.to_json)

if not RUN or parse_args.rest:
    if parse_args.per_test:
        tests.print_rest_flat(parse_args.rest)
    else:
        tests.print_nested_rest(parse_args.rest)
