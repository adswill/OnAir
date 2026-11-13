// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fixed-width integer packing helpers: big-endian (network order) and little-endian writers and readers,
// for byte buffers and for std::vector<uint8_t>. Replaces the dozens of per-file put16/put32/be32/le32 copies.
// "put" appends to a vector, "store" writes through a pointer, "load" reads from a pointer; Be = most
// significant byte first, Le = least significant byte first. All are inline and header-only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace endian {

// ---- readers ----
// 16-bit big-endian read (network order).
inline uint16_t loadBe16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
// 32-bit big-endian read.
inline uint32_t loadBe32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
// 64-bit big-endian read.
inline uint64_t loadBe64(const uint8_t* p) { return ((uint64_t)loadBe32(p) << 32) | loadBe32(p + 4); }
// 16-bit little-endian read.
inline uint16_t loadLe16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
// 24-bit little-endian read.
inline uint32_t loadLe24(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
// 32-bit little-endian read.
inline uint32_t loadLe32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
// 64-bit little-endian read.
inline uint64_t loadLe64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

// ---- writers through a pointer ----
// 16-bit big-endian store.
inline void storeBe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
// 32-bit big-endian store.
inline void storeBe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
// 64-bit big-endian store.
inline void storeBe64(uint8_t* p, uint64_t v) { storeBe32(p, (uint32_t)(v >> 32)); storeBe32(p + 4, (uint32_t)v); }
// 16-bit little-endian store.
inline void storeLe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
// 32-bit little-endian store.
inline void storeLe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// ---- appenders to a byte vector ----
// Append 16 bits big-endian.
inline void putBe16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
// Append 32 bits big-endian.
inline void putBe32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
// Append 64 bits big-endian.
inline void putBe64(std::vector<uint8_t>& v, uint64_t x) { putBe32(v, (uint32_t)(x >> 32)); putBe32(v, (uint32_t)x); }
// Append 16 bits little-endian.
inline void putLe16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
// Append 32 bits little-endian.
inline void putLe32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
// Append 64 bits little-endian.
inline void putLe64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i))); }

} // namespace endian
} // namespace dect2
