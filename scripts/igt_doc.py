#!/usr/bin/env python3
# pylint: disable=C0301,R0914,R0912,R0915
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

## Copyright (C) 2023    Intel Corporation                 ##
## Author: Mauro Carvalho Chehab <mchehab@kernel.org>      ##
##                                                         ##
## Allow keeping inlined test documentation and validate   ##
## if the documentation is kept updated.                   ##

"""Maintain test plan and test implementation documentation on IGT."""

import argparse
import json
import re
import subprocess
import sys

IGT_BUILD_PATH = 'build/'
IGT_RUNNER = 'runner/igt_runner'

# Fields that mat be inside TEST and SUBTEST macros
fields = [
    'Category',       # Hardware building block / Software building block / ...
    'Sub-category',   # waitfence / dmabuf/ sysfs / debugfs / ...
    'Functionality',  # basic test / ...
    'Test category',  # functionality test / pereformance / stress
    'Run type',       # BAT / workarouds / stress / developer-specific / ...
    'Issue',          # Bug tracker issue(s)
    'GPU excluded platforms',     # none / DG1 / DG2 / TGL / MTL / PVC / ATS-M / ...
    'GPU requirements',  # Any other specific platform requirements
    'Depends on',     # some other IGT test, like igt@test@subtes
    'Test requirements',  # other non-platform requirements
    'Description']    # Description of the test

#
# TestList class definition
#
class TestList:

    """
    Parse and handle test lists with test/subtest documentation, in the
    form of C comments, with two meta-tags (TEST and SUBTEST), and a set of
    `field: value` items:

        /**
         * TEST: Check if new IGT test documentation logic functionality is working
         * Category: Software build block
         * Sub-category: documentation
         * Functionality: test documentation
         * Test category: ReST generation
         * Run type: IGT kunit test
         * Issue: none
         * GPU excluded platforms: none
         * GPU requirements: none
         * Depends on: @igt@deadbeef@basic
         * Test requirements: Need python3 to run it
         * Description: Complete description of this test
         *
         * SUBTEST: foo
         * Description: do foo things
         * 	description continuing on another line
         *
         * SUBTEST: bar
         * Description: do bar things
         * 	description continuing on another line
         */

        /**
         * SUBTEST: test-%s-binds-%s-with-%ld-size
         * Description: Test arg[2] arg[1] binds with arg[3] size
         *
         * SUBTEST: test-%s-%ld-size
         * Description: Test arg[1] with %arg[2] size
         *
         * arg[1]:
         *
         * @large:	large
         * 		something
         * @mixed:	mixed
         * 		something
         * @small:	small
         * 		something
         *
         * arg[2]:	buffer size
         * 		of some type
         *
         * arg[3]:
         *
         * @binds:			foo
         * @misaligned-binds:		misaligned
         * @userptr-binds:		userptr
         * @userptr-misaligned-binds:	userptr misaligned
         */

    The valid `field` values are defined on a JSON configuration file.

    The TEST documents the common fields present on all tests. Typically,
    each IGT C test file should contain just one such field.

    The SUBTEST contains the fields that are specific to each subtest.

    Some IGT tests may have strings or integer wildcards, like:
        test-%s-%ld-size

    Those are declared using igt_subtest_f() and similar macros.

    The wildcard arguments there need to be expanded. This is done by
    defining arg[1] to arg[n] at the same code comment that contains the
    SUBTEST as those variables are locally processed on each comment line.
    """

    def __init__(self):
        self.doc = {}
        self.test_number = 0
        self.min_test_prefix = ''

    #
    # ancillary methods
    #

    def expand_subtest(self, fname, test_name, test):

        """Expand subtest wildcards providing an array with subtests"""

        subtest_array = []

        for subtest in self.doc[test]["subtest"].keys():
            summary = test_name + '@' + self.doc[test]["subtest"][subtest]["Summary"]

            if not summary:
                continue

            num_vars = summary.count('%')

            # Handle trivial case: no wildcards
            if num_vars == 0:
                subtest_dict = {}

                subtest_dict["Summary"] = summary

                for k in sorted(self.doc[test]["subtest"][subtest].keys()):
                    if k == 'Summary':
                        continue
                    if k == 'arg':
                        continue

                    if self.doc[test]["subtest"][subtest][k] == self.doc[test][k]:
                        continue

                    subtest_dict[k] = self.doc[test]["subtest"][subtest][k]
                    subtest_array.append(subtest_dict)

                continue

            # Handle summaries with wildcards

            # Convert subtest arguments into an array
            arg_array = {}
            arg_ref = self.doc[test]["subtest"][subtest]["arg"]

            for arg_k in self.doc[test]["arg"][arg_ref].keys():
                arg_array[arg_k] = []
                if int(arg_k) > num_vars:
                    continue

                for arg_el in sorted(self.doc[test]["arg"][arg_ref][arg_k].keys()):
                    arg_array[arg_k].append(arg_el)

            size = len(arg_array)

            if size < num_vars:
                sys.exit(f"{fname}:subtest {summary} needs {num_vars} arguments but only {size} are defined.")

            for j in range(0, num_vars):
                if arg_array[j] is None:
                    sys.exit(f"{fname}:subtest{summary} needs arg[{j}], but this is not defined.")


            # convert numeric wildcards to string ones
            summary = re.sub(r'%(d|ld|lld|i|u|lu|llu)','%s', summary)

            # Expand subtests
            pos = [ 0 ] * num_vars
            args = [ 0 ] * num_vars
            arg_map = [ 0 ] * num_vars
            while 1:
                for j in range(0, num_vars):
                    arg_val = arg_array[j][pos[j]]
                    args[j] = arg_val

                    if arg_val in self.doc[test]["arg"][arg_ref][j]:
                        arg_map[j] = self.doc[test]["arg"][arg_ref][j][arg_val]
                        if re.match(r"\<.*\>", self.doc[test]["arg"][arg_ref][j][arg_val]):
                            args[j] = "<" + arg_val + ">"
                    else:
                        arg_map[j] = arg_val

                arg_summary = summary % tuple(args)

                # Store the element
                subtest_dict = {}
                subtest_dict["Summary"] = arg_summary

                for field in sorted(self.doc[test]["subtest"][subtest].keys()):
                    if field == 'Summary':
                        continue
                    if field == 'arg':
                        continue

                    sub_field = self.doc[test]["subtest"][subtest][field]
                    sub_field = re.sub(r"%?\barg\[(\d+)\]", lambda m: arg_map[int(m.group(1)) - 1], sub_field) # pylint: disable=W0640
                    if sub_field == self.doc[test][field]:
                        continue

                    subtest_dict[field] = sub_field

                subtest_array.append(subtest_dict)

                # Increment variable inside the array
                arr_pos = 0
                while pos[arr_pos] + 1 >= len(arg_array[arr_pos]):
                    pos[arr_pos] = 0
                    arr_pos += 1
                    if arr_pos >= num_vars:
                        break

                if arr_pos >= num_vars:
                    break

                pos[arr_pos] += 1

        return subtest_array

    def expand_dictionary(self):

        """ prepares a dictonary with subtest arguments expanded """

        test_dict = {}

        for test in self.doc:                   # pylint: disable=C0206
            fname = self.doc[test]["File"]

            name = re.sub(r'.*tests/', '', fname)
            name = re.sub(r'\.[ch]', '', name)
            name = "igt@" + name

            test_dict[name] = {}

            for field in self.doc[test]:
                if field == "subtest":
                    continue
                if field == "arg":
                    continue

                test_dict[name][field] = self.doc[test][field]

            subtest_array = self.expand_subtest(fname, name, test)
            for subtest in subtest_array:
                summary = subtest["Summary"]
                test_dict[name][summary] = {}
                for field in sorted(subtest.keys()):
                    if field == 'Summary':
                        continue
                    if field == 'arg':
                        continue
                    test_dict[name][summary][field] = subtest[field]

        return test_dict

    #
    # Output methods
    #

    def print_test(self):

        """Print tests and subtests"""

        for test in sorted(self.doc.keys()):
            fname = self.doc[test]["File"]

            name = re.sub(r'.*tests/', '', fname)
            name = re.sub(r'\.[ch]', '', name)
            name = "igt@" + name

            print(len(name) * '=')
            print(name)
            print(len(name) * '=')
            print()

            for field in sorted(self.doc[test].keys()):
                if field == "subtest":
                    continue
                if field == "arg":
                    continue

                print(f":{field}: {self.doc[test][field]}")

            subtest_array = self.expand_subtest(fname, name, test)

            for subtest in subtest_array:
                print()
                print(subtest["Summary"])
                print(len(subtest["Summary"]) * '=')
                print("")

                for field in sorted(subtest.keys()):
                    if field == 'Summary':
                        continue
                    if field == 'arg':
                        continue

                    print(f":{field}:", subtest[field])

                print()

            print()
            print()

    def print_json(self, out_fname):

        """Adds the contents of test/subtest documentation form a file"""
        test_dict = self.expand_dictionary()

        with open(out_fname, "w", encoding='utf8') as write_file:
            json.dump(test_dict, write_file, indent=4)

    #
    # Subtest list methods
    #

    def get_subtests(self):

        """Return an array with all subtests"""

        subtests = []

        for test in sorted(self.doc.keys()):
            fname = self.doc[test]["File"]

            test_name = re.sub(r'.*tests/', '', fname)
            test_name = re.sub(r'\.[ch]', '', test_name)
            test_name = "igt@" + test_name

            subtest_array = self.expand_subtest(fname, test_name, test)

            for subtest in subtest_array:
                subtests.append(subtest["Summary"])

        return subtests

    #
    # Validation methods
    #
    def check_tests(self):

        """Compare documented subtests with the IGT test list"""

        doc_subtests = sorted(self.get_subtests())

        for i in range(0, len(doc_subtests)): # pylint: disable=C0200
            doc_subtests[i] = re.sub(r'\<[^\>]+\>', r'\\d+', doc_subtests[i])

        # Get a list of tests from
        result = subprocess.run([ f"{IGT_BUILD_PATH}/{IGT_RUNNER}",  # pylint: disable=W1510
                                "-L", "-t",  self.min_test_prefix,
                                f"{IGT_BUILD_PATH}/tests"],
                                capture_output = True, text = True)
        if result.returncode:
            print( result.stdout)
            print("Error:", result.stderr)
            sys.exit(result.returncode)

        run_subtests = sorted(result.stdout.splitlines())

        # Compare arrays

        run_missing = []
        doc_uneeded = []

        for doc_test in doc_subtests:
            found = False
            for run_test in run_subtests:
                if re.match(r'^' + doc_test + r'$', run_test):
                    found = True
                    break
            if not found:
                doc_uneeded.append(doc_test)

        for run_test in run_subtests:
            found = False
            for doc_test in doc_subtests:
                if re.match(r'^' + doc_test + r'$', run_test):
                    found = True
                    break
            if not found:
                run_missing.append(run_test)

        if doc_uneeded:
            print("Unused documentation")
            for test_name in doc_uneeded:
                print(test_name)

        if run_missing:
            if doc_uneeded:
                print()
            print("Missing documentation")
            for test_name in run_missing:
                print(test_name)

    #
    # File handling methods
    #

    def add_file_documentation(self, fname):

        """Adds the contents of test/subtest documentation form a file"""

        current_test = None
        current_subtest = None

        handle_section = ''
        current_field = ''
        arg_number = 1
        cur_arg = -1
        cur_arg_element = 0

        prefix = re.sub(r'.*tests/', '', fname)
        prefix = r'igt\@' + re.sub(r'(.*/).*', r'\1', prefix)

        if self.min_test_prefix == '':
            self.min_test_prefix = prefix
        elif len(prefix) < len(self.min_test_prefix):
            self.min_test_prefix = prefix

        with open(fname, 'r', encoding='utf8') as handle:
            arg_ref = None
            current_test = ''
            subtest_number = 0

            for file_ln,file_line in enumerate(handle):
                if re.match(r'^\s*\*$', file_line):
                    continue

                if re.match(r'^\s*\*/$', file_line):
                    handle_section = ''
                    current_subtest = None
                    arg_ref = None
                    cur_arg = -1

                    continue

                if re.match(r'^\s*/\*\*$', file_line):
                    handle_section = '1'
                    continue

                if not handle_section:
                    continue

                file_line = re.sub(r'^\s*\*\s*', '', file_line)

                # Handle only known sections
                if handle_section == '1':
                    current_field = ''

                    # Check if it is a new TEST section
                    if (match := re.match(r'^TEST:\s*(.*)', file_line)):
                        current_test = self.test_number
                        self.test_number += 1

                        handle_section = 'test'

                        self.doc[current_test] = {}
                        self.doc[current_test]["arg"] = {}
                        self.doc[current_test]["Summary"] = match.group(1)
                        self.doc[current_test]["File"] = fname
                        self.doc[current_test]["subtest"] = {}
                        current_subtest = None

                        continue

                # Check if it is a new SUBTEST section
                if (match := re.match(r'^SUBTESTS?:\s*(.*)', file_line)):
                    current_subtest = subtest_number
                    subtest_number += 1

                    current_field = ''
                    handle_section = 'subtest'

                    self.doc[current_test]["subtest"][current_subtest] = {}

                    self.doc[current_test]["subtest"][current_subtest]["Summary"] = match.group(1)
                    self.doc[current_test]["subtest"][current_subtest]["Description"] = ''

                    if not arg_ref:
                        arg_ref = arg_number
                        arg_number += 1
                        self.doc[current_test]["arg"][arg_ref] = {}

                    self.doc[current_test]["subtest"][current_subtest]["arg"] = arg_ref

                    continue

                # It is a known section. Parse its contents
                if (match := re.match(field_re, file_line)):
                    current_field = match.group(1).lower().capitalize()
                    match_val = match.group(2)

                    if handle_section == 'test':
                        self.doc[current_test][current_field] = match_val
                    else:
                        self.doc[current_test]["subtest"][current_subtest][current_field] = match_val

                    cur_arg = -1

                    continue

                # Use hashes for arguments to avoid duplication
                if (match := re.match(r'arg\[(\d+)\]:\s*(.*)', file_line)):
                    current_field = ''
                    if arg_ref is None:
                        sys.exit(f"{fname}:{file_ln + 1}: arguments should be defined after one or more subtests, at the same comment")

                    cur_arg = int(match.group(1)) - 1
                    if cur_arg not in self.doc[current_test]["arg"][arg_ref]:
                        self.doc[current_test]["arg"][arg_ref][cur_arg] = {}

                    cur_arg_element = match.group(2)

                    if match.group(2):
                        # Should be used only for numeric values
                        self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] = "<" + match.group(2) + ">"

                    continue

                if (match := re.match(r'\@(\S+):\s*(.*)', file_line)):
                    if cur_arg >= 0:
                        current_field = ''
                        if arg_ref is None:
                            sys.exit(f"{fname}:{file_ln + 1}: arguments should be defined after one or more subtests, at the same comment")

                        cur_arg_element = match.group(1)
                        self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] = match.group(2)

                    else:
                        print(f"{fname}:{file_ln + 1}: Warning: invalid argument: @%s: %s" %
                            (match.group(1), match.group(2)))

                    continue

                if (match := re.match(r'^(.*):', file_line)):
                    sys.exit(f"{fname}:{file_ln + 1}: Error: unrecognized field '%s'. Need to add at %s" %
                            (match.group(1), fname))

                # Handle multi-line field contents
                if current_field:
                    if (match := re.match(r'\s*(.*)', file_line)):
                        if handle_section == 'test':
                            self.doc[current_test][current_field] += " " + \
                                match.group(1)
                        else:
                            self.doc[current_test]["subtest"][current_subtest][current_field] += " " + \
                                match.group(1)

                    continue

                # Handle multi-line argument contents
                if cur_arg >= 0 and arg_ref is not None:
                    if (match := re.match(r'\s*\*?\s*(.*)', file_line)):
                        match_val = match.group(1)

                        match = re.match(r'^(\<.*)\>$',self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element])
                        if match:
                            self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] = match.group(1) + ' ' + match_val + ">"
                        else:
                            self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] += ' ' + match_val

                    continue

#
# Main
#

parser = argparse.ArgumentParser(description = "Print formatted kernel documentation to stdout.",
                                 formatter_class = argparse.ArgumentDefaultsHelpFormatter,
                                 epilog = 'If no action specified, assume --rest.')
parser.add_argument("--rest", action="store_true",
                    help="Generate documentation from the source files, in ReST file format.")
parser.add_argument("--to-json",
                    help="Output test documentation in JSON format as TO_JSON file")
parser.add_argument("--show-subtests", action="store_true",
                    help="Shows the name of the documented subtests in alphabetical order.")
parser.add_argument("--check-testlist", action="store_true",
                    help="Compare documentation against IGT runner testlist.")
parser.add_argument("--igt-build-path",
                    help="Path where the IGT runner is sitting. Used by --check-testlist.",
                    default=IGT_BUILD_PATH)
parser.add_argument('--files', nargs='+', required=True,
                    help="File name(s) to be processed")

parse_args = parser.parse_args()

field_regex = re.compile(r"(" + '|'.join(fields) + r'):\s*(.*)', re.I)

tests = TestList()

for filename in parse_args.files:
    tests.add_file_documentation(filename, field_regex)

RUN = 0
if parse_args.show_subtests:
    RUN = 1
    for sub in tests.get_subtests():
        print (sub)

if parse_args.check_testlist:
    RUN = 1
    tests.check_tests()

if parse_args.to_json:
    RUN = 1
    tests.print_json(parse_args.to_json)

if not RUN or parse_args.rest:
    tests.print_test()
