// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bit-level helpers shared by the protocol decoders: popcount/parity, Gray code, bit reversal,
// and readers/writers for "unpacked" bit vectors (one bit per byte, only bit 0 significant).
// MSB-first means the first element is the most significant bit of the field (the usual air order);
// LSB-first is the opposite (BLE, 802.11ah, MAC). All functions are inline and header-only.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace bits {

// Number of set bits (Hamming weight); C++20 std::popcount. The x86-64 baseline has no POPCNT instruction, so std::popcount there is a
// libgcc call (a byte table): the bit-parallel count is a few integer operations instead (the same count). Sync-word searches call it per bit.
#if defined(__x86_64__) && !defined(__POPCNT__)
inline int popcount(uint32_t v) { v = v - ((v >> 1) & 0x55555555u); v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u); return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24); }
inline int popcount(uint64_t v) {
    v = v - ((v >> 1) & 0x5555555555555555ull); v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    return (int)((((v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full) * 0x0101010101010101ull) >> 56);
}
#else
inline int popcount(uint32_t v) { return std::popcount(v); }
inline int popcount(uint64_t v) { return std::popcount(v); }
#endif
// Parity (xor of all bits): 1 when the number of set bits is odd.
inline int parity(uint32_t v) { return std::popcount(v) & 1; }
inline int parity(uint64_t v) { return std::popcount(v) & 1; }
// Parity of the low n bits of v (n <= 32).
inline int parityLow(uint32_t v, int n) { return std::popcount(n >= 32 ? v : (v & ((1u << n) - 1u))) & 1; }

// Binary-reflected Gray code of x (IEEE / ITU-T usage in LoRa, DVB constellations, GFSK).
inline uint32_t gray(uint32_t x) { return x ^ (x >> 1); }
// Inverse Gray code: the binary value whose Gray code is g.
inline uint32_t grayInverse(uint32_t g) { uint32_t x = g; for (uint32_t s = g >> 1; s; s >>= 1) x ^= s; return x; }

// Reverse the order of the 8 bits of a byte (MSB<->LSB).
inline uint8_t reverse8(uint8_t v) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (v >> i & 1) r |= (uint8_t)(0x80 >> i); return r; }
// Reverse the low n bits of v.
inline uint32_t reverseN(uint32_t v, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) if (v >> i & 1) r |= 1u << (n - 1 - i); return r; }

// Read n unpacked bits MSB-first starting at b[pos]; each element contributes its bit 0.
inline uint64_t getMsb(const uint8_t* b, size_t pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[pos + (size_t)i] & 1u); return v; }
// Same, advancing pos past the field.
inline uint64_t getMsbAdv(const uint8_t* b, int& pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[pos++] & 1u); return v; }
// Read n unpacked bits LSB-first starting at b[pos].
inline uint64_t getLsb(const uint8_t* b, size_t pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v |= (uint64_t)(b[pos + (size_t)i] & 1u) << i; return v; }
// Same without masking to bit 0: for buffers whose elements are already exactly 0 or 1 (kept to preserve the original arithmetic).
inline uint64_t getMsbRaw(const uint8_t* b, size_t pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[pos + (size_t)i]; return v; }
inline uint64_t getMsbAdvRaw(const uint8_t* b, int& pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[pos++]; return v; }
// Read n bits MSB-first from PACKED bytes starting at bit offset pos (bit 0 = MSB of byte 0): ADS-B/DAB field extraction.
inline uint64_t getPackedMsb(const uint8_t* d, size_t pos, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) { const size_t b = pos + (size_t)i; v = (v << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1u); } return v; }
// Write the low n bits of v MSB-first to b[pos..pos+n).
inline void putMsb(uint8_t* b, size_t pos, uint64_t v, int n) { for (int i = 0; i < n; i++) b[pos + (size_t)i] = (uint8_t)((v >> (n - 1 - i)) & 1u); }
// Same, advancing pos past the field.
inline void putMsbAdv(uint8_t* b, int& pos, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b[pos++] = (uint8_t)((v >> i) & 1u); }
// Append the low n bits of v MSB-first to a bit vector.
inline void pushMsb(std::vector<uint8_t>& out, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) out.push_back((uint8_t)((v >> i) & 1u)); }
// Write the low n bits of v LSB-first to b[pos..pos+n).
inline void putLsb(uint8_t* b, size_t pos, uint64_t v, int n) { for (int i = 0; i < n; i++) b[pos + (size_t)i] = (uint8_t)((v >> i) & 1u); }

// Pack whole bytes into unpacked bits, MSB-first (bytes[0] bit 7 first); out must hold 8*n entries.
inline void bytesToBitsMsb(const uint8_t* bytes, size_t n, uint8_t* out) { for (size_t i = 0; i < n; i++) for (int k = 0; k < 8; k++) out[i * 8 + (size_t)k] = (bytes[i] >> (7 - k)) & 1; }
// Same, LSB-first within each byte.
inline void bytesToBitsLsb(const uint8_t* bytes, size_t n, uint8_t* out) { for (size_t i = 0; i < n; i++) for (int k = 0; k < 8; k++) out[i * 8 + (size_t)k] = (bytes[i] >> k) & 1; }
// Pack n whole bytes from unpacked bits, MSB-first (any non-zero bit-0 counts as one).
inline void bitsToBytesMsb(const uint8_t* b, size_t n, uint8_t* out) { for (size_t i = 0; i < n; i++) { out[i] = 0; for (int k = 0; k < 8; k++) out[i] = (uint8_t)((out[i] << 1) | (b[i * 8 + (size_t)k] & 1)); } }
// Same, LSB-first within each byte.
inline void bitsToBytesLsb(const uint8_t* b, size_t n, uint8_t* out) { for (size_t i = 0; i < n; i++) { out[i] = 0; for (int k = 0; k < 8; k++) out[i] |= (uint8_t)((b[i * 8 + (size_t)k] & 1) << k); } }

} // namespace bits
} // namespace dect2
