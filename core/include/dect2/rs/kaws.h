// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_kaws (KAWS: the HF weak-signal mode, 2.4 kHz). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Complex baseband samples are interleaved f32 (re, im) at 2400 samples/s (audio frequency = 1500 Hz + baseband frequency); bits are one per byte.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_kaws_version(void);

// Information bits k / coded block length n of profile id 0..7 (P0 Abyss .. P7 Voice-1200), or -1.
int32_t onair_kaws_profile_k(int32_t id);
int32_t onair_kaws_profile_n(int32_t id);

// CRC-16-CCITT (init 0xFFFF, "123456789" -> 0x29B1) and CRC-24 (OpenPGP, "123456789" -> 0x21CF02) of len bytes.
uint32_t onair_kaws_crc16(const uint8_t* data, int32_t len);
uint32_t onair_kaws_crc24(const uint8_t* data, int32_t len);

// The weave directions (1 rising, -1 falling) of the first count symbols of a block (16-bit Galois LFSR, seed 0xACE1). Returns count, or -1.
int32_t onair_kaws_weave(int32_t count, int32_t* out);

// Number of baseband samples of a burst (lattice units, data blocks, closing weave) of nblocks blocks of profile id, or -1.
int32_t onair_kaws_burst_len(int32_t id, int32_t nblocks);
// Build a burst: info = nblocks x k bits; iq receives 2 x burst_len floats (cap_samples is the capacity in samples). Mean power 1. Returns the samples, or -1.
int32_t onair_kaws_build_burst(int32_t id, int32_t nblocks, const uint8_t* info, float* iq, int32_t cap_samples);
// Receive a burst from n samples; the first lattice unit starts in [search_lo, search_hi]. out_info gets nblocks x k bits (0 where undecoded), out_iters
// nblocks values (LDPC iterations, -1 = not decoded), out_stats 4 doubles (detection statistic, timing in samples, frequency offset in Hz, 0).
// Returns 1 when the preamble was detected, 0 when not, -1 on a bad argument (the range is clamped so that a lattice unit fits in the stream).
int32_t onair_kaws_rx_burst(int32_t id, int32_t nblocks, const float* iq, int32_t n, int32_t search_lo, int32_t search_hi, uint8_t* out_info, int32_t* out_iters, double* out_stats);

// Watterson channel (channel 0 KW-AWGN, 1 KW-MOD 2 ms / 1 Hz, 2 KW-POOR 5 ms / 2 Hz) with frequency offset, drift (Hz/s), start delay (samples, may be
// fractional), noise-only tail and a 64-bit seed. iq_out needs n + start_delay + tail + 64 samples. SNR is in 2.4 kHz against the mean power of the input.
int32_t onair_kaws_channel(const float* iq, int32_t n, int32_t channel, double snr_db, double freq_offset_hz, double drift_hz_s, double start_delay, int32_t tail,
                           uint32_t seed_lo, uint32_t seed_hi, float* iq_out, int32_t cap_samples);

// Baseband to 12 kHz real audio (5 x n samples, largest sample = peak) and back (n / 5 samples). Return the output samples, or -1.
int32_t onair_kaws_to_audio(const float* iq, int32_t n, float peak, float* out, int32_t cap);
int32_t onair_kaws_from_audio(const float* audio, int32_t n, float* iq_out, int32_t cap_samples);

// One lattice unit (8400 samples, 3.5 s, mean power 1) into iq. Returns 8400, or -1.
int32_t onair_kaws_lattice_unit(float* iq, int32_t cap_samples);

#ifdef __cplusplus
}
#endif
