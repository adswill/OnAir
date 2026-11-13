#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Fails if any `extern "C" fn` exported from rust/*/src has a narrow integer/bool/char parameter.
x86-64 SysV callers (GCC) do not extend narrow arguments to 32 bits but LLVM assumes they did, so
the Rust side would read garbage in the upper bits. Use u32/i32 parameters and narrow inside the body.
Return types may stay narrow. Usage: tools/check_ffi_widths.py [rust-dir]"""
import re, sys, glob, os
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rust')
NARROW = re.compile(r'^(?:core::ffi::|std::os::raw::|libc::)?(u8|u16|i8|i16|bool|c_char|c_schar|c_uchar|c_short|c_ushort)$')
bad = 0
for f in sorted(glob.glob(os.path.join(root, '*', 'src', '**', '*.rs'), recursive=True)):
    s = open(f, encoding='utf-8').read()
    for m in re.finditer(r'\bpub\s+(?:unsafe\s+)?extern\s+"C"\s+fn\s+(\w+)\s*(?:<[^>]*>)?\s*\((.*?)\)\s*(?:->[^{;]*)?[{;]', s, re.S):
        for p in re.split(r',(?![^()]*\))', m.group(2)):
            if ':' not in p:
                continue
            n, t = p.split(':', 1)
            if NARROW.match(t.strip()):
                print(f'{os.path.relpath(f, root)}: {m.group(1)}: parameter `{n.strip()}: {t.strip()}` is narrow; use u32/i32')
                bad += 1
if bad:
    print(f'check_ffi_widths: {bad} narrow extern "C" parameter(s)')
    sys.exit(1)
print('check_ffi_widths: ok')
