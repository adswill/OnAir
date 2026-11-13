// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared CRC engines and named presets, replacing the per-protocol copies.
// Byte-oriented functions use constexpr 256-entry tables built at compile time (hot paths stay fast);
// the bit-serial engine (one bit per byte, any width up to 32) serves the air-interface CRCs
// that are computed over unpacked bit vectors. Polynomials are given in normal (non-reflected) form
// unless a name says Lsb, where the reflected polynomial is used (e.g. 0xEDB88320 for CRC-32).
// Every function documents the catalogue name or standard it implements.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dect2 {
namespace crc {

namespace detail {
constexpr uint32_t widthMask(int w) { return w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1u); }
// one entry of the MSB-first (non-reflected) byte table
constexpr uint32_t msbEntry(uint32_t i, int w, uint32_t poly) {
    uint32_t c = i << (w - 8);
    for (int k = 0; k < 8; k++) c = ((c >> (w - 1)) & 1u) ? ((c << 1) ^ poly) : (c << 1);
    return c & widthMask(w);
}
// one entry of the LSB-first (reflected) byte table
constexpr uint32_t lsbEntry(uint32_t i, uint32_t polyRef) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1u) ? ((c >> 1) ^ polyRef) : (c >> 1);
    return c;
}
template <int W, uint32_t Poly> constexpr std::array<uint32_t, 256> makeMsb() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) t[i] = msbEntry(i, W, Poly);
    return t;
}
template <uint32_t PolyRef> constexpr std::array<uint32_t, 256> makeLsb() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) t[i] = lsbEntry(i, PolyRef);
    return t;
}
template <int W, uint32_t Poly> inline const std::array<uint32_t, 256>& msbTable() {
    static constexpr std::array<uint32_t, 256> t = makeMsb<W, Poly>();
    return t;
}
template <uint32_t PolyRef> inline const std::array<uint32_t, 256>& lsbTable() {
    static constexpr std::array<uint32_t, 256> t = makeLsb<PolyRef>();
    return t;
}
} // namespace detail

// Feed one byte into an MSB-first (non-reflected) W-bit register; the building block of every Msb CRC (W in 8..32).
template <int W, uint32_t Poly> inline uint32_t msbUpdate(uint32_t reg, uint8_t b) {
    const auto& t = detail::msbTable<W, Poly>();
    return ((reg << 8) ^ t[((reg >> (W - 8)) ^ b) & 0xFF]) & detail::widthMask(W);
}
// Raw MSB-first CRC register over bytes (Rocksoft model, refin=refout=false); caller supplies init and applies any xorout.
template <int W, uint32_t Poly> inline uint32_t msbBytes(const uint8_t* p, size_t n, uint32_t init) {
    const auto& t = detail::msbTable<W, Poly>();
    uint32_t r = init;
    for (size_t i = 0; i < n; i++) r = ((r << 8) ^ t[((r >> (W - 8)) ^ p[i]) & 0xFF]) & detail::widthMask(W);
    return r;
}
// Raw LSB-first (reflected, refin=refout=true) CRC register over bytes; PolyRef is the bit-reversed polynomial.
template <uint32_t PolyRef> inline uint32_t lsbBytes(const uint8_t* p, size_t n, uint32_t init) {
    const auto& t = detail::lsbTable<PolyRef>();
    uint32_t r = init;
    for (size_t i = 0; i < n; i++) r = (r >> 8) ^ t[(r ^ p[i]) & 0xFF];
    return r;
}

// Bit-serial MSB-first CRC over unpacked bits (one bit per byte, only bit 0 used), any width 1..32: the air-interface form.
inline uint32_t bits(const uint8_t* b, size_t n, int width, uint32_t poly, uint32_t init, uint32_t xorout = 0) {
    const uint32_t mask = detail::widthMask(width);
    uint32_t r = init & mask;
    for (size_t i = 0; i < n; i++) {
        const uint32_t fb = ((r >> (width - 1)) & 1u) ^ (b[i] & 1u);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return (r ^ xorout) & mask;
}

// Fully generic Rocksoft-model CRC over bytes (slow bit-serial; for rare variants and as a reference): width, poly, init, refin, refout, xorout.
inline uint32_t generic(const uint8_t* p, size_t n, int width, uint32_t poly, uint32_t init, bool refin, bool refout, uint32_t xorout) {
    const uint32_t mask = detail::widthMask(width), top = 1u << (width - 1);
    uint32_t r = init & mask;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 8; k++) {
            const uint32_t in = refin ? ((p[i] >> k) & 1u) : ((p[i] >> (7 - k)) & 1u);
            const bool fb = ((r & top) != 0) != (in != 0);
            r = (r << 1) & mask;
            if (fb) r ^= poly;
        }
    if (refout) {
        uint32_t o = 0;
        for (int k = 0; k < width; k++) o |= ((r >> k) & 1u) << (width - 1 - k);
        r = o;
    }
    return (r ^ xorout) & mask;
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout); check("123456789") = 0x29B1. init is overridable (ITU-T V.41 chaining).
inline uint16_t crc16CcittFalse(const uint8_t* p, size_t n, uint16_t init = 0xFFFF) { return (uint16_t)msbBytes<16, 0x1021>(p, n, init); }
// CRC-16/XMODEM (poly 0x1021, init 0, no reflection, no xorout); check("123456789") = 0x31C3.
inline uint16_t crc16Xmodem(const uint8_t* p, size_t n, uint16_t init = 0) { return (uint16_t)msbBytes<16, 0x1021>(p, n, init); }
// CRC-16/GENIBUS (poly 0x1021, init 0xFFFF, xorout 0xFFFF): the ETSI "CCITT with inverted result" used by DAB FIB/AU; check = 0xD64E.
inline uint16_t crc16Genibus(const uint8_t* p, size_t n) { return (uint16_t)(msbBytes<16, 0x1021>(p, n, 0xFFFF) ^ 0xFFFF); }
// CRC-16/X-25 (reflected 0x1021 = 0x8408, init 0xFFFF, xorout 0xFFFF; HDLC FCS, AIS); check("123456789") = 0x906E.
inline uint16_t crc16X25(const uint8_t* p, size_t n) { return (uint16_t)(lsbBytes<0x8408>(p, n, 0xFFFF) ^ 0xFFFF); }
// CRC-16/KERMIT (reflected 0x1021, init 0, no xorout; IEEE 802.15.4 FCS); init is overridable (DJI DUML uses 0x3692); check("123456789") = 0x2189.
inline uint16_t crc16Kermit(const uint8_t* p, size_t n, uint16_t init = 0) { return (uint16_t)lsbBytes<0x8408>(p, n, init); }
// CRC-16/BUYPASS (poly 0x8005, init 0, no reflection, no xorout; FLAC frame footer); check("123456789") = 0xFEE8.
inline uint16_t crc16Buypass(const uint8_t* p, size_t n) { return (uint16_t)msbBytes<16, 0x8005>(p, n, 0); }
// CRC-32 (IEEE 802.3 / zlib / PNG: reflected 0x04C11DB7, init and xorout 0xFFFFFFFF); check("123456789") = 0xCBF43926.
inline uint32_t crc32(const uint8_t* p, size_t n) { return lsbBytes<0xEDB88320u>(p, n, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; }
// CRC-32 continued from a previous finished value (zlib crc32(crc, buf, n) semantics: pass 0 to start).
inline uint32_t crc32Update(uint32_t prev, const uint8_t* p, size_t n) { return lsbBytes<0xEDB88320u>(p, n, ~prev) ^ 0xFFFFFFFFu; }
// CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no xorout; MPEG-TS PSI, DVB); check("123456789") = 0x0376E6E7.
inline uint32_t crc32Mpeg2(const uint8_t* p, size_t n) { return msbBytes<32, 0x04C11DB7u>(p, n, 0xFFFFFFFFu); }

} // namespace crc
} // namespace dect2
