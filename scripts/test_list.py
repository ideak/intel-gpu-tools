#!/usr/bin/env python3
# pylint: disable=C0301,R0902,R0914,R0912,R0913,R0915,R1702,C0302
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

## Copyright (C) 2023    Intel Corporation                 ##
## Author: Mauro Carvalho Chehab <mchehab@kernel.org>      ##
##                                                         ##
## Allow keeping inlined test documentation and validate   ##
## if the documentation is kept updated.                   ##

"""Maintain test plan and test implementation documentation on IGT."""

import glob
import json
import os
import re
import subprocess
import sys

MIN_PYTHON = (3, 6)
if sys.version_info < MIN_PYTHON:
    sys.exit("Python %s.%s or later is required.\n" % MIN_PYTHON) # pylint: disable=C0209

#
# ancillary functions to sort dictionary hierarchy
#
def _sort_per_level(item):
    if "level" not in item[1]["_properties_"]:
        return item[0]

    return "%05d_%05d_%s" % (item[1]["_properties_"]["level"], item[1]["_properties_"]["sublevel"], item[0])   # pylint: disable=C0209

def _sort_using_array(item, array):
    ret_str = ''
    for field in array:
        if field in item[1]:
            ret_str += '_' + field + '_' + item[1][field]

    if ret_str == '':
        ret_str="________"

    return ret_str

#
# Ancillary logic to allow plurals on fields
#
# As suggested at https://stackoverflow.com/questions/18902608/generating-the-plural-form-of-a-noun/19018986
#

def _plural(field):

    """
    Poor man's conversion to plural.

    It should cover usual English plural rules, although it is not meant
    to cover exceptions (except for a few ones that could be useful on actual
    fields).

    """

    match = re.match(r"(.*)\b(\S+)", field)
    if match:
        ret_str = match.group(1)
        word = match.group(2)

        if word.isupper():
            ret_str += word
        elif word in ["of", "off", "on", "description", "todo"]:
            ret_str += word
        elif word.endswith('ed'):
            ret_str += word
        elif word[-1:] in ['s', 'x', 'z']:
            ret_str += word + 'es'
        elif word[-2:] in ['sh', 'ch']:
            ret_str += word + 'es'
        elif word.endswith('fe'):
            ret_str += word[:-2] + 'ves'
        elif word.endswith('f'):
            ret_str += word[:-1] + 'ves'
        elif word.endswith('y'):
            ret_str += word[:-1] + 'ies'
        elif word.endswith('o'):
            ret_str += word + 'es'
        elif word.endswith('us'):
            ret_str += word[:-2] + 'i'
        elif word.endswith('on'):
            ret_str += word[:-2] + 'a'
        elif word.endswith('an'):
            ret_str += word[:-2] + 'en'
        else:
            ret_str += word + 's'

        return ret_str

    return field

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
         * Issue: none
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

    It is also possible to define a list of values that will actually be
    used by the test and no string replacement is needed.
    This is done by using one or multiple arg[n].values lines:

        /**
         * SUBTEST: unbind-all-%d-vmas
         * Description: unbind all with %arg[1] VMAs
         *
         * arg[1].values: 2, 8
         * arg[1].values: 16, 32
         */

        /**
         * SUBTEST: unbind-all-%d-vmas
         * Description: unbind all with %arg[1] VMAs
         *
         * arg[1].values: 64, 128
         */

    Such feature is specially useful for numeric parameters

    The valid `field` values are defined on a JSON configuration file.

    The TEST documents the common fields present on all tests. Typically,
    each IGT C test file should contain just one such field.

    The SUBTEST contains the fields that are specific to each subtest.

    Note: when igt_simple_main is used, there will be a single nameless
    subtest. On such case, "SUBTEST:" is still needed, but without a
    test name on it, e. g., it would be documented as:

        /**
         * TEST: some test that uses igt_simple_main
         * Category: Software build block
         * Sub-category: documentation
         * Functionality: test documentation
         * Issue: none
         * Description: Complete description of this test
         *
         * SUBTEST:
         * Description: do foo things
         */

    Some IGT tests may have strings or integer wildcards, like:
        test-%s-%ld-size

    Those are declared using igt_subtest_f() and similar macros.

    The wildcard arguments there need to be expanded. This is done by
    defining arg[1] to arg[n] at the same code comment that contains the
    SUBTEST as those variables are locally processed on each comment line.

    This script needs a configuration file, in JSON format, describing the
    fields which will be parsed for TEST and/or SUBTEST tags.

    An example of such file is:

    {
        "files": [ "tests/driver/*.c" ],
        "fields": {
            "Category": {
                "Sub-category": {
                    "Functionality": {
                    }
                }
            },
            "Issue": {
                "_properties_": {
                    "description": "If the test is used to solve an issue, point to the URL containing the issue."
                }
            },
            "Description" : {
                "_properties_": {
                    "description": "Provides a description for the test/subtest."
                }
            }
        }
    }

    So, the above JSON config file expects tags like those:

    TEST: foo
    Description: foo

    SUBTEST: bar
    Category: Hardware
    Sub-category: EU
    Description: test bar on EU

    SUBTEST: foobar
    Category: Software
    Type: ioctl
    Description: test ioctls
    """

    def __init__(self, config_fname, include_plan = False, file_list = False,
                 igt_build_path = None):
        self.doc = {}
        self.test_number = 0
        self.config = None
        self.filenames = file_list
        self.plan_filenames = []
        self.props = {}
        self.config_fname = config_fname
        self.igt_build_path = igt_build_path
        self.level_count = 0
        self.field_list = {}
        self.title = None
        self.filters = {}

        driver_name = re.sub(r'(.*/)?([^\/]+)/.*', r'\2', config_fname).capitalize()

        implemented_class = None

        with open(config_fname, 'r', encoding='utf8') as handle:
            self.config = json.load(handle)

            self.__add_field(None, 0, 0, self.config["fields"])

            sublevel_count = [ 0 ] * self.level_count

            for field, item in self.props.items():
                if "sublevel" in item["_properties_"]:
                    level = item["_properties_"]["level"]
                    sublevel = item["_properties_"]["sublevel"]
                    if sublevel > sublevel_count[level - 1]:
                        sublevel_count[level - 1] = sublevel

                field_lc = field.lower()
                self.field_list[field_lc] = field
                field_plural = _plural(field_lc)
                if field_lc != field_plural:
                    self.field_list[field_plural] = field

            if include_plan:
                self.props["Class"] = {}
                self.props["Class"]["_properties_"] = {}
                self.props["Class"]["_properties_"]["level"] = 1
                self.props["Class"]["_properties_"]["sublevel"] = sublevel_count[0] + 1

            # Remove non-multilevel items, as we're only interested on
            # hierarchical item levels here
            for field, item in self.props.items():
                if "sublevel" in item["_properties_"]:
                    level = item["_properties_"]["level"]
                    if sublevel_count[level - 1] == 1:
                        del item["_properties_"]["level"]
                        del item["_properties_"]["sublevel"]
            del self.props["_properties_"]

            has_implemented = False
            if not self.filenames:
                self.filenames = []
                files = self.config["files"]
                for cfg_file in files:
                    cfg_file = os.path.realpath(os.path.dirname(config_fname)) + "/" + cfg_file
                    for fname in glob.glob(cfg_file):
                        self.filenames.append(fname)
                        has_implemented = True
            else:
                for cfg_file in self.filenames:
                    if cfg_file:
                        has_implemented = True

            has_planned = False
            if include_plan and "planning_files" in self.config:
                implemented_class = "Implemented"
                files = self.config["planning_files"]
                for cfg_file in files:
                    cfg_file = os.path.realpath(os.path.dirname(config_fname)) + "/" + cfg_file
                    for fname in glob.glob(cfg_file):
                        self.plan_filenames.append(fname)
                        has_planned = True

        planned_class = None
        if has_implemented:
            if has_planned:
                planned_class = "Planned"
                self.title = "Planned and implemented tests for "
            else:
                self.title = "Implemented tests for "
        else:
            if has_planned:
                self.title = "Planned tests for "
            else:
                sys.exit("Need file names to be processed")

        self.title += driver_name + " driver"

        # Parse files, expanding wildcards
        field_re = re.compile(r"(" + '|'.join(self.field_list.keys()) + r'):\s*(.*)', re.I)

        for fname in self.filenames:
            if fname == '':
                continue

            self.__add_file_documentation(fname, implemented_class, field_re)

        if include_plan:
            for fname in self.plan_filenames:
                self.__add_file_documentation(fname, planned_class, field_re)

    #
    # ancillary methods
    #

    def __add_field(self, name, sublevel, hierarchy_level, field):

        """ Flatten config fields into a non-hierarchical dictionary """

        for key in field:
            if key not in self.props:
                self.props[key] = {}
                self.props[key]["_properties_"] = {}

            if name:
                if key == "_properties_":
                    if key not in self.props:
                        self.props[key] = {}
                    self.props[name][key].update(field[key])

                    sublevel += 1
                    hierarchy_level += 1
                    if "sublevel" in self.props[name][key]:
                        if self.props[name][key]["sublevel"] != sublevel:
                            sys.exit(f"Error: config defined {name} as sublevel {self.props[key]['sublevel']}, but wants to redefine as sublevel {sublevel}")

                    self.props[name][key]["level"] = self.level_count
                    self.props[name][key]["sublevel"] = sublevel

                    continue
            else:
                self.level_count += 1

            self.__add_field(key, sublevel, hierarchy_level, field[key])

    def __filter_subtest(self, test, subtest, field_not_found_value):

        """ Apply filter criteria to subtests """

        for filter_field, regex in self.filters.items():
            if filter_field in subtest:
                if not re.match(regex, subtest[filter_field]):
                    return True
            elif filter_field in test:
                if not re.match(regex, test[filter_field]):
                    return True
            else:
                return field_not_found_value

        # None of the filtering rules were applied
        return False

    def expand_subtest(self, fname, test_name, test, allow_inherit):

        """Expand subtest wildcards providing an array with subtests"""

        subtest_array = []

        for subtest in self.doc[test]["subtest"].keys():
            summary = test_name
            if self.doc[test]["subtest"][subtest]["Summary"] != '':
                summary += '@' + self.doc[test]["subtest"][subtest]["Summary"]
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

                    if not allow_inherit:
                        if k in self.doc[test] and self.doc[test]["subtest"][subtest][k] == self.doc[test][k]:
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

                    if not allow_inherit:
                        if field in self.doc[test]:
                            if sub_field in self.doc[test][field] and sub_field == self.doc[test][field]:
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

    def expand_dictionary(self, subtest_only):

        """ prepares a dictionary with subtest arguments expanded """

        test_dict = {}

        for test in self.doc:                   # pylint: disable=C0206
            fname = self.doc[test]["File"]

            name = re.sub(r'.*/', '', fname)
            name = re.sub(r'\.[\w+]$', '', name)
            name = "igt@" + name

            if not subtest_only:
                test_dict[name] = {}

                for field in self.doc[test]:
                    if field == "subtest":
                        continue
                    if field == "arg":
                        continue

                    test_dict[name][field] = self.doc[test][field]
                dic = test_dict[name]
            else:
                dic = test_dict

            subtest_array = self.expand_subtest(fname, name, test, subtest_only)
            for subtest in subtest_array:
                if self.__filter_subtest(test, subtest, True):
                    continue

                summary = subtest["Summary"]

                dic[summary] = {}
                for field in sorted(subtest.keys()):
                    if field == 'Summary':
                        continue
                    if field == 'arg':
                        continue
                    dic[summary][field] = subtest[field]

        return test_dict

    #
    # Output methods
    #

    def print_rest_flat(self, filename):

        """Print tests and subtests ordered by tests"""

        handler = None
        if filename:
            original_stdout = sys.stdout
            handler = open(filename, "w", encoding='utf8') # pylint: disable=R1732
            sys.stdout = handler

        print("=" * len(self.title))
        print(self.title)
        print("=" * len(self.title))
        print()

        for test in sorted(self.doc.keys()):
            fname = self.doc[test]["File"]

            name = re.sub(r'.*/', '', fname)
            name = re.sub(r'\.[ch]', '', name)
            name = "igt@" + name

            tmp_subtest = self.expand_subtest(fname, name, test, False)

            # Get subtests first, to avoid displaying tests with
            # all filtered subtests
            subtest_array = []
            for subtest in tmp_subtest:
                if self.__filter_subtest(self.doc[test], subtest, True):
                    continue
                subtest_array.append(subtest)

            if not subtest_array:
                continue

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

        if handler:
            handler.close()
            sys.stdout = original_stdout

    def print_nested_rest(self, filename):

        """Print tests and subtests ordered by tests"""

        handler = None
        if filename:
            original_stdout = sys.stdout
            handler = open(filename, "w", encoding='utf8') # pylint: disable=R1732
            sys.stdout = handler

        print("=" * len(self.title))
        print(self.title)
        print("=" * len(self.title))
        print()

        # Identify the sort order for the fields
        fields_order = []
        fields = sorted(self.props.items(), key = _sort_per_level)
        for item in fields:
            fields_order.append(item[0])

        # Receives a flat subtest dictionary, with wildcards expanded
        subtest_dict = self.expand_dictionary(True)

        subtests = sorted(subtest_dict.items(),
                          key = lambda x: _sort_using_array(x, fields_order))

        # Use the level markers below
        level_markers='=-^_~:.`"*+#'

        # Print the data
        old_fields = [ '' ] * len(fields_order)

        for subtest, fields in subtests:
            # Check what level has different message
            marker = 0
            for cur_level in range(0, len(fields_order)):  # pylint: disable=C0200
                field = fields_order[cur_level]
                if not "level" in self.props[field]["_properties_"]:
                    continue
                if field in fields:
                    if old_fields[cur_level] != fields[field]:
                        break
                    marker += 1

            # print hierarchy
            for i in range(cur_level, len(fields_order)):
                if not "level" in self.props[fields_order[i]]["_properties_"]:
                    continue
                if not fields_order[i] in fields:
                    continue

                if marker >= len(level_markers):
                    sys.exit(f"Too many levels: {marker}, maximum limit is {len(level_markers):}")

                title_str = fields_order[i] + ": " + fields[fields_order[i]]

                print(title_str)
                print(level_markers[marker] * len(title_str))
                print()
                marker += 1

            print()
            print("``" + subtest + "``")
            print()

            # print non-hierarchy fields
            for field in fields_order:
                if "level" in self.props[field]["_properties_"]:
                    continue

                if field in fields:
                    print(f":{field}: {fields[field]}")

            # Store current values
            for i in range(cur_level, len(fields_order)):
                field = fields_order[i]
                if not "level" in self.props[field]["_properties_"]:
                    continue
                if field in fields:
                    old_fields[i] = fields[field]
                else:
                    old_fields[i] = ''

            print()

        if handler:
            handler.close()
            sys.stdout = original_stdout

    def print_json(self, out_fname):

        """Adds the contents of test/subtest documentation form a file"""

        # Receives a dictionary with tests->subtests with expanded subtests
        test_dict = self.expand_dictionary(False)

        with open(out_fname, "w", encoding='utf8') as write_file:
            json.dump(test_dict, write_file, indent = 4)

    #
    # Add filters
    #
    def add_filter(self, filter_field_expr):

        """ Add a filter criteria for output data """

        match = re.match(r"(.*)=~\s*(.*)", filter_field_expr)
        if not match:
            sys.exit(f"Filter field {filter_field_expr} is not at <field> =~ <regex> syntax")
        field = match.group(1).strip().lower()
        if field not in self.field_list:
            sys.exit(f"Field '{field}' is not defined")
        filter_field = self.field_list[field]
        regex = re.compile("{0}".format(match.group(2).strip()), re.I) # pylint: disable=C0209
        self.filters[filter_field] = regex

    #
    # Subtest list methods
    #

    def get_subtests(self, sort_field = None):

        """Return an array with all subtests"""

        subtests = {}
        subtests[""] = []

        if sort_field:
            if sort_field.lower() not in self.field_list:
                sys.exit(f"Field '{sort_field}' is not defined")
            sort_field = self.field_list[sort_field.lower()]

        for test in sorted(self.doc.keys()):
            fname = self.doc[test]["File"]

            test_name = re.sub(r'.*/', '', fname)
            test_name = re.sub(r'\.[ch]', '', test_name)
            test_name = "igt@" + test_name

            subtest_array = self.expand_subtest(fname, test_name, test, True)

            for subtest in subtest_array:
                if self.__filter_subtest(test, subtest, True):
                    continue

                if sort_field:
                    if sort_field in subtest:
                        if subtest[sort_field] not in subtests:
                            subtests[subtest[sort_field]] = []

                        subtests[subtest[sort_field]].append(subtest["Summary"])
                    else:
                        subtests[""].append(subtest["Summary"])

                else:
                    subtests[""].append(subtest["Summary"])

        return subtests

    def __get_testlist(self, name):
        match = re.match(r"(.*/)?(.*)\.c$", name)
        if not match:
            return []

        basename = "igt@" + match.group(2)

        fname = os.path.join(self.igt_build_path, "tests", match.group(2))
        if not os.path.isfile(fname):
            print(f"Error: file {fname} doesn't exist.")
            sys.exit(1)
        try:
            result = subprocess.run([ fname, "--list-subtests" ],
                                    check = True,
                                    stdout = subprocess.PIPE,
                                    universal_newlines=True)
            subtests = result.stdout.splitlines()

            return [basename  + "@" + i for i in subtests]
        except subprocess.CalledProcessError:
            # Handle it as a test using igt_simple_main
            return [basename]

    def get_testlist(self):

        """ Return a list of tests as reported by --list-subtests """
        tests = []
        for name in self.filenames:
            tests += self.__get_testlist(name)

        return sorted(tests)

    #
    # Validation methods
    #
    def check_tests(self):

        """Compare documented subtests with the IGT test list"""

        if not self.igt_build_path:
            sys.exit("Need the IGT build path")

        if self.filters:
            print("NOTE: test checks are affected by filters")

        doc_subtests = sorted(self.get_subtests()[""])

        for i in range(0, len(doc_subtests)): # pylint: disable=C0200
            doc_subtests[i] = re.sub(r'\<[^\>]+\>', r'\\d+', doc_subtests[i])

        # Get a list of tests from
        run_subtests = self.get_testlist()

        # Compare arrays

        run_missing = []
        doc_uneeded = []

        test_regex = r""
        for doc_test in doc_subtests:
            if test_regex != r"":
                test_regex += r"|"
            test_regex += r'^' + doc_test + r'$'

        test_regex = re.compile(test_regex)

        for doc_test in doc_subtests:
            found = False
            for run_test in run_subtests:
                if re.match(test_regex, run_test):
                    found = True
                    break
            if not found:
                doc_uneeded.append(doc_test)

        for run_test in run_subtests:
            found = False
            if re.match(test_regex, run_test):
                found = True
            if not found:
                run_missing.append(run_test)

        if doc_uneeded:
            for test_name in doc_uneeded:
                print(f"Warning: Documented {test_name} doesn't exist on source files")

        if run_missing:
            for test_name in run_missing:
                print(f'Warning: Missing documentation for {test_name}')
        if doc_uneeded or run_missing:
            sys.exit(1)

    #
    # File handling methods
    #

    def __add_file_documentation(self, fname, implemented_class, field_re):

        """Adds the contents of test/subtest documentation form a file"""

        current_test = None
        current_subtest = None

        handle_section = ''
        current_field = ''
        arg_number = 1
        cur_arg = -1
        cur_arg_element = 0
        has_test_or_subtest = 0

        with open(fname, 'r', encoding='utf8') as handle:
            arg_ref = None
            current_test = ''
            subtest_number = 0

            for file_ln,file_line in enumerate(handle):
                file_line = file_line.rstrip()

                if re.match(r'^\s*\*$', file_line):
                    continue

                if re.match(r'^\s*\*/$', file_line):
                    handle_section = ''
                    current_subtest = None
                    arg_ref = None
                    cur_arg = -1
                    has_test_or_subtest = 0

                    continue

                if re.match(r'^\s*/\*\*$', file_line):
                    handle_section = '1'
                    continue

                if not handle_section:
                    continue

                file_line = re.sub(r'^\s*\* ?', '', file_line)

                # Handle only known sections
                if handle_section == '1':
                    current_field = ''

                    # Check if it is a new TEST section
                    match = re.match(r'^TEST:\s*(.*)', file_line)
                    if match:
                        has_test_or_subtest = 1
                        current_test = self.test_number
                        self.test_number += 1

                        handle_section = 'test'

                        self.doc[current_test] = {}
                        self.doc[current_test]["arg"] = {}
                        self.doc[current_test]["Summary"] = match.group(1)
                        self.doc[current_test]["File"] = fname
                        self.doc[current_test]["subtest"] = {}

                        if implemented_class:
                            self.doc[current_test]["Class"] = implemented_class
                        current_subtest = None

                        continue

                # Check if it is a new SUBTEST section
                match = re.match(r'^SUBTESTS?:\s*(.*)', file_line)
                if match:
                    has_test_or_subtest = 1
                    current_subtest = subtest_number
                    subtest_number += 1

                    current_field = ''
                    handle_section = 'subtest'

                    # subtests inherit properties from the tests
                    self.doc[current_test]["subtest"][current_subtest] = {}
                    for field in self.doc[current_test].keys():
                        if field == "arg":
                            continue
                        if field == "summary":
                            continue
                        if field == "File":
                            continue
                        if field == "subtest":
                            continue
                        if field == "_properties_":
                            continue
                        if field == "Description":
                            continue
                        self.doc[current_test]["subtest"][current_subtest][field] = self.doc[current_test][field]

                    self.doc[current_test]["subtest"][current_subtest]["Summary"] = match.group(1)
                    self.doc[current_test]["subtest"][current_subtest]["Description"] = ''

                    if not arg_ref:
                        arg_ref = arg_number
                        arg_number += 1
                        self.doc[current_test]["arg"][arg_ref] = {}

                    self.doc[current_test]["subtest"][current_subtest]["arg"] = arg_ref

                    continue

                # It is a known section. Parse its contents
                match = re.match(field_re, file_line)
                if match:
                    current_field = self.field_list[match.group(1).lower()]
                    match_val = match.group(2)

                    if handle_section == 'test':
                        self.doc[current_test][current_field] = match_val
                    else:
                        self.doc[current_test]["subtest"][current_subtest][current_field] = match_val

                    cur_arg = -1

                    continue

                if not has_test_or_subtest:
                    continue

                # Use hashes for arguments to avoid duplication
                match = re.match(r'arg\[(\d+)\]:\s*(.*)', file_line)
                if match:
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

                match = re.match(r'\@(\S+):\s*(.*)', file_line)
                if match:
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

                # We don't need a multi-lined version here
                match = re.match(r'arg\[(\d+)\]\.values:\s*(.*)', file_line)
                if match:
                    cur_arg = int(match.group(1)) - 1

                    if cur_arg not in self.doc[current_test]["arg"][arg_ref]:
                        self.doc[current_test]["arg"][arg_ref][cur_arg] = {}

                    values = match.group(2).replace(" ", "").split(",")
                    for split_val in values:
                        if split_val == "":
                            continue
                        self.doc[current_test]["arg"][arg_ref][cur_arg][split_val] = split_val

                    continue

                # Handle multi-line field contents
                if current_field:
                    match = re.match(r'\s+(.*)', file_line)
                    if match:
                        if handle_section == 'test':
                            dic = self.doc[current_test]
                        else:
                            dic = self.doc[current_test]["subtest"][current_subtest]

                        if dic[current_field] != '':
                            dic[current_field] += " "
                        dic[current_field] += match.group(1)
                        continue

                # Handle multi-line argument contents
                if cur_arg >= 0 and arg_ref is not None:
                    match = re.match(r'\s+\*?\s*(.*)', file_line)
                    if match:
                        match_val = match.group(1)

                        match = re.match(r'^(\<.*)\>$',self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element])
                        if match:
                            self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] = match.group(1) + ' ' + match_val + ">"
                        else:
                            if self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] != '':
                                self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] += ' '
                            self.doc[current_test]["arg"][arg_ref][cur_arg][cur_arg_element] += match_val
                    continue

                re.sub(r"\n$","", file_line)
                sys.exit(f"{fname}:{file_ln + 1}: Error: unrecognized line. Need to add field at %s?\n\t==> %s" %
                         (self.config_fname, file_line))

    def show_subtests(self, sort_field):

        """Show subtests, allowing sort and filter a field """

        if sort_field:
            test_subtests = self.get_subtests(sort_field)
            for val_key in sorted(test_subtests.keys()):
                if not test_subtests[val_key]:
                    continue
                if val_key == "":
                    print("not defined:")
                else:
                    print(f"{val_key}:")
                    for sub in test_subtests[val_key]:
                        print (f"  {sub}")
        else:
            for sub in self.get_subtests(sort_field)[""]:
                print (sub)

    def gen_testlist(self, directory, sort_field):

        """Generate testlists from the test documentation"""

        test_prefix = os.path.commonprefix(self.get_subtests()[""])
        test_prefix = re.sub(r'^igt@', '', test_prefix)

        test_subtests = self.get_subtests(sort_field)

        for test in test_subtests.keys():  # pylint: disable=C0201,C0206
            if not test_subtests[test]:
                continue

            testlist = test.lower()
            if testlist == "":
                fname = "other"
            elif testlist == "bat":
                fname = "fast-feedback"
            else:
                fname = testlist

            fname = directory + "/" + test_prefix + fname + ".testlist"
            fname = re.sub(r"[\s_]+", "-", fname)

            with open(fname, 'w', encoding='utf8') as handler:
                for sub in test_subtests[test]:
                    handler.write (f"{sub}\n")
                print(f"{fname} created.")
