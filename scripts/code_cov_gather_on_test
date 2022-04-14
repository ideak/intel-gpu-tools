#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR MIT
#
# Copyright (C) 2022 Intel Corporation
#
# gather_on_test.py
#
# by Tomi Sarvela <tomi.p.sarvela@intel.com>
#
# Faster implementation for linux kernel GCOV data collection
# Command line compatible with original gather_on_test.sh
#
# Refs:
# https://www.kernel.org/doc/html/latest/dev-tools/gcov.html
#
import argparse
import errno
import io
import os
import sys
import tarfile

def parse_args() -> argparse.Namespace:
    '''Command line arguments'''
    ap = argparse.ArgumentParser(description="Gather Linux kernel GCOV data")
    ap.add_argument('output',
                    help="Output file name (.tar.gz will be added)")
    ap.add_argument('--gcov', default='/sys/kernel/debug/gcov',
                    help="GCOV directory, default: /sys/kernel/debug/gcov")
    return ap.parse_args()

def tar_add_link(tar:tarfile.TarFile, filename:str):
    '''Add filename as symlink to tarfile'''
    info = tarfile.TarInfo(filename)
    if info.name[0] == '/': info.name = info.name[1:]
    info.type = tarfile.SYMTYPE
    info.linkname = os.readlink(filename)
    tar.addfile(info)

def tar_add_file(tar:tarfile.TarFile, filename:str):
    '''Add filename to tarfile, file size expected to be invalid'''
    try:
        with open(filename, "rb") as fp:
            data = fp.read() # big gulp
    except OSError as e:
        print(f"ERROR: {filename}: {e}", file=sys.stderr)
        return
    info = tarfile.TarInfo(filename)
    if info.name[0] == '/': info.name = info.name[1:]
    info.size = len(data)
    tar.addfile(info, io.BytesIO(data))

def tar_add_tree(tar:tarfile.TarFile, tree:str):
    '''Add gcov files in directory tree to tar'''
    # FIXME: should dirs be added to tar for compatibility?
    for root, _, files in os.walk(tree, followlinks=False):
        for file in files:
            filepath = os.path.join(root, file)
            if file.endswith('.gcda'): tar_add_file(tar, filepath)
            if file.endswith('.gcno'): tar_add_link(tar, filepath)

def main() -> int:
    '''MAIN'''
    if not os.path.isdir(args.gcov):
        print(f"ERROR: [Errno {errno.ENOTDIR}] {os.strerror(errno.ENOTDIR)}: '{args.gcov}'",
              file=sys.stderr)
        return errno.ENOTDIR
    if args.output == '-':
        # reopen stdout as bytes for tarfile
        fp = os.fdopen(sys.stdout.fileno(), "wb", closefd=False)
    else:
        if not args.output.endswith('.tgz') and \
           not args.output.endswith('.tar.gz'):
            args.output+='.tar.gz'
        try:
            fp = open(args.output, 'wb')
        except OSError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return e.errno
    with tarfile.open(fileobj=fp, mode='w:gz') as tar:
        tar_add_tree(tar, args.gcov)
    fp.close()
    return 0

if __name__ == '__main__':
    try:
        args = parse_args()
        sys.exit(main())
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        sys.exit(errno.EINTR)
