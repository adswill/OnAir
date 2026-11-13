// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB SI text (see dvb_text.h). convertCharset() is the iconv / Windows code page wrapper and stays C++; dvbText() is the prefix dispatch, which the Rust crate
// rust/onair_dvbsi does (dvb_text_rs.cpp: it calls convertCharset back for the tables the system provides).
#include "dect2/tv/dvb_text.h"
#include "dect2/common/text_charset.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace dect2::dvbsi {
namespace textns = dect2::text;

#ifdef _WIN32
// Windows has the ISO 8859 tables built in (code pages 28591..28599, 28603, 28605): no iconv needed.
std::string convertCharset(const char* charset, const uint8_t* p, int n) {
    std::wstring w;
    if (!strcmp(charset, "UCS-2BE")) {
        for (int i = 0; i + 1 < n; i += 2) w += (wchar_t)((p[i] << 8) | p[i + 1]);
    } else {
        UINT cp = 28591;
        int v = 0;
        if (sscanf(charset, "ISO-8859-%d", &v) == 1) {
            if ((v >= 1 && v <= 9) || v == 13 || v == 15) cp = 28590 + v;
            else if (v == 11) cp = 874;   // Thai
        } else if (!strcmp(charset, "BIG5")) cp = 950;
        else if (!strcmp(charset, "GB2312")) cp = 936;
        else if (!strcmp(charset, "EUC-KR")) cp = 949;
        else if (!strcmp(charset, "EUC-JP")) cp = 20932;
        const int wn = MultiByteToWideChar(cp, 0, (const char*)p, n, nullptr, 0);
        if (wn <= 0) return std::string((const char*)p, n);
        w.resize((size_t)wn);
        MultiByteToWideChar(cp, 0, (const char*)p, n, w.data(), wn);
    }
    if (w.empty()) return {};
    const int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(un > 0 ? un : 0), '\0');
    if (un > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), un, nullptr, nullptr);
    return out;
}
#else
std::string convertCharset(const char* charset, const uint8_t* p, int n) {
    iconv_t cd = iconv_open("UTF-8", !strcmp(charset, "GB2312") ? "GB18030" : charset);   // GB 18030 holds GBK and GB 2312: names with extension characters convert whole
    if (cd == (iconv_t)-1) return std::string((const char*)p, n);
    std::string out(n * 4 + 8, '\0');
    char* in = (char*)p;
    size_t inLeft = n, outLeft = out.size();
    char* o = &out[0];
    iconv(cd, &in, &inLeft, &o, &outLeft);
    iconv_close(cd);
    out.resize(out.size() - outLeft);
    return out;
}
#endif


} // namespace dect2::dvbsi
