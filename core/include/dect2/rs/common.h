// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of rust/onair_common/src/diag.rs: where the Rust diagnostic codes (docs/DIAGNOSTICS.md) go. Written by hand.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once at startup: every Rust diagnostic goes to emit(level, code, text, len) (level 0 debug, 1 warning, 2 error; the text has no [E######]
// prefix) and debug() returns non-zero while debug diagnostics are on. Passing null for emit sends the lines back to standard error.
void onair_diag_set_sink(void (*emit)(int32_t level, int32_t code, const uint8_t* text, size_t len), int32_t (*debug)(void));

#ifdef __cplusplus
}
#endif
