// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_ring (the lock-free SPSC sample ring behind common/ring.h). Written by hand; the Rust test
// rust/onair/tests/headers.rs checks it against ffi.rs. Only meaningful when DECT2_RUST_RING is defined (see rust/README.md).
// The caller owns the storage (cap elements of elem bytes, cap a power of two) and a header block of onair_ring_hdr_size() bytes aligned
// to onair_ring_hdr_align(); both stay at the same address while the ring is used. One producer thread (write, note_lost), one consumer
// thread (read, skip_to_newest, drop_backlog, clear); the other calls are safe from any thread.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bytes the C++ wrapper reserves for the header block (a Rust test checks that the real one fits).
#define ONAIR_RING_HDR_BYTES 640

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_ring_version(void);
// The header block: size and alignment in bytes.
size_t onair_ring_hdr_size(void);
size_t onair_ring_hdr_align(void);
// Builds an empty ring (start = initial value of both counters, 0 normally); 0 = ok, -1 = bad arguments.
int32_t onair_ring_init(uint8_t* hdr, uint8_t* buf, size_t cap, size_t elem, size_t start);
size_t onair_ring_capacity(const uint8_t* h);
// Producer: elements stored (the rest is counted as dropped); now_ns stamps the write (see last_write).
size_t onair_ring_write(const uint8_t* h, const uint8_t* src, size_t n, int64_t now_ns);
// Consumer: elements read.
size_t onair_ring_read(const uint8_t* h, uint8_t* dst, size_t n);
size_t onair_ring_skip_to_newest(const uint8_t* h, size_t keep);
size_t onair_ring_drop_backlog(const uint8_t* h);
size_t onair_ring_available(const uint8_t* h);
uint64_t onair_ring_dropped(const uint8_t* h);
void onair_ring_note_lost(const uint8_t* h, uint64_t n);
void onair_ring_clear(const uint8_t* h);
void onair_ring_reset_dropped(const uint8_t* h);
// 1 and the stamp (total offered, time of the last write, its size), 0 while nothing was written.
int32_t onair_ring_last_write(const uint8_t* h, uint64_t* offered, int64_t* ns, size_t* n);

#ifdef __cplusplus
}
#endif
