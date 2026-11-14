// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dvbText() on top of the Rust crate rust/onair_dvbsi: the prefix dispatch, ISO/IEC 6937 and the clean-up are Rust; the character set tables
// the system provides come back through convertCharset() (dvb_text.cpp), registered with the crate before main.
#include "dect2/tv/dvb_text.h"
#include "dect2/rs/dvbsi.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace dect2::dvbsi {

namespace {
// The conversion routine the crate calls: the whole of convertCharset(), copied into the crate's buffer.
size_t convertCb(const uint8_t* charset, const uint8_t* src, size_t n, uint8_t* out, size_t cap) {
    const std::string s = convertCharset((const char*)charset, src, (int)n);
    const size_t k = std::min(s.size(), cap);
    memcpy(out, s.data(), k);
    return k;
}
struct Register { Register() { onair_dvbsi_set_convert(convertCb); } } registerConvert;
} // namespace

TextContext TextContext::withLanguage(std::string_view language) const {
    TextContext result = *this;
    auto lower = [](std::string_view s) {
        std::string v(s);
        for (char& c : v) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return v;
    };
    const std::string lang = lower(language), region = lower(country);
    if (encoding == TextEncoding::Isdb) {
        if (region == "jpn" || lang == "jpn") result.encoding = TextEncoding::Arib;
        else if (region == "bra") result.encoding = TextEncoding::Latin15;
        else if (region == "phl") result.encoding = TextEncoding::Utf8;
        return result;
    }
    if (encoding != TextEncoding::Dvb) return result;   // a receiver/profile override is already specific
    if (lang == "chi" || lang == "zho") result.encoding = region == "twn" ? TextEncoding::Big5 : TextEncoding::Gb2312;
    else if (lang == "kor") result.encoding = TextEncoding::EucKr;
    else if (lang.empty()) { // country is only a fallback when the field has no language
        if (region == "twn") result.encoding = TextEncoding::Big5;
        else if (region == "chn") result.encoding = TextEncoding::Gb2312;
        else if (region == "kor") result.encoding = TextEncoding::EucKr;
    }
    if (!lang.empty()) result.country.clear(); // resolved for this field; do not reapply the country in dvbText()
    return result;
}

std::string dvbText(const uint8_t* p, int n, const TextContext& context) {
    if (!p || n <= 0) return {};
    TextEncoding encoding = context.withLanguage("").encoding;
    if (encoding == TextEncoding::Arib) return aribText(p, n);
    if (encoding == TextEncoding::Isdb) {
        // Latin ISDB deployments can use DVB selectors. Do not feed tagged UTF-8 through the ARIB state machine.
        const int first = p[0];
        const bool selector = (first >= 1 && first <= 0x0B && first != 8) || (first >= 0x10 && first <= 0x15);
        if (!selector && looksArib(p, n)) return aribText(p, n);
        encoding = TextEncoding::Dvb;
    }
    std::vector<uint8_t> b((size_t)n * 4 + 16);
    const size_t len = onair_dvbsi_dvb_text_with_encoding(p, (size_t)n, (uint32_t)encoding, b.data(), b.size());
    return len > b.size() ? std::string() : std::string((const char*)b.data(), len);
}

} // namespace dect2::dvbsi
