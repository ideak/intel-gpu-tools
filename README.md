IGT GPU Tools
=============


Description
-----------

IGT GPU Tools is a collection of tools for development and testing of the DRM
drivers. There are many macro-level test suites that get used against the
drivers, including xtest, rendercheck, piglit, and oglconform, but failures from
those can be difficult to track down to kernel changes, and many require
complicated build procedures or specific testing environments to get useful
results. Therefore, IGT GPU Tools includes low-level tools and tests
specifically for development and testing of the DRM Drivers.

Generated documentation for the latest master is published under
<https://drm.pages.freedesktop.org/igt-gpu-tools/>.


Requirements
------------

See `Dockerfile.build-fedora` for up-to-date list of package names in Fedora
or `Dockerfile.build-debian-minimal` and `Dockerfile.build-debian` for Debian.

If your distribution packages IGT you can also use your package manager to
install the dependencies, e.g.:

    # dnf builddep igt-gpu-tools

But keep in mind that this may be slightly outdated and miss some
recently added dependencies for building the current master.


Building
--------

Oneliner to get started:

    $ meson build && ninja -C build

Note that meson insist on separate build directories from the source tree.

Running selfchecks for `lib/tests` and `tests/` is done with

    $ ninja -C build test

Documentation is built using

    $ ninja -C build igt-gpu-tools-doc


Running Tests
-------------

In `tests/` you can find a set of automated tests to run against the DRM
drivers to validate your changes. Many of the tests have subtests, which can
be listed by using the `--list-subtests` command line option and then run
using the --run-subtest option. If `--run-subtest` is not used, all subtests
will be run. Some tests have further options and these are detailed by using
the `--help` option.

Most of the test must be run as a root and with no X or Wayland compositor
running.

    # build/tests/core_auth
    IGT-Version: 1.24 (x86_64) (Linux: 5.3.0 x86_64)
    Starting subtest: getclient-simple
    Subtest getclient-simple: SUCCESS (0.001s)
    Starting subtest: getclient-master-drop
    Subtest getclient-master-drop: SUCCESS (0.000s)
    Starting subtest: basic-auth
    Subtest basic-auth: SUCCESS (0.000s)
    Starting subtest: many-magics
    Subtest many-magics: SUCCESS (0.000s)

    # build/tests/core_auth --run-subtest getclient-simple
    IGT-Version: 1.24 (x86_64) (Linux: 5.3.0 x86_64)
    Starting subtest: getclient-simple
    Subtest getclient-simple: SUCCESS (0.000s)


The test suite can be run using the `run-tests.sh` script available in the
`scripts/` directory. To use it make sure that `igt_runner` is built, e.g.:

    meson -Drunner=enabled build && ninja -C build

`run-tests.sh` has options for filtering and excluding tests from test
runs:

    -t <regex>      only include tests that match the regular expression
    -x <regex>      exclude tests that match the regular expression

Useful patterns for test filtering are described in the [API
documentation][API] and the full list of tests and subtests can be produced
by passing `-l` to the `run-tests.sh` script. Further options are are
detailed by using the `-h` option.

Results are written to a JSON file.

[API]: https://drm.pages.freedesktop.org/igt-gpu-tools/igt-gpu-tools-Core.html


IGT Containers
--------------

IGT is packed into nifty docker-compatible containers for ease of execution
and to avoid having to install all the dependencies. You can use
podman/docker to to run it on your system.

Oneliner to get you started with the latest master:

    # podman run --rm --priviledged registry.freedesktop.org/drm/igt-gpu-tools/igt:master


Other Things
------------

### `benchmarks/`

A collection of useful microbenchmarks that can be used to tune DRM code.

The benchmarks require KMS to be enabled.  When run with an X Server
running, they must be run as root to avoid the authentication
requirement.

Note that a few other microbenchmarks are in tests (e.g. `gem_gtt_speed`).

### `tools/`

A collection of debugging tools. They generally must be run as root, except
for the ones that just decode dumps.

### `docs/`

Contains the infrastructure to automatically generate igt-gpu-tools libraries
reference documentation. You need to have the gtk-doc tools installed.

To regenerate the html files when updating documentation, use:

    $ ninja -C build igt-gpu-tools-doc

If you've added/changed/removed a symbol or anything else that changes the
overall structure or indexes you need to reflect the change in
`igt-gpu-tools-sections.txt`. Entirely new sections also need to be added to
`igt-gpu-tools-docs.xml` in the appropriate place.

### `include/drm-uapi/`

Imported DRM uapi headers from airlied's drm-next branch.

These should be updated all together by executing `make headers_install` from
that branch of the kernel and then copying the resulting
`./usr/include/drm/*.h` in and committing with a note of which exact commit
from airlied's branch was used to generate them.
