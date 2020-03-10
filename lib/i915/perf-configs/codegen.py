import xml.etree.cElementTree as et

class Codegen:

    _file = None
    _indent = 0

    endl="\n"
    use_tabs = False

    def __init__(self, filename = None):
        if filename != None:
            self._file = open(filename, 'w')

    def __call__(self, *args):
        if self._file:
            code = ' '.join(map(str, args))
            for line in code.splitlines():
                indent = ''.rjust(self._indent)

                if self.use_tabs:
                    indent = indent.replace("        ", "\t")

                text = indent + line
                self._file.write(text.rstrip() + self.endl)

    #without indenting or new lines
    def frag(self, *args):
        code = ' '.join(map(str, args))
        self._file.write(code)

    def indent(self, n):
        self._indent = self._indent + n
    def outdent(self, n):
        self._indent = self._indent - n


class Counter:
    def __init__(self, set, xml):
        self.xml = xml
        self.set = set
        self.read_hash = None
        self.max_hash = None

        self.read_sym = "{0}__{1}__{2}__read".format(self.set.gen.chipset,
                                                     self.set.underscore_name,
                                                     self.xml.get('underscore_name'))

        max_eq = self.xml.get('max_equation')
        if not max_eq:
            self.max_sym = "NULL /* undefined */"
        elif max_eq == "100":
            self.max_sym = "percentage_max_callback_" + self.xml.get('data_type')
        else:
            self.max_sym = "{0}__{1}__{2}__max".format(self.set.gen.chipset,
                                                       self.set.underscore_name,
                                                       self.xml.get('underscore_name'))

    def get(self, prop):
        return self.xml.get(prop)

    def compute_hashes(self):
        if self.read_hash is not None:
            return

        def replace_func(token):
            if token[0] != "$":
                return token
            if token not in self.set.counter_vars:
                return token
            self.set.counter_vars[token].compute_hashes()
            return self.set.counter_vars[token].read_hash

        read_eq = self.xml.get('equation')
        self.read_hash = ' '.join(map(replace_func, read_eq.split()))

        max_eq = self.xml.get('max_equation')
        if max_eq:
            self.max_hash = ' '.join(map(replace_func, max_eq.split()))

class Set:
    def __init__(self, gen, xml):
        self.gen = gen
        self.xml = xml

        self.counter_vars = {}
        self.max_funcs = {}
        self.read_funcs = {}
        self.counter_hashes = {}

        self.counters = []
        xml_counters = self.xml.findall("counter")
        for xml_counter in xml_counters:
            counter = Counter(self, xml_counter)
            self.counters.append(counter)
            self.counter_vars["$" + counter.get('symbol_name')] = counter
            self.max_funcs[counter.get('symbol_name')] = counter.max_sym
            self.read_funcs[counter.get('symbol_name')] = counter.read_sym

        for counter in self.counters:
            counter.compute_hashes()

    @property
    def hw_config_guid(self):
        return self.xml.get('hw_config_guid')

    @property
    def name(self):
        return self.xml.get('name')

    @property
    def symbol_name(self):
        return self.xml.get('symbol_name')

    @property
    def underscore_name(self):
        return self.xml.get('underscore_name')

    def findall(self, path):
        return self.xml.findall(path)

    def find(self, path):
        return self.xml.find(path)


class Gen:
    def __init__(self, filename, c):
        self.filename = filename
        self.xml = et.parse(self.filename)
        self.chipset = self.xml.find('.//set').get('chipset').lower()
        self.sets = []
        self.c = c

        for xml_set in self.xml.findall(".//set"):
            self.sets.append(Set(self, xml_set))

        self.ops = {}
        #                     (n operands, emitter)
        self.ops["FADD"]     = (2, self.emit_fadd)
        self.ops["FDIV"]     = (2, self.emit_fdiv)
        self.ops["FMAX"]     = (2, self.emit_fmax)
        self.ops["FMUL"]     = (2, self.emit_fmul)
        self.ops["FSUB"]     = (2, self.emit_fsub)
        self.ops["READ"]     = (2, self.emit_read)
        self.ops["READ_REG"] = (1, self.emit_read_reg)
        self.ops["UADD"]     = (2, self.emit_uadd)
        self.ops["UDIV"]     = (2, self.emit_udiv)
        self.ops["UMUL"]     = (2, self.emit_umul)
        self.ops["USUB"]     = (2, self.emit_usub)
        self.ops["UMIN"]     = (2, self.emit_umin)
        self.ops["<<"]       = (2, self.emit_lshft)
        self.ops[">>"]       = (2, self.emit_rshft)
        self.ops["AND"]      = (2, self.emit_and)

        self.exp_ops = {}
        #                 (n operands, splicer)
        self.exp_ops["AND"]  = (2, self.splice_bitwise_and)
        self.exp_ops["UGTE"] = (2, self.splice_ugte)
        self.exp_ops["ULT"]  = (2, self.splice_ult)
        self.exp_ops["&&"]   = (2, self.splice_logical_and)

        self.hw_vars = {
            "$EuCoresTotalCount": { 'c': "perf->devinfo.n_eus", 'desc': "The total number of execution units" },
            "$EuSlicesTotalCount": { 'c': "perf->devinfo.n_eu_slices" },
            "$EuSubslicesTotalCount": { 'c': "perf->devinfo.n_eu_sub_slices" },
            "$EuThreadsCount": { 'c': "perf->devinfo.eu_threads_count" },
            "$SliceMask": { 'c': "perf->devinfo.slice_mask" },
            "$DualSubsliceMask": { 'c': "perf->devinfo.subslice_mask" },
            "$SubsliceMask": { 'c': "perf->devinfo.subslice_mask" },
            "$GpuTimestampFrequency": { 'c': "perf->devinfo.timestamp_frequency" },
            "$GpuMinFrequency": { 'c': "perf->devinfo.gt_min_freq" },
            "$GpuMaxFrequency": { 'c': "perf->devinfo.gt_max_freq" },
            "$SkuRevisionId": { 'c': "perf->devinfo.revision" },
            "$QueryMode": { 'c': "perf->devinfo.query_mode" },
        }

    def emit_fadd(self, tmp_id, args):
        self.c("double tmp{0} = {1} + {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    # Be careful to check for divide by zero...
    def emit_fdiv(self, tmp_id, args):
        self.c("double tmp{0} = {1};".format(tmp_id, args[1]))
        self.c("double tmp{0} = {1};".format(tmp_id + 1, args[0]))
        self.c("double tmp{0} = tmp{1} ? tmp{2} / tmp{1} : 0;".format(tmp_id + 2, tmp_id + 1, tmp_id))
        return tmp_id + 3

    def emit_fmax(self, tmp_id, args):
        self.c("double tmp{0} = {1};".format(tmp_id, args[1]))
        self.c("double tmp{0} = {1};".format(tmp_id + 1, args[0]))
        self.c("double tmp{0} = MAX(tmp{1}, tmp{2});".format(tmp_id + 2, tmp_id, tmp_id + 1))
        return tmp_id + 3

    def emit_fmul(self, tmp_id, args):
        self.c("double tmp{0} = {1} * {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_fsub(self, tmp_id, args):
        self.c("double tmp{0} = {1} - {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_read(self, tmp_id, args):
        type = args[1].lower()
        self.c("uint64_t tmp{0} = accumulator[metric_set->{1}_offset + {2}];".format(tmp_id, type, args[0]))
        return tmp_id + 1

    # Disabled here as the generated code is not capturing registers. This
    # will only be useful for query mode where the driver captures
    # additional registers.
    def emit_read_reg(self, tmp_id, args):
        self.c("uint64_t tmp{0} = 0;".format(tmp_id))
        return tmp_id + 1

    def emit_uadd(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} + {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    # Be careful to check for divide by zero...
    def emit_udiv(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1};".format(tmp_id, args[1]))
        self.c("uint64_t tmp{0} = {1};".format(tmp_id + 1, args[0]))
        self.c("uint64_t tmp{0} = tmp{1} ? tmp{2} / tmp{1} : 0;".format(tmp_id + 2, tmp_id + 1, tmp_id))
        return tmp_id + 3

    def emit_umul(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} * {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_usub(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} - {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_umin(self, tmp_id, args):
        self.c("uint64_t tmp{0} = MIN({1}, {2});".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_lshft(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} << {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_rshft(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} >> {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def emit_and(self, tmp_id, args):
        self.c("uint64_t tmp{0} = {1} & {2};".format(tmp_id, args[1], args[0]))
        return tmp_id + 1

    def brkt(self, subexp):
        if " " in subexp:
            return "(" + subexp + ")"
        else:
            return subexp

    def splice_bitwise_and(self, args):
        return self.brkt(args[1]) + " & " + self.brkt(args[0])

    def splice_logical_and(self, args):
        return self.brkt(args[1]) + " && " + self.brkt(args[0])

    def splice_ult(self, args):
        return self.brkt(args[1]) + " < " + self.brkt(args[0])

    def splice_ugte(self, args):
        return self.brkt(args[1]) + " >= " + self.brkt(args[0])

    def output_rpn_equation_code(self, set, counter, equation):
        self.c("/* RPN equation: " + equation + " */")
        tokens = equation.split()
        stack = []
        tmp_id = 0
        tmp = None

        for token in tokens:
            stack.append(token)
            while stack and stack[-1] in self.ops:
                op = stack.pop()
                argc, callback = self.ops[op]
                args = []
                for i in range(0, argc):
                    operand = stack.pop()
                    if operand[0] == "$":
                        if operand in self.hw_vars:
                            operand = self.hw_vars[operand]['c']
                        elif operand in set.counter_vars:
                            reference = set.counter_vars[operand]
                            operand = set.read_funcs[operand[1:]] + "(perf, metric_set, accumulator)"
                        else:
                            raise Exception("Failed to resolve variable " + operand + " in equation " + equation + " for " + set.name + " :: " + counter.get('name'));
                    args.append(operand)

                tmp_id = callback(tmp_id, args)

                tmp = "tmp{0}".format(tmp_id - 1)
                stack.append(tmp)

        if len(stack) != 1:
            raise Exception("Spurious empty rpn code for " + set.name + " :: " +
                    counter.get('name') + ".\nThis is probably due to some unhandled RPN function, in the equation \"" +
                    equation + "\"")

        value = stack[-1]

        if value in self.hw_vars:
            value = self.hw_vars[value]['c']
        if value in set.counter_vars:
            value = set.read_funcs[value[1:]] + "(perf, metric_set, accumulator)"

        self.c("\nreturn " + value + ";")

    def splice_rpn_expression(self, set, counter_name, expression):
        tokens = expression.split()
        stack = []

        for token in tokens:
            stack.append(token)
            while stack and stack[-1] in self.exp_ops:
                op = stack.pop()
                argc, callback = self.exp_ops[op]
                args = []
                for i in range(0, argc):
                    operand = stack.pop()
                    if operand[0] == "$":
                        if operand in self.hw_vars:
                            operand = self.hw_vars[operand]['c']
                        else:
                            raise Exception("Failed to resolve variable " + operand + " in expression " + expression + " for " + set.name + " :: " + counter_name)
                    args.append(operand)

                subexp = callback(args)

                stack.append(subexp)

        if len(stack) != 1:
            raise Exception("Spurious empty rpn expression for " + set.name + " :: " +
                    counter_name + ".\nThis is probably due to some unhandled RPN operation, in the expression \"" +
                    expression + "\"")

        return stack[-1]

    def output_availability(self, set, availability, counter_name):
        expression = self.splice_rpn_expression(set, counter_name, availability)
        lines = expression.split(' && ')
        n_lines = len(lines)
        if n_lines == 1:
            self.c("if (" + lines[0] + ") {")
        else:
            self.c("if (" + lines[0] + " &&")
            self.c.indent(4)
            for i in range(1, (n_lines - 1)):
                self.c(lines[i] + " &&")
            self.c(lines[(n_lines - 1)] + ") {")
            self.c.outdent(4)
