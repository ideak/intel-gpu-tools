#!/usr/bin/env python3
#
# Copyright (c) 2015-2020 Intel Corporation
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

import argparse
import os
import sys
import textwrap

import codegen

h = None
c = None

hashed_funcs = {}

def data_type_to_ctype(ret_type):
    if ret_type == "uint64":
        return "uint64_t"
    elif ret_type == "float":
        return "double"
    else:
        raise Exception("Unhandled case for mapping \"" + ret_type + "\" to a C type")


def output_counter_read(gen, set, counter):
    if counter.read_hash in hashed_funcs:
        return

    c("\n")
    c("/* {0} :: {1} */".format(set.name, counter.get('name')))

    ret_type = counter.get('data_type')
    ret_ctype = data_type_to_ctype(ret_type)
    read_eq = counter.get('equation')

    c(ret_ctype)
    c(counter.read_sym + "(const struct intel_perf *perf,\n")
    c.indent(len(counter.read_sym) + 1)
    c("const struct intel_perf_metric_set *metric_set,\n")
    c("uint64_t *accumulator)\n")
    c.outdent(len(counter.read_sym) + 1)

    c("{")
    c.indent(4)

    gen.output_rpn_equation_code(set, counter, read_eq)

    c.outdent(4)
    c("}")

    hashed_funcs[counter.read_hash] = counter.read_sym


def output_counter_read_definition(gen, set, counter):
    if counter.read_hash in hashed_funcs:
        h("#define %s \\" % counter.read_sym)
        h.indent(4)
        h("%s" % hashed_funcs[counter.read_hash])
        h.outdent(4)
    else:
        ret_type = counter.get('data_type')
        ret_ctype = data_type_to_ctype(ret_type)
        read_eq = counter.get('equation')

        h(ret_ctype)
        h(counter.read_sym + "(const struct intel_perf *perf,\n")
        h.indent(len(counter.read_sym) + 1)
        h("const struct intel_perf_metric_set *metric_set,\n")
        h("uint64_t *accumulator);\n")
        h.outdent(len(counter.read_sym) + 1)

        hashed_funcs[counter.read_hash] = counter.read_sym


def output_counter_max(gen, set, counter):
    max_eq = counter.get('max_equation')

    if not max_eq or max_eq == "100":
        return

    if counter.max_hash in hashed_funcs:
        return

    c("\n")
    c("/* {0} :: {1} */".format(set.name, counter.get('name')))

    ret_type = counter.get('data_type')
    ret_ctype = data_type_to_ctype(ret_type)

    c(ret_ctype)
    c(counter.max_sym + "(const struct intel_perf *perf,\n")
    c.indent(len(counter.max_sym) + 1)
    c("const struct intel_perf_metric_set *metric_set,\n")
    c("uint64_t *accumulator)\n")
    c.outdent(len(counter.max_sym) + 1)

    c("{")
    c.indent(4)

    gen.output_rpn_equation_code(set, counter, max_eq)

    c.outdent(4)
    c("}")

    hashed_funcs[counter.max_hash] = counter.max_sym


def output_counter_max_definition(gen, set, counter):
    max_eq = counter.get('max_equation')

    if not max_eq or max_eq == "100":
        return

    if counter.max_hash in hashed_funcs:
        h("#define %s \\" % counter.max_sym)
        h.indent(4)
        h("%s" % hashed_funcs[counter.max_hash])
        h.outdent(4)
        h("\n")
    else:
        ret_type = counter.get('data_type')
        ret_ctype = data_type_to_ctype(ret_type)

        h(ret_ctype)

        h(counter.max_sym + "(const struct intel_perf *perf,")
        h.indent(len(counter.max_sym) + 1)
        h("const struct intel_perf_metric_set *metric_set,")
        h("uint64_t *accumulator);")
        h.outdent(len(counter.max_sym) + 1)
        h("\n")

        hashed_funcs[counter.max_hash] = counter.max_sym


def generate_equations(args, gens):
    global hashed_funcs

    header_file = os.path.basename(args.header)
    header_define = header_file.replace('.', '_').upper()

    hashed_funcs = {}
    c(textwrap.dedent("""\
        #include <stdlib.h>
        #include <string.h>

        #include <i915_drm.h>

        #include "i915/perf.h"
        #include "%s"

        #define MIN(x, y) (((x) < (y)) ? (x) : (y))
        #define MAX(a, b) (((a) > (b)) ? (a) : (b))

        double
        percentage_max_callback_float(const struct intel_perf *perf,
                                      const struct intel_perf_metric_set *metric_set,
                                      uint64_t *accumulator)
        {
           return 100;
        }

        uint64_t
        percentage_max_callback_uint64(const struct intel_perf *perf,
                                       const struct intel_perf_metric_set *metric_set,
                                       uint64_t *accumulator)
        {
           return 100;
        }

        """ % os.path.basename(args.header)))

    # Print out all equation functions.
    for gen in gens:
        for set in gen.sets:
            for counter in set.counters:
                output_counter_read(gen, set, counter)
                output_counter_max(gen, set, counter)

    hashed_funcs = {}
    h(textwrap.dedent("""\
        #ifndef __%s__
        #define __%s__

        #include <stddef.h>
        #include <stdint.h>
        #include <stdbool.h>

        struct intel_perf;
        struct intel_perf_metric_set;

        double
        percentage_max_callback_float(const struct intel_perf *perf,
                                      const struct intel_perf_metric_set *metric_set,
                                      uint64_t *accumulator);
        uint64_t
        percentage_max_callback_uint64(const struct intel_perf *perf,
                                       const struct intel_perf_metric_set *metric_set,
                                       uint64_t *accumulator);

        """ % (header_define, header_define)))

    # Print out all equation functions.
    for gen in gens:
        for set in gen.sets:
            for counter in set.counters:
                output_counter_read_definition(gen, set, counter)
                output_counter_max_definition(gen, set, counter)

    h(textwrap.dedent("""\

        #endif /* __%s__ */
        """ % header_define))


def main():
    global c
    global h

    parser = argparse.ArgumentParser()
    parser.add_argument("--header", help="Header file to write")
    parser.add_argument("--code", help="C file to write")
    parser.add_argument("xml_files", nargs='+', help="List of xml metrics files to process")

    args = parser.parse_args()

    # Note: either arg may == None
    h = codegen.Codegen(args.header)
    c = codegen.Codegen(args.code)

    gens = []
    for xml_file in args.xml_files:
        gens.append(codegen.Gen(xml_file, c))

    copyright = textwrap.dedent("""\
        /* Autogenerated file, DO NOT EDIT manually! generated by {}
         *
         * Copyright (c) 2018 Intel Corporation
         *
         * Permission is hereby granted, free of charge, to any person obtaining a
         * copy of this software and associated documentation files (the "Software"),
         * to deal in the Software without restriction, including without limitation
         * the rights to use, copy, modify, merge, publish, distribute, sublicense,
         * and/or sell copies of the Software, and to permit persons to whom the
         * Software is furnished to do so, subject to the following conditions:
         *
         * The above copyright notice and this permission notice (including the next
         * paragraph) shall be included in all copies or substantial portions of the
         * Software.
         *
         * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
         * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
         * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
         * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
         * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
         * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
         * DEALINGS IN THE SOFTWARE.
         */

        """).format(os.path.basename(__file__))

    h(copyright)
    c(copyright)

    generate_equations(args, gens)


if __name__ == '__main__':
    main()
