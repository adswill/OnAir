// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// text_charset.h on top of the Rust crate rust/onair_dvbsi: the functions of text_charset.h (text_charset.cpp keeps appendUtf8). Output buffers
// are sized for the worst case of each converter (3 bytes per input byte for the table based ones).
#include "dect2/common/text_charset.h"
#include "dect2/rs/dvbsi.h"
#include <vector>

namespace dect2::text {

namespace {
// Runs a Rust converter with a buffer of `cap` bytes and returns the text.
template <class F> std::string run(size_t cap, F f) {
    std::vector<uint8_t> b(cap + 8);
    const size_t n = f(b.data(), b.size());
    return n > b.size() ? std::string() : std::string((const char*)b.data(), n);
}
} // namespace

std::string ebuLatin(const uint8_t* p, size_t n) { return run(3 * n, [&](uint8_t* o, size_t c) { return onair_dvbsi_ebu_latin(p, n, o, c); }); }
std::string ucs2be(const uint8_t* p, size_t n) { return run(3 * n, [&](uint8_t* o, size_t c) { return onair_dvbsi_ucs2be(p, n, o, c); }); }
std::string dabCharset(int cs, const uint8_t* p, size_t n) { return run(3 * n, [&](uint8_t* o, size_t c) { return onair_dvbsi_dab_charset(cs, p, n, o, c); }); }
std::string iso6937(const uint8_t* p, size_t n) { return run(3 * n, [&](uint8_t* o, size_t c) { return onair_dvbsi_iso6937(p, n, o, c); }); }
std::string clean(const std::string& s) { return run(s.size(), [&](uint8_t* o, size_t c) { return onair_dvbsi_clean((const uint8_t*)s.data(), s.size(), o, c); }); }

} // namespace dect2::text
