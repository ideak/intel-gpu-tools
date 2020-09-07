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

semantic_type_map = {
    "duration": "raw",
    "ratio": "event"
    }

def output_units(unit):
    return unit.replace(' ', '_').upper()


def output_counter_report(set, counter):
    data_type = counter.get('data_type')
    data_type_uc = data_type.upper()
    c_type = data_type

    if "uint" in c_type:
        c_type = c_type + "_t"

    semantic_type = counter.get('semantic_type')
    if semantic_type in semantic_type_map:
        semantic_type = semantic_type_map[semantic_type]

    semantic_type_uc = semantic_type.upper()

    c("\n")

    availability = counter.get('availability')
    if availability:
        set.gen.output_availability(set, availability, counter.get('name'))
        c.indent(4)

    c("counter = &metric_set->counters[metric_set->n_counters++];\n")
    c("counter->metric_set = metric_set;\n")
    c("counter->name = \"{0}\";\n".format(counter.get('name')))
    c("counter->symbol_name = \"{0}\";\n".format(counter.get('symbol_name')));
    c("counter->desc = \"{0}\";\n".format(counter.get('description')))
    c("counter->type = INTEL_PERF_LOGICAL_COUNTER_TYPE_{0};\n".format(semantic_type_uc))
    c("counter->storage = INTEL_PERF_LOGICAL_COUNTER_STORAGE_{0};\n".format(data_type_uc))
    c("counter->unit = INTEL_PERF_LOGICAL_COUNTER_UNIT_{0};\n".format(output_units(counter.get('units'))))
    c("counter->read_{0} = {1};\n".format(data_type, set.read_funcs[counter.get('symbol_name')]))
    c("counter->max_{0} = {1};\n".format(data_type, set.max_funcs[counter.get('symbol_name')]))
    c("intel_perf_add_logical_counter(perf, counter, \"{0}\");\n".format(counter.get('mdapi_group')))

    if availability:
        c.outdent(4)
        c("}\n")


def generate_metric_sets(args, gen):
    c(textwrap.dedent("""\
        #include <stddef.h>
        #include <stdint.h>
        #include <stdlib.h>
        #include <stdbool.h>
        #include <assert.h>

        #include "i915_drm.h"

        """))

    c("#include \"{0}\"".format(os.path.basename(args.header)))
    c("#include \"{0}\"".format(os.path.basename(args.equations_include)))
    c("#include \"{0}\"".format(os.path.basename(args.registers_include)))

    # Print out all set registration functions for each set in each
    # generation.
    for set in gen.sets:
        c("\nstatic void\n")
        c(gen.chipset + "_add_" + set.underscore_name + "_metric_set(struct intel_perf *perf)")
        c("{\n")
        c.indent(4)

        c("struct intel_perf_metric_set *metric_set;\n")
        c("struct intel_perf_logical_counter *counter;\n\n")

        counters = sorted(set.counters, key=lambda k: k.get('symbol_name'))

        c("metric_set = calloc(1, sizeof(*metric_set));\n")
        c("metric_set->name = \"" + set.name + "\";\n")
        c("metric_set->symbol_name = \"" + set.symbol_name + "\";\n")
        c("metric_set->hw_config_guid = \"" + set.hw_config_guid + "\";\n")
        c("metric_set->counters = calloc({0}, sizeof(struct intel_perf_logical_counter));\n".format(str(len(counters))))
        c("metric_set->n_counters = 0;\n")
        c("metric_set->perf_oa_metrics_set = 0; // determined at runtime\n")

        if gen.chipset == "hsw":
            c(textwrap.dedent("""\
                metric_set->perf_oa_format = I915_OA_FORMAT_A45_B8_C8;

                metric_set->perf_raw_size = 256;
                metric_set->gpu_time_offset = 0;
                metric_set->a_offset = 1;
                metric_set->b_offset = metric_set->a_offset + 45;
                metric_set->c_offset = metric_set->b_offset + 8;
                metric_set->perfcnt_offset = metric_set->c_offset + 8;

            """))
        else:
            c(textwrap.dedent("""\
                metric_set->perf_oa_format = I915_OA_FORMAT_A32u40_A4u32_B8_C8;

                metric_set->perf_raw_size = 256;
                metric_set->gpu_time_offset = 0;
                metric_set->gpu_clock_offset = 1;
                metric_set->a_offset = 2;
                metric_set->b_offset = metric_set->a_offset + 36;
                metric_set->c_offset = metric_set->b_offset + 8;
                metric_set->perfcnt_offset = metric_set->c_offset + 8;

            """))

        c("%s_%s_add_registers(perf, metric_set);" % (gen.chipset, set.underscore_name))

        c("intel_perf_add_metric_set(perf, metric_set);");
        c("\n")

        for counter in counters:
            output_counter_report(set, counter)

        c("\nassert(metric_set->n_counters <= {0});\n".format(len(counters)));

        c.outdent(4)
        c("}\n")

    c("\nvoid")
    c("intel_perf_load_metrics_" + gen.chipset + "(struct intel_perf *perf)")
    c("{")
    c.indent(4)

    for set in gen.sets:
        c("{0}_add_{1}_metric_set(perf);".format(gen.chipset, set.underscore_name))

    c.outdent(4)
    c("}")



def main():
    global c
    global h

    parser = argparse.ArgumentParser()
    parser.add_argument("--header", help="Header file to write")
    parser.add_argument("--code", help="C file to write")
    parser.add_argument("--equations-include", help="Equations header file")
    parser.add_argument("--registers-include", help="Registers header file")
    parser.add_argument("--xml-file", help="Xml file to generate metric sets from")

    args = parser.parse_args()

    # Note: either arg may == None
    h = codegen.Codegen(args.header)
    c = codegen.Codegen(args.code)

    gen = codegen.Gen(args.xml_file, c)

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

    header_file = os.path.basename(args.header)
    header_define = header_file.replace('.', '_').upper()

    h(copyright)
    h(textwrap.dedent("""\
        #ifndef %s
        #define %s

        #include "i915/perf.h"

        """ % (header_define, header_define)))

    # Print out all set registration functions for each generation.
    h("void intel_perf_load_metrics_" + gen.chipset + "(struct intel_perf *perf);\n\n")

    h(textwrap.dedent("""\
        #endif /* %s */
        """ % header_define))

    c(copyright)
    generate_metric_sets(args, gen)


if __name__ == '__main__':
    main()
