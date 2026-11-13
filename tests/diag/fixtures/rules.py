# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
# Fixture for tools/lint/diag_check.py --self-test (Python call forms).
def diag(code, msg, *args):   # a definition is not a call site
    pass

def fixture(path):
    diag(482917, "The CTest timing file %s is missing, so the shards are split by test count instead.", path)
    diag(482913, f"The cache folder {path} is not writable, so every run starts from an empty cache.")
    diag(482919, 'Single quotes make one literal too, and this message is a full sentence.')
    diag(482913, "This line reuses a code that the second call site above already has.")   # expect: dup-code
    diag(482921, "The cache " + path + " is joined with a plus sign, which the rule forbids.")   # expect: literal
    obj.diag(100000, "a method of another object is not a call site")
