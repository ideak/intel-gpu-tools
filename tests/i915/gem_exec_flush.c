/*
 * Copyright © 2016 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <time.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_ring.h"
#include "igt.h"
#include "igt_x86.h"
/**
 * TEST: gem exec flush
 * Description: Basic check of flushing after batches
 * Run type: FULL
 *
 * SUBTEST: basic-batch-kernel-default-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: basic-batch-kernel-default-uc
 * Feature: cmd_submission
 *
 * SUBTEST: basic-batch-kernel-default-wb
 * Feature: cmd_submission
 *
 * SUBTEST: basic-uc-pro-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-uc-prw-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-uc-ro-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-uc-rw-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-uc-set-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-pro-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-prw-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-ro-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-ro-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-rw-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-rw-default
 * Feature: cmd_submission
 *
 * SUBTEST: basic-wb-set-default
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-blt-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-blt-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-blt-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-bsd-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd1-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-bsd1-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd1-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd2-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-bsd2-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-bsd2-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-default-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-default-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-default-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-render-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-render-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-render-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-vebox-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-cpu-vebox-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-cpu-vebox-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-blt-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-blt-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-blt-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-bsd-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd1-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-bsd1-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd1-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd2-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-bsd2-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-bsd2-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-default-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-default-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-default-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-render-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-render-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-render-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-vebox-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-gtt-vebox-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-gtt-vebox-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-blt-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-blt-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-blt-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-bsd-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd1-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-bsd1-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd1-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd2-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-bsd2-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-bsd2-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-render-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-render-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-render-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-vebox-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-kernel-vebox-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-kernel-vebox-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-blt-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-blt-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-blt-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-bsd-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd1-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-bsd1-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd1-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd2-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-bsd2-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-bsd2-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-default-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-default-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-default-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-render-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-render-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-render-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-vebox-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-user-vebox-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-user-vebox-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-blt-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-blt-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-blt-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-bsd-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd1-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-bsd1-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd1-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd2-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-bsd2-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-bsd2-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-default-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-default-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-default-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-render-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-render-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-render-wb
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-vebox-cmd
 * Feature: cmd_submission, command_parser
 *
 * SUBTEST: batch-wc-vebox-uc
 * Feature: cmd_submission
 *
 * SUBTEST: batch-wc-vebox-wb
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-pro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-prw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-ro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-rw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-blt
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-default
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-render
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: stream-set-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-pro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-prw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-ro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-rw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-blt
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-render
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: uc-set-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-pro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-prw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-ro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-rw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-render
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wb-set-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-pro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-prw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-ro-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-before-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-rw-vebox-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-blt
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-blt-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd1
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd1-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd2
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-bsd2-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-default
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-default-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-render
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-render-interruptible
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-vebox
 * Feature: cmd_submission
 *
 * SUBTEST: wc-set-vebox-interruptible
 * Feature: cmd_submission
 */

IGT_TEST_DESCRIPTION("Basic check of flushing after batches");

#define UNCACHED 0
#define COHERENT 1
#define WC 2
#define WRITE 4
#define KERNEL 8
#define SET_DOMAIN 16
#define BEFORE 32
#define INTERRUPTIBLE 64
#define CMDPARSER 128
#define BASIC 256
#define MOVNT 512

#if defined(__x86_64__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC target("sse4.1")
#include <smmintrin.h>
__attribute__((noinline))
static uint32_t movnt(uint32_t *map, int i)
{
	__m128i tmp;

	tmp = _mm_stream_load_si128((__m128i *)map + i/4);
	switch (i%4) { /* gcc! */
	default:
	case 0: return _mm_extract_epi32(tmp, 0);
	case 1: return _mm_extract_epi32(tmp, 1);
	case 2: return _mm_extract_epi32(tmp, 2);
	case 3: return _mm_extract_epi32(tmp, 3);
	}
}
static inline unsigned x86_64_features(void)
{
	return igt_x86_features();
}
#pragma GCC pop_options
#else
static inline unsigned x86_64_features(void)
{
	return 0;
}
static uint32_t movnt(uint32_t *map, int i)
{
	igt_assert(!"reached");
}
#endif

static void run(int fd, unsigned ring, int nchild, int timeout,
		unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));

	/* The crux of this testing is whether writes by the GPU are coherent
	 * from the CPU.
	 *
	 * For example, using plain clflush (the simplest and most visible
	 * in terms of function calls / syscalls) we have two tests which
	 * perform:
	 *
	 * USER (0):
	 *	execbuf(map[i] = i);
	 *	sync();
	 *	clflush(&map[i]);
	 *	assert(map[i] == i);
	 *
	 *	execbuf(map[i] = i ^ ~0);
	 *	sync();
	 *	clflush(&map[i]);
	 *	assert(map[i] == i ^ ~0);
	 *
	 * BEFORE:
	 *	clflush(&map[i]);
	 *	execbuf(map[i] = i);
	 *	sync();
	 *	assert(map[i] == i);
	 *
	 *	clflush(&map[i]);
	 *	execbuf(map[i] = i ^ ~0);
	 *	sync();
	 *	assert(map[i] == i ^ ~0);
	 *
	 * The assertion here is that the cacheline invalidations are precise
	 * and we have no speculative prefetch that can see the future map[i]
	 * access and bring it ahead of the execution, or accidental cache
	 * pollution by the kernel.
	 */

	igt_fork(child, nchild) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_relocation_entry reloc0[1024];
		struct drm_i915_gem_relocation_entry reloc1[1024];
		struct drm_i915_gem_execbuffer2 execbuf;
		unsigned long cycles = 0;
		bool snoop = false;
		uint32_t *ptr;
		uint32_t *map;
		int i;
		bool has_relocs = gem_has_relocations(fd);

		memset(obj, 0, sizeof(obj));
		obj[0].handle = gem_create(fd, 4096);
		obj[0].flags |= EXEC_OBJECT_WRITE;

		if (flags & WC) {
			igt_assert(flags & COHERENT);
			map = gem_mmap__wc(fd, obj[0].handle, 0, 4096, PROT_WRITE);
			gem_set_domain(fd, obj[0].handle,
				       I915_GEM_DOMAIN_WC,
				       I915_GEM_DOMAIN_WC);
		} else {
			snoop = flags & COHERENT;
			if (igt_has_set_caching(intel_get_drm_devid(fd)))
				gem_set_caching(fd, obj[0].handle, snoop);
			map = gem_mmap__cpu(fd, obj[0].handle, 0, 4096, PROT_WRITE);
			gem_set_domain(fd, obj[0].handle,
				       I915_GEM_DOMAIN_CPU,
				       I915_GEM_DOMAIN_CPU);
		}

		for (i = 0; i < 1024; i++)
			map[i] = 0xabcdabcd;

		gem_set_domain(fd, obj[0].handle,
			       I915_GEM_DOMAIN_WC,
			       I915_GEM_DOMAIN_WC);

		/* Prepare a mappable binding to prevent pread migrating */
		if (!snoop) {
			ptr = gem_mmap__device_coherent(fd, obj[0].handle, 0,
							4096, PROT_READ);
			igt_assert_eq_u32(ptr[0], 0xabcdabcd);
			munmap(ptr, 4096);
		}

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = 3;
		execbuf.flags = ring | (1 << 12);
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		obj[1].handle = gem_create(fd, 1024*64);
		obj[2].handle = gem_create(fd, 1024*64);
		gem_write(fd, obj[2].handle, 0, &bbe, sizeof(bbe));
		igt_require(__gem_execbuf(fd, &execbuf) == 0);

		if (has_relocs) {
			obj[1].relocation_count = 1;
			obj[2].relocation_count = 1;
		} else {
			/*
			 * For gens without relocations we already have
			 * objects in appropriate place of gtt as warming
			 * execbuf pins them so just set EXEC_OBJECT_PINNED
			 * flag.
			 */
			obj[0].flags |= EXEC_OBJECT_PINNED;
			obj[1].flags |= EXEC_OBJECT_PINNED;
			obj[2].flags |= EXEC_OBJECT_PINNED;
		}

		ptr = gem_mmap__wc(fd, obj[1].handle, 0, 64*1024,
				   PROT_WRITE | PROT_READ);
		gem_set_domain(fd, obj[1].handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

		memset(reloc0, 0, sizeof(reloc0));
		for (i = 0; i < 1024; i++) {
			uint64_t offset;
			uint32_t *b = &ptr[16 * i];

			reloc0[i].presumed_offset = obj[0].offset;
			reloc0[i].offset = (b - ptr + 1) * sizeof(*ptr);
			reloc0[i].delta = i * sizeof(uint32_t);
			reloc0[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc0[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = obj[0].offset + reloc0[i].delta;
			*b++ = MI_STORE_DWORD_IMM_GEN4 | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc0[i].offset += sizeof(*ptr);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i;
			*b++ = MI_BATCH_BUFFER_END;
		}
		munmap(ptr, 64*1024);

		ptr = gem_mmap__wc(fd, obj[2].handle, 0, 64*1024,
				   PROT_WRITE | PROT_READ);
		gem_set_domain(fd, obj[2].handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

		memset(reloc1, 0, sizeof(reloc1));
		for (i = 0; i < 1024; i++) {
			uint64_t offset;
			uint32_t *b = &ptr[16 * i];

			reloc1[i].presumed_offset = obj[0].offset;
			reloc1[i].offset = (b - ptr + 1) * sizeof(*ptr);
			reloc1[i].delta = i * sizeof(uint32_t);
			reloc1[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc1[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = obj[0].offset + reloc1[i].delta;
			*b++ = MI_STORE_DWORD_IMM_GEN4 | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc1[i].offset += sizeof(*ptr);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i ^ 0xffffffff;
			*b++ = MI_BATCH_BUFFER_END;
		}
		munmap(ptr, 64*1024);

		igt_until_timeout(timeout) {
			bool xor = false;
			int idx = cycles++ % 1024;

			/* Inspect a different cacheline each iteration */
			i = 16 * (idx % 64) + (idx / 64);
			obj[1].relocs_ptr = to_user_pointer(&reloc0[i]);
			obj[2].relocs_ptr = to_user_pointer(&reloc1[i]);
			execbuf.batch_start_offset = 64 * i;

overwrite:
			if ((flags & BEFORE) &&
			    !((flags & COHERENT) || gem_has_llc(fd)))
				igt_clflush_range(&map[i], sizeof(map[i]));

			execbuf.buffer_count = 2 + xor;
			gem_execbuf(fd, &execbuf);

			if (flags & SET_DOMAIN) {
				unsigned domain = flags & WC ? I915_GEM_DOMAIN_WC : I915_GEM_DOMAIN_CPU;
				igt_while_interruptible(flags & INTERRUPTIBLE)
					gem_set_domain(fd, obj[0].handle,
						       domain, (flags & WRITE) ? domain : 0);

				if (xor)
					igt_assert_eq_u32(map[i], i ^ 0xffffffff);
				else
					igt_assert_eq_u32(map[i], i);

				if (flags & WRITE)
					map[i] = 0xdeadbeef;
			} else if (flags & KERNEL) {
				uint32_t val;

				igt_while_interruptible(flags & INTERRUPTIBLE)
					gem_read(fd, obj[0].handle,
						 i*sizeof(uint32_t),
						 &val, sizeof(val));

				if (xor)
					igt_assert_eq_u32(val, i ^ 0xffffffff);
				else
					igt_assert_eq_u32(val, i);

				if (flags & WRITE) {
					val = 0xdeadbeef;
					igt_while_interruptible(flags & INTERRUPTIBLE)
						gem_write(fd, obj[0].handle,
							  i*sizeof(uint32_t),
							  &val, sizeof(val));
				}
			} else if (flags & MOVNT) {
				uint32_t x;

				igt_while_interruptible(flags & INTERRUPTIBLE)
					gem_sync(fd, obj[0].handle);

				x = movnt(map, i);
				if (xor)
					igt_assert_eq_u32(x, i ^ 0xffffffff);
				else
					igt_assert_eq_u32(x, i);

				if (flags & WRITE)
					map[i] = 0xdeadbeef;
			} else {
				igt_while_interruptible(flags & INTERRUPTIBLE)
					gem_sync(fd, obj[0].handle);

				if (!(flags & (BEFORE | COHERENT)) &&
				    !gem_has_llc(fd))
					igt_clflush_range(&map[i], sizeof(map[i]));

				if (xor)
					igt_assert_eq_u32(map[i], i ^ 0xffffffff);
				else
					igt_assert_eq_u32(map[i], i);

				if (flags & WRITE) {
					map[i] = 0xdeadbeef;
					if (!(flags & (COHERENT | BEFORE)))
						igt_clflush_range(&map[i], sizeof(map[i]));
				}
			}

			if (!xor) {
				xor= true;
				goto overwrite;
			}
		}
		igt_info("Child[%d]: %lu cycles\n", child, cycles);

		gem_close(fd, obj[2].handle);
		gem_close(fd, obj[1].handle);

		munmap(map, 4096);
		gem_close(fd, obj[0].handle);
	}
	igt_waitchildren();
}

enum batch_mode {
	BATCH_KERNEL,
	BATCH_USER,
	BATCH_CPU,
	BATCH_GTT,
	BATCH_WC,
};
static void batch(int fd, unsigned ring, int nchild, int timeout,
		  enum batch_mode mode, unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));

	if (mode == BATCH_GTT)
		gem_require_mappable_ggtt(fd);

	if (flags & CMDPARSER) {
		int cmdparser = -1;
                drm_i915_getparam_t gp;

		gp.param = I915_PARAM_CMD_PARSER_VERSION;
		gp.value = &cmdparser;
		drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
		igt_require(cmdparser > 0);
	}

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, nchild) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 obj[2];
		struct drm_i915_gem_relocation_entry reloc;
		struct drm_i915_gem_execbuffer2 execbuf;
		unsigned long cycles = 0;
		uint32_t *ptr;
		uint32_t *map;
		int i;
		bool has_relocs = gem_has_relocations(fd);

		memset(obj, 0, sizeof(obj));
		obj[0].handle = gem_create(fd, 4096);
		obj[0].flags |= EXEC_OBJECT_WRITE;

		if (igt_has_set_caching(intel_get_drm_devid(fd)))
			gem_set_caching(fd, obj[0].handle, !!(flags & COHERENT));
		map = gem_mmap__cpu(fd, obj[0].handle, 0, 4096, PROT_WRITE);

		gem_set_domain(fd, obj[0].handle,
				I915_GEM_DOMAIN_CPU,
				I915_GEM_DOMAIN_CPU);
		for (i = 0; i < 1024; i++)
			map[i] = 0xabcdabcd;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = 2;
		execbuf.flags = ring | (1 << 12);
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		obj[1].handle = gem_create(fd, 64<<10);
		gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));
		igt_require(__gem_execbuf(fd, &execbuf) == 0);

		if (!has_relocs) {
			obj[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
			obj[1].flags |= EXEC_OBJECT_PINNED;
		}
		obj[1].relocation_count = has_relocs ? 1 : 0;
		obj[1].relocs_ptr = to_user_pointer(&reloc);

		switch (mode) {
		case BATCH_CPU:
		case BATCH_USER:
			ptr = gem_mmap__cpu(fd, obj[1].handle, 0, 64<<10,
					    PROT_WRITE);
			break;

		case BATCH_WC:
			ptr = gem_mmap__wc(fd, obj[1].handle, 0, 64<<10,
					    PROT_WRITE);
			break;

		case BATCH_GTT:
			ptr = gem_mmap__gtt(fd, obj[1].handle, 64<<10,
					    PROT_WRITE);
			break;

		case BATCH_KERNEL:
			ptr = mmap(0, 64<<10, PROT_WRITE,
				   MAP_PRIVATE | MAP_ANON, -1, 0);
			break;

		default:
			igt_assert(!"reachable");
			ptr = NULL;
			break;
		}

		memset(&reloc, 0, sizeof(reloc));
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		igt_until_timeout(timeout) {
			execbuf.batch_start_offset = 0;
			reloc.offset = sizeof(uint32_t);
			if (gen >= 4 && gen < 8)
				reloc.offset += sizeof(uint32_t);

			for (i = 0; i < 1024; i++) {
				uint64_t offset;
				uint32_t *start = &ptr[execbuf.batch_start_offset/sizeof(*start)];
				uint32_t *b = start;

				switch (mode) {
				case BATCH_CPU:
					gem_set_domain(fd, obj[1].handle,
						       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
					break;

				case BATCH_WC:
					gem_set_domain(fd, obj[1].handle,
						       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
					break;

				case BATCH_GTT:
					gem_set_domain(fd, obj[1].handle,
						       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
					break;

				case BATCH_USER:
				case BATCH_KERNEL:
					break;
				}

				reloc.presumed_offset = obj[0].offset;
				reloc.delta = i * sizeof(uint32_t);

				offset = reloc.presumed_offset + reloc.delta;
				*b++ = MI_STORE_DWORD_IMM_GEN4 | (gen < 6 ? 1 << 22 : 0);
				if (gen >= 8) {
					*b++ = offset;
					*b++ = offset >> 32;
				} else if (gen >= 4) {
					*b++ = 0;
					*b++ = offset;
				} else {
					b[-1] -= 1;
					*b++ = offset;
				}
				*b++ = cycles + i;
				*b++ = MI_BATCH_BUFFER_END;

				if (flags & CMDPARSER) {
					execbuf.batch_len =
						(b - start) * sizeof(uint32_t);
					if (execbuf.batch_len & 4)
						execbuf.batch_len += 4;
				}

				switch (mode) {
				case BATCH_KERNEL:
					gem_write(fd, obj[1].handle,
						  execbuf.batch_start_offset,
						  start, (b - start) * sizeof(uint32_t));
					break;

				case BATCH_USER:
					if (!gem_has_llc(fd))
						igt_clflush_range(start,
								  (b - start) * sizeof(uint32_t));
					break;

				case BATCH_CPU:
				case BATCH_GTT:
				case BATCH_WC:
					break;
				}
				gem_execbuf(fd, &execbuf);

				execbuf.batch_start_offset += 64;
				reloc.offset += 64;
			}

			if (!(flags & COHERENT)) {
				gem_set_domain(fd, obj[0].handle,
					       I915_GEM_DOMAIN_CPU,
					       I915_GEM_DOMAIN_CPU);
			} else
				gem_sync(fd, obj[0].handle);
			for (i = 0; i < 1024; i++) {
				igt_assert_eq_u32(map[i], cycles + i);
				map[i] = 0xabcdabcd ^ cycles;
			}
			cycles += 1024;

			if (mode == BATCH_USER)
				gem_sync(fd, obj[1].handle);
		}
		igt_info("Child[%d]: %lu cycles\n", child, cycles);

		munmap(ptr, 64<<10);
		gem_close(fd, obj[1].handle);

		munmap(map, 4096);
		gem_close(fd, obj[0].handle);
	}
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static const char *yesno(bool x)
{
	return x ? "yes" : "no";
}

igt_main
{
	const struct intel_execution_ring *e;
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct batch {
		const char *name;
		unsigned mode;
	} batches[] = {
		{ "kernel", BATCH_KERNEL },
		{ "user", BATCH_USER },
		{ "cpu", BATCH_CPU },
		{ "gtt", BATCH_GTT },
		{ "wc", BATCH_WC },
		{ NULL }
	};
	const struct mode {
		const char *name;
		unsigned flags;
	} modes[] = {
		{ "ro", BASIC },
		{ "rw", BASIC | WRITE },
		{ "ro-before", BEFORE },
		{ "rw-before", BEFORE | WRITE },
		{ "pro", BASIC | KERNEL },
		{ "prw", BASIC | KERNEL | WRITE },
		{ "set", BASIC | SET_DOMAIN | WRITE },
		{ NULL }
	};
	unsigned cpu = x86_64_features();
	int fd = -1;

	igt_fixture {
		igt_require(igt_setup_clflush());
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		igt_require(gem_can_store_dword(fd, 0));
		igt_info("Has LLC? %s\n", yesno(gem_has_llc(fd)));

		if (cpu) {
			char str[1024];

			igt_info("CPU features: %s\n",
				 igt_x86_features_to_string(cpu, str));
		}

		igt_fork_hang_detector(fd);
	}

	for (e = intel_execution_rings; e->name; e++) igt_subtest_group {
		unsigned ring = eb_ring(e);
		unsigned timeout = 5 + 120*!!e->exec_id;

		igt_fixture {
			gem_require_ring(fd, ring);
			igt_require(gem_can_store_dword(fd, ring));
		}

		for (const struct batch *b = batches; b->name; b++) {
			igt_subtest_f("%sbatch-%s-%s-uc",
				      b == batches && e->exec_id == 0 ? "basic-" : "",
				      b->name,
				      e->name)
				batch(fd, ring, ncpus, timeout, b->mode, 0);
			igt_subtest_f("%sbatch-%s-%s-wb",
				      b == batches && e->exec_id == 0 ? "basic-" : "",
				      b->name,
				      e->name)
				batch(fd, ring, ncpus, timeout, b->mode, COHERENT);
			igt_subtest_f("%sbatch-%s-%s-cmd",
				      b == batches && e->exec_id == 0 ? "basic-" : "",
				      b->name,
				      e->name)
				batch(fd, ring, ncpus, timeout, b->mode,
				      COHERENT | CMDPARSER);
		}

		for (const struct mode *m = modes; m->name; m++) {
			igt_subtest_f("%suc-%s-%s",
				      (m->flags & BASIC && e->exec_id == 0) ? "basic-" : "",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    UNCACHED | m->flags);

			igt_subtest_f("uc-%s-%s-interruptible",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    UNCACHED | m->flags | INTERRUPTIBLE);

			igt_subtest_f("%swb-%s-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    COHERENT | m->flags);

			igt_subtest_f("wb-%s-%s-interruptible",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    COHERENT | m->flags | INTERRUPTIBLE);

			igt_subtest_f("wc-%s-%s",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    COHERENT | WC | m->flags);

			igt_subtest_f("wc-%s-%s-interruptible",
				      m->name,
				      e->name)
				run(fd, ring, ncpus, timeout,
				    COHERENT | WC | m->flags | INTERRUPTIBLE);

			igt_subtest_f("stream-%s-%s",
				      m->name,
				      e->name) {
				igt_require(cpu & SSE4_1);
				run(fd, ring, ncpus, timeout,
				    MOVNT | COHERENT | WC | m->flags);
			}

			igt_subtest_f("stream-%s-%s-interruptible",
				      m->name,
				      e->name) {
				igt_require(cpu & SSE4_1);
				run(fd, ring, ncpus, timeout,
				    MOVNT | COHERENT | WC | m->flags | INTERRUPTIBLE);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
