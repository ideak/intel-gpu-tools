# Collecting code coverage data from IGT tests

## Introduction

Ensuring that a test plan covers all the driver code is not trivial. Also,
as time goes by, changes at both the tests and drivers may badly affect
the code coverage. So, some tools are needed in order to be able to verify
and improve the driver test coverage. While static analyzers can help
checking the driver's code, it is not as effective as runtime tests.

Thankfully gcc has a feature which allows capturing such data in realtime,
called gcov. LLVM/clang also has a similar feature (llvm-cov). Such feature
is available at the Linux Kernel since 2009.

## Building a Kernel with GCOV support

Enabling GCOV at the Linux Kernel requires two steps:

1. Enable GCOV_KERNEL:

   ```
   ./scripts/config -e DEBUG_FS -e GCOV_KERNEL
   ```


2. Enable per-driver or per-makefile GCOV support. In order to enable support
   for all DRM drivers:

   ```
   for i in $(find drivers/gpu/drm/ -name Makefile); do
       sed '1 a GCOV_PROFILE := y' -i $i
   done
   ```

When gcov is enabled for a given driver or directory, GCC will generate
some special object files, like:

```
...
drivers/gpu/drm/drm_probe_helper.gcno
drivers/gpu/drm/drm_dp_dual_mode_helper.gcno
drivers/gpu/drm/drm_plane.gcno
drivers/gpu/drm/drm_lease.gcno
drivers/gpu/drm/drm_mipi_dsi.gcno
drivers/gpu/drm/drm_dsc.gcno
drivers/gpu/drm/drm_property.gcno
drivers/gpu/drm/drm_dp_aux_dev.gcno
drivers/gpu/drm/drm_blend.gcno
...
```

Those will be stored at the Kernel object directory, which is usually
the same as the Kernel source directory, except if the Kernel was built
with:

```
make O=kernel_output_dir
```

Such compile-time files are compiler-dependent and they're needed in order
to properly decode the code coverage counters that will be produced in
runtime.

## Collecting GCOV data in runtime

Once a GCOV-enabled Kernel boots, the Kernel will keep track of the code
monitored via GCOV under sysfs, at `/sys/kernel/debug/gcov/`.

There is a special file there: `/sys/kernel/debug/gcov/reset`. When something
is written to it, all counters will be cleaned.

There are also driver-related counters and softlinks stored there:

```
ls -la /basedir/linux/drivers/gpu/drm/
...
-rw------- 1 root root 0 Feb 16 07:03 drm_probe_helper.gcda
lrwxrwxrwx 1 root root 0 Feb 16 07:03 drm_probe_helper.gcno -> /basedir/linux/drivers/gpu/drm/drm_probe_helper.gcno
-rw------- 1 root root 0 Feb 16 07:03 drm_property.gcda
lrwxrwxrwx 1 root root 0 Feb 16 07:03 drm_property.gcno -> /basedir/linux/drivers/gpu/drm/drm_property.gcno
-rw------- 1 root root 0 Feb 16 07:03 drm_rect.gcda
lrwxrwxrwx 1 root root 0 Feb 16 07:03 drm_rect.gcno -> /basedir/linux/drivers/gpu/drm/drm_rect.gcno
...
```

The actual counters are stored at the *.gcda files on a compiler-dependent
format.

### calling `igt_runner` directly

When code coverage support is enabled, the `igt_runner` tool will internally
clean up the counters before starting test(s). Once test(s) finish, it will
also run an external script that will be responsible for collecting the data
and store on some file.

Enabling code coverage data collect can be done either per test or as
a hole for an entire test list, by using those command line options:

- `--collect-code-cov`

  Enables gcov-based collect of code coverage for tests.

- `--coverage-per-test`

  Stores code coverage results per each test. This option implies
  `--collect-code-cov`.

For those options to work, it is mandatory to specifiy what script will
be used to collect the data with `--collect-script` _file_name_.

### calling `./scripts/run-tests.sh` script

The `run-tests.sh` script can used instead as a frontend for igt_runner.
It has the following options:

- `-c <capture_script>`

  Capture gcov code coverage using the _capture_script_

- `-P`

  Store code coverage results per each test.

- `-k` _kernel_dir_

  Linux Kernel source code directory used to generate code coverage builds.
  This is passed through the capture script via the `IGT_KERNEL_TREE`
  shell environment variable.

So, for instance, if one wans to capture code coverage data from the
Kernel that was built at the same machine, at the directory `~/linux`,
and wants to capture one file per test, it would use:

```
./scripts/run-tests.sh -T my.testlist -k ~/linux -c code_cov_capture -P
```

### Code Coverage Collect script

While any script could in thesis be used, currently, there are two ones
under the IGT's `scripts/` source directory:

- `code_cov_capture`:

  Assumes that the Kernel was built at the same machine, and uses
  the lcov tool to generate GCC-independent code coverage data,
  in the form of `*.info` files. Internally, it uses an shell environment
  variable (`IGT_KERNEL_TREE`), which points to the place where the Kernel
  source and objects are contained.

  Such script requires `lcov` tool to be installed at the test machine.

- `code_cov_gather_on_test`:

  Generates a gzipped tarbal with the code coverage counters in
  binary format. Such kind of output should then be parsed at
  the same machine where the Kernel as built, as its content is not
  ony dependent on the Kernel source, but also on the Kernel output
  objects.

For each script, the igt_runner passes just one parameter: the results
directory + the test name.

For instance, if it is needed to run a test called
`debugfs_test (read_all_entries)` using `code_cov_capture`
parameter, e. g.:

```
$ echo "igt@debugfs_test@read_all_entries" > my.testlist
$ ./scripts/run-tests.sh -T my.testlist -k ~/linux -c code_cov_capture -P
Found test list: "/basedir/igt/build/tests/test-list.txt"
[31410.499969] [1/1] debugfs_test (read_all_entries)
[31411.060446] Storing code coverage results...
[31418.01]     Code coverage wrote to /basedir/igt/results/code_cov/debugfs_test_read_all_entries.info
Done.
```

The script will be called as:

```
code_cov_capture results/code_cov/debugfs_test_read_all_entries
```

Please notice that any character that it is not a number nor a letter at the
test name will be converted into '_', as other characters are not supported
as titles at the lcov files.

### Passing extra arguments to the script

If any extra global parameters are needed by the script, those can be sent
via shell's environment var.

## Parsing data from code coverage *.info files

The `*.info` files generated by `lcov` are plain text files that list the
tests that were executed in runtime.

The `code_cov_parse_info` script has some logic on it that allows
printing the called functions stored inside the `*.info` file. It can also
optionally apply the following filters. Its main options are:

- `--stat` or `--statistics`

  Prints code coverage statistics.

  It displays function, line, branch and file coverage percentage.

  The statistics report is affected by the applied filters.

- `--print-coverage`, `--print` or `-p`


  Prints the functions that were executed in runtime and how many times
  they were reached.

  The function coverage report is affected by the applied filters.

- `--print-unused` or `-u`

  Prints the functions that were never reached.

  The function coverage report is affected by the applied filters.

- `--show-lines` or `--show_lines`

  When printing per-function code coverage data, always output the source
  file and the line number where the function is defined.

- `--output` *output file* or `-o` *output file*

  Produces an output file merging all input files.

  The generated output file is affected by the applied filters.

- `--show-files` or `--show_files`

   Shows the list of files that were useed to produce the code coverage
   results.

- It also has a set of parameters that filters the code coverage results:
  `--only-drm`, `--only-i915`, `--func-filters`, `--source-filters`,
  `--ignore-unused`.
  When used, all coverage displayed reports, and the stored output file
  will be affected by such filters.

More details can be seen by calling:

```
code_cov_parse_info --help

```

or:

```
code_cov_parse_info --man

```

### The `*.info` file format

The `*.info` files contain several fields on it, grouped into records.
An info file looks like:

```
TN:fbdev_eof
...
SF:/basedir/linux/drivers/gpu/drm/i915/intel_runtime_pm.c
...
FN:158,__intel_runtime_pm_get
FNDA:2,__intel_runtime_pm_get
...
end_of_record
SF:<some other file>
...
end_of_record
...
```

The main fields at the above record are:

- `TN:`	Test name
- `SF:`	Source file
- `FN:`	line_number   function_name
- `FNDA:` call_count  function_name

So, the above example means that, inside
`drivers/gpu/drm/i915/intel_runtime_pm.c` there's a function
`__intel_runtime_pm_get()` which it was called 2 times.

## Generating code coverage documentation

The `lcov` package contains the needed tools to parse and generate code
coverage documentation. It is used by `code_cov_capture` script
to convery from compiler-dependent `*.gcno` counters into a
compiler-independent format (`*.info`).

Grouping multiple `*.info` files is as easy as running:

```
cat core*.info > all_core.info
```

The `lcov` package also contains a tool which converts a given `*.info` file
into html patches, called `genhtml`.

As the output can actually show the code source file, `genhtml` need access
not only to the info file, but also to the Kernel directory with the
source files. Some optional arguments can be used at the command line, or
can be stored at `/etc/lcovrc` or `~/.lcovrc` files.

As generating the documentation depends wheather the results were generated
as with a single or multiple `*.info` files by `code_cov_capture`
or stored in raw formats inside `*.tar.gz` file(s) by
`code_cov_gather_on_test`, there's a script that does all the
required steps to build the code coverage html reports:
`code_cov_gen_report`. Besides its own command line arguments, it
also accepts arguments to be passed to `code_cov_parse_info`.

If a `code_cov_parse_info` command line parameter is passed, it will
also call the script, in order to use a filtered `*.info` file to be
used when generating the HTML reports.

It requires the following arguments:

- `--read`  _file or dir_ (or `-r` _file or dir_)

  File or directory where the code coverage capture file(s) is(are) located.

- `--kernel-source` _dir_ (or `-k` _dir_)

  Kernel source directory.

- `--kernel-object` _dir_ (or `-O` _dir_)

  Kernel object directory. Only needed when Kernel was built with `make O=dir`.

- `--output-dir` _dir_ (or `-o` _dir)

  Directory where the html output will be stored. By default, the script
  won't let re-use an already existing directory.

- `--info`

  The files specified by `--read` parameter are at lcov's `*.info` format.

- `--tar`

  The files specified by `--read` are gzipped tarballs containing all
  `*.gcno` files and all `*.gcda` softlinks from the `/sys/kernel/debug/gcov/`
  directory at the test machine, created by `code_cov_gather_on_test` script.

- `--force-override`

  Allow using a non-empty directory for `--output-dir`.

- It also accepts `--print`, `--only-drm`, `--only-i915` and `--ignore-unused`
  options from `code_cov_parse_info`.

`--info` and `--tar` are mutually exclusive and at least one of them should
be specified.

## Code coverage capture script example

### Capture, parse and generate code coverage html data

The script below provides a simple yet powerful script using code
coverage capture on a test machine that also contains the Linux
Kernel source and objects. It assumes that LGT was installed.

```
#/bin/bash -e

TESTLIST="my_tests.testlist"
OUT_DIR="${HOME}/results"

mkdir -p $OUT_DIR/html

echo "igt@debugfs_test@read_all_entries" > $TESTLIST
echo "igt@core_auth@basic-auth" >> $TESTLIST
echo "igt@gem_exec_basic@basic" >> $TESTLIST

sudo IGT_KERNEL_TREE="${HOME}/linux" igt_runner -s -o --coverage-per-test \
                  --collect-script code_cov_capture --test-list $TESTLIST \
                  /usr/local/libexec/igt-gpu-tools $OUT_DIR/ | sed s,$HOME/,,

sudo chown -R $(id -u):$(id -g) $OUT_DIR/

for i in $OUT_DIR/code_cov/*.info; do
        echo -e "\n$(basename $i):"
        code_cov_parse_info --only-drm --ignore-unused --stat $i
done
echo -e "\nTOTAL:"
code_cov_parse_info --only-drm --stat --output $OUT_DIR/results.info \
        $OUT_DIR/code_cov/*.info

cd $OUT_DIR/html
genhtml -q -s --legend --branch-coverage $OUT_DIR/results.info
```

Running such script produces the following output:

```
[3622.993304] [1/3] debugfs_test (read_all_entries)
[3631.95]     Code coverage wrote to results/code_cov/debugfs_test_read_all_entries.info
[3626.217016] Storing code coverage results...
[3631.957998] [2/3] core_auth (basic-auth)
[3638.03]     Code coverage wrote to results/code_cov/core_auth_basic_auth.info
[3632.116024] Storing code coverage results...
[3638.070869] [3/3] gem_exec_basic (basic)
[3644.24]     Code coverage wrote to results/code_cov/gem_exec_basic_basic.info
[3638.366790] Storing code coverage results...
Done.

core_auth_basic_auth.info:
  lines......: 11.7% (8217 of 70257 lines)
  functions..: 7.1% (776 of 10971 functions)
  branches...: 7.0% (3596 of 51041 branches)
Ignored......: non-drm headers and source files where none of its code ran.
Source files.: 23.27% (165 of 709 total), 29.57% (165 of 558 filtered)

debugfs_test_read_all_entries.info:
  lines......: 19.3% (20266 of 104802 lines)
  functions..: 17.5% (1922 of 10971 functions)
  branches...: 12.7% (9462 of 74555 branches)
Ignored......: non-drm headers and source files where none of its code ran.
Source files.: 34.70% (246 of 709 total), 44.09% (246 of 558 filtered)

gem_exec_basic_basic.info:
  lines......: 17.1% (14964 of 87503 lines)
  functions..: 13.0% (1422 of 10971 functions)
  branches...: 10.1% (6446 of 63758 branches)
Ignored......: non-drm headers and source files where none of its code ran.
Source files.: 30.89% (219 of 709 total), 39.25% (219 of 558 filtered)

TOTAL:
  lines......: 15.5% (25821 of 166849 lines)
  functions..: 22.1% (2429 of 10971 functions)
  branches...: 10.5% (11869 of 112665 branches)
Ignored......: non-drm headers.
Source files.: 78.70% (558 of 709 total)
```

### Reporting detailed function coverage stored on *.info files

The `code_cov_parse_info` script can be used alone in order to provide
a text file output containing code coverage data obtained from a *.info
file. For example, listing code coverage usage for all functions whose name
contains "edid_" can be done with:

```
$ echo edid_ >filter.txt
$ code_cov_parse_info --func-filters filter.txt results/results.info -p -u --stat
TEST: Code_coverage_tests
__drm_get_edid_firmware_path(): unused
__drm_set_edid_firmware_path(): unused
displayid_iter_edid_begin(): executed 10 times
drm_add_edid_modes(): executed 2 times
drm_add_override_edid_modes(): unused
drm_connector_attach_edid_property(): unused
drm_connector_update_edid_property(): executed 8 times
drm_dp_send_real_edid_checksum(): unused
drm_edid_are_equal(): executed 4 times
drm_edid_block_valid(): executed 8 times
drm_edid_duplicate(): unused
drm_edid_get_monitor_name(): unused
drm_edid_header_is_valid(): executed 4 times
drm_edid_is_valid(): executed 2 times
drm_edid_to_eld(): executed 2 times
drm_edid_to_sad(): unused
drm_edid_to_speaker_allocation(): unused
drm_find_edid_extension(): executed 22 times
drm_get_edid_switcheroo(): unused
drm_load_edid_firmware(): executed 2 times
edid_firmware_get(): unused
edid_firmware_set(): unused
edid_fixup_preferred(): unused
edid_get_quirks(): executed 6 times
edid_load(): unused
edid_open(): executed 4 times
edid_show() from linux/drivers/gpu/drm/drm_debugfs.c: executed 4 times
edid_show() from linux/drivers/gpu/drm/drm_sysfs.c: unused
edid_vendor(): executed 348 times
edid_write(): unused
intel_panel_edid_downclock_mode(): unused
intel_panel_edid_fixed_mode(): unused
is_edid_digital_input_dp(): unused
  lines......: 5.5% (5 of 91 lines)
  functions..: 42.4% (14 of 33 functions)
  branches...: 1.9% (1 of 52 branches)
Ignored......: unmatched functions m/(?^:edid_)/ and source files where none of its code ran.
Source files.: 0.90% (5 of 558 total), 55.56% (5 of 9 filtered)
```

When the function is unique, it will just display the function name and how
many times the IGT test(s) executed it. When the same function name exists
on multiple files (like the `edid_show()` on the above example), it will
display multiple lines, one for each different function/file combination.

## References

More information is available at Kernel gcov documentation:
[Using gcov with the Linux kernel](https://www.kernel.org/doc/html/latest/dev-tools/gcov.html).
