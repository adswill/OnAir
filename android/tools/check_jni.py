#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Compares the `external fun` declarations of the Kotlin JNI objects with their C++ functions (names, parameter and return types):
OnAirNative.kt <-> native/src/jni_onair.cpp (macro FN), usb/RadioNative.kt <-> native/src/jni_radio.cpp (macro RFN),
tv/TvNative.kt <-> native/src/jni_tv.cpp (macro TFN)."""
import re, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K = {'Int': 'jint', 'Long': 'jlong', 'Double': 'jdouble', 'Float': 'jfloat', 'Boolean': 'jboolean', 'String': 'jstring', 'FloatArray': 'jfloatArray',
     'DoubleArray': 'jdoubleArray', 'ShortArray': 'jshortArray', 'ByteArray': 'jbyteArray', 'IntArray': 'jintArray', 'Unit': 'void'}
PAIRS = [
    ('app/src/main/java/io/github/adswill/onair/OnAirNative.kt', 'native/src/jni_onair.cpp', 'FN'),
    ('app/src/main/java/io/github/adswill/onair/usb/RadioNative.kt', 'native/src/jni_radio.cpp', 'RFN'),
    ('app/src/main/java/io/github/adswill/onair/tv/TvNative.kt', 'native/src/jni_tv.cpp', 'TFN'),
]
bad = 0
for ktf, ccf, macro in PAIRS:
    kt = open(f'{root}/{ktf}').read()
    cc = open(f'{root}/{ccf}').read()
    kts = {}
    for m in re.finditer(r'external fun (\w+)\((.*?)\)(?:\s*:\s*(\w+))?\s*$', kt, re.M | re.S):
        name, params, ret = m.group(1), m.group(2), m.group(3) or 'Unit'
        ps = [K[p.split(':')[1].strip()] for p in params.replace('\n', ' ').split(',') if p.strip()]
        kts[name] = (K[ret], ps)
    ccs = {}
    for m in re.finditer(macro + r'\((\w+), (\w+)\)\(JNIEnv\*(?: e)?, jobject(.*?)\)\s*\{', cc, re.S):
        ret, name, params = m.groups()
        ps = [p.strip().split()[0] for p in params.replace('\n', ' ').split(',') if p.strip()]
        ccs[name] = (ret, ps)
    for n in sorted(set(kts) | set(ccs)):
        if n not in kts: print(f'{ktf}: only in C++:', n); bad += 1
        elif n not in ccs: print(f'{ccf}: only in Kotlin:', n); bad += 1
        elif kts[n] != ccs[n]: print('MISMATCH', n, kts[n], ccs[n]); bad += 1
    print(f'{os.path.basename(ktf)}: checked', len(kts), 'Kotlin and', len(ccs), 'C++ functions')
print(bad, 'problems')
sys.exit(1 if bad else 0)
