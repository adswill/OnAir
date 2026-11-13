// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the network protocol layers, meant for DECT2_SANITIZE=ON: every parser must never crash or hang on any bytes
// (WebSocket frames, request heads, flat JSON, URL text, rtl_tcp / SpyServer / SDR++ messages, the menu draw list, the radio list text). Buffers are exact-size heap
// blocks so a sanitizer sees a read or write past the end. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/net/net_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace N = dect2::netp;

static void one(const uint8_t* d, size_t n) {
    auto exact = [&](size_t len) { uint8_t* p = (uint8_t*)malloc(len ? len : 1); for (size_t i = 0; i < len; i++) p[i] = n ? d[i % n] : 0; return p; };
    uint8_t* e = exact(n);
    const std::string t((const char*)e, n);
    N::sha1(t); N::base64(t); N::wsAcceptKey(t); N::wsFrame(d && n ? d[0] : 1, t);
    N::wsParseClient(t);
    N::urlDecode(t); N::queryParam(t, "token"); N::queryParam(t, t.substr(0, n / 2)); N::hostOnly(t); N::loopbackName(t); N::ctEqual(t, t.substr(0, n / 2)); N::statusText((int)n);
    std::map<std::string, dect2::JsonVal> j;
    N::parseFlatJson(t, j);
    size_t end = 0;
    N::httpScan(t, end);
    N::HttpHead hh;
    N::httpHead(t, hh);
    if (n >= 12) { N::RtlGreeting g; N::rtlGreeting(e, n, g); }
    N::rtlTunerGains((int)n - 3); N::rtlTunerName((int)n - 3);
    std::string host; int port;
    N::splitHostPort(t, host, port);
    uint32_t w12[12], w9[9];
    N::spyDeviceInfo(e, (uint32_t)n, w12); N::spyClientSync(e, (uint32_t)n, w9);
    N::spyCommand(5, e, (uint32_t)n);
    if (n >= 20) { uint32_t a, b, c; N::spyHeader(e, a, b, c); }
    if (n >= 12) { uint32_t mr; memcpy(&mr, e, 4); N::spyRates(mr, (uint32_t)(e[4] % 40), e[8]); }
    std::vector<N::UiWidget> ui;
    N::sdrppParseUi(e, n, ui);
    N::sdrppIdFlags(t); N::sdrppLabelFromId(t); N::sdrppRateText(t);
    N::sdrppCommand(1, e, n); N::sdrppUiAction(t, (uint8_t)(n % 4), (int32_t)n, 1.5f, n & 1, n & 2);
    if (n >= 8) { uint32_t a, b; N::sdrppPacketHeader(e, a, b); }
    N::radiosFromString(t);
    N::cleanHost(t);
    std::vector<N::Radio> rs(1);
    rs[0].kind = (uint32_t)(n % 8); rs[0].host = t.substr(0, n / 2); rs[0].remote = t.substr(n / 2); rs[0].port = (int)n - 5;
    N::radiosToString(rs);
    free(e);
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : 100000;
    std::mt19937 g(1357);
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> b(g() % 3 == 0 ? g() % 40 : g() % 600);
        for (auto& x : b) x = (uint8_t)(g() % 4 ? g() % 128 : g());
        if (it % 5 == 0 && b.size() > 4) { memcpy(b.data(), "\x81\xfe", 2); }          // frames
        if (it % 7 == 0 && b.size() > 20) { memcpy(b.data(), "GET / HTTP/1.1\r\n", 16); }   // request heads
        if (it % 11 == 0 && b.size() > 4) { b[0] = 0; b[1] = 0x8b; b[2] = 0; }          // menus
        one(b.data(), b.size());
    }
    printf("fuzz_net_proto: %ld iterations ok\n", iters);
    return 0;
}
#endif
