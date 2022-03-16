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
./scripts/run-tests.sh -T my.testlist -k ~/linux -c scripts/code_cov_capture.sh -P
```

### Code Coverage Collect script

While any script could in thesis be used, currently, there are two ones
under `scripts/`:

- `scripts/code_cov_capture.sh`:

  Assumes that the Kernel was built at the same machine, and uses
  the lcov tool to generate GCC-independent code coverage data,
  in the form of `*.info` files. Internally, it uses an shell environment
  variable (`IGT_KERNEL_TREE`), which points to the place where the Kernel
  source and objects are contained.

  Such script requires `lcov` tool to be installed at the test machine.

- `scripts/code_cov_gather_on_test.py`:

  Generates a gzipped tarbal with the code coverage counters in
  binary format. Such kind of output should then be parsed at
  the same machine where the Kernel as built, as its content is not
  ony dependent on the Kernel source, but also on the Kernel output
  objects.

For each script, the igt_runner passes just one parameter: the results
directory + the test name.

For instance, if it is needed to run a test called
`debugfs_test (read_all_entries)` using `scripts/code_cov_capture.sh`
parameter, e. g.:

```
$ echo "igt@debugfs_test@read_all_entries" > my.testlist
$ ./scripts/run-tests.sh -T my.testlist -k ~/linux -c scripts/code_cov_capture.sh -P
Found test list: "/basedir/igt/build/tests/test-list.txt"
[31410.499969] [1/1] debugfs_test (read_all_entries)
[31411.060446] Storing code coverage results...
[31418.01]     Code coverage wrote to /basedir/igt/results/code_cov/debugfs_test_read_all_entries.info
Done.
```

The script will be called as:

```
scripts/code_cov_capture.sh /basedir/igt/results/code_cov/debugfs_test_read_all_entries
```

Please notice that any character that it is not a number nor a letter at the
test name will be converted into '_', as other characters are not supported
as titles at the lcov files.

### Passing extra arguments to the script

If any extra global parameters are needed by the script, those can be sent
via shell's environment var.

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
coverage documentation. It is used by `scripts/code_cov_capture.sh` script
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
as with a single or multiple `*.info` files by `scripts/code_cov_capture.sh`
or stored in raw formats inside `*.tar.gz` file(s) by
`scripts/code_cov_gather_on_test.py`, there's a script that does all the
required steps to build the code coverage html reports:
`scripts/code_cov_gen_report.sh`.

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
  directory at the test machine, created by
  `scripts/code_cov_gather_on_test.py` script.

- `--force-override`

  Allow using a non-empty directory for `--output-dir`.

`--info` and `--tar` are mutually exclusive and at least one of them should
be specified.

## References

More information is available at Kernel gcov documentation:
[Using gcov with the Linux kernel](https://www.kernel.org/doc/html/latest/dev-tools/gcov.html).

