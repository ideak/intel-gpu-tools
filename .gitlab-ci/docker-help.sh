#!/bin/sh
#
# Copyright © 2019 Intel Corporation
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

TEST_LIST=$(find /opt/igt/libexec/igt-gpu-tools -type f -printf "%f ")
cat << END
This is a igt-gpu-tools container:
 - igt_runner is in the \$PATH. In order for the results to be available on the
   host system, the directory has to be made available inside, e.g.
     docker run -v results:/tmp/results igt-final igt_runner /tmp/results
 - The test lists are in /opt/igt/share/igt-gpu-tools/
 - The test binaries are in IGT_TEST_ROOT=$IGT_TEST_ROOT

In order for the graphic devices to be available inside the container, those
either need to be mapped with --device or the container needs to be run in
--privileged mode.

Environment:
  - PATH=$PATH
  - LD_LIBRARY_PATH=$LD_LIBRARY_PATH
  - IGT_TEST_ROOT=$IGT_TEST_ROOT

Contents of /opt/igt/libexec/igt-gpu-tools: $TEST_LIST
END
