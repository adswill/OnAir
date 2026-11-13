// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// Fixture for tools/lint/diag_check.py --self-test: each line that must be reported says which rule in an "expect:" comment.
#include "dect2/common/diag.h"

void fixture(const char* path, int n, const std::string& tail) {
    DIAG(735289, "The fixture file %s could not be opened, so the check goes on without it.", path);
    DIAG_DEBUG(735281, "The fixture reader skipped %d bytes of padding before the first record.", n);
    DIAG(131072, "This code is a power of two and must be rejected by the number rules.");   // expect: code
    DIAG(735280, "This code ends in a zero digit and must be rejected by the number rules.");   // expect: code
    DIAG(777777, "This code repeats one digit six times and must be rejected by the rules.");   // expect: code
    DIAG(131137, "This code has only three bits set and must be rejected by the number rules.");   // expect: code
    DIAG(73528, "This code has five digits only and must be rejected by the number rules.");   // expect: code
    DIAG(735000 + 289, "This code is built from parts and must be rejected by the number rules.");   // expect: code
    DIAG(735289, "This line reuses the code of the first call site and must be reported.");   // expect: dup-code
    DIAG(642817, "The fixture file %s could not be opened, so the check goes on without it.", path);   // expect: dup-text
    DIAG(642813, "Failed.");   // expect: generic
    DIAG(642819, "too short a message.");   // expect: sentence
    DIAG(642831, "the message starts with a small letter and that is not a sentence.");   // expect: sentence
    DIAG(642837, "The message has no full stop at its end and so it is not a sentence");   // expect: sentence
    DIAG(642839, "The message is " "joined from two literals, which hides the text from a search.");   // expect: literal
    DIAG(642851, tail);   // expect: literal
    DIAG_EVERY(642857, 10.0, "The fixture reader saw %d damaged records in the last ten seconds.", n);
    DIAG_EVERY(642859, 10.0, tail);   // expect: literal
    std::string e = DIAG_TEXT(642861, "The fixture output %s is read-only, so nothing is written there.", path);
    std::string u = DIAG_UI(642863, tr("The translated fixture message stays outside the translation key."));
    for (int i = 0; i < n; i++) {
        DIAG(642869, "The fixture loop hit record %d, which a real decoder would count instead.", i);
    }
    // DIAG(100000, "a commented-out call is not a call site")
    const char* s = "DIAG(100000, \"a call inside a string is not a call site\")";
    DIAG(   // expect: code
         642871, "The code on the next line cannot be found with one search for the call.");
}
