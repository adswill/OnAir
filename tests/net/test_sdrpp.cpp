// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SDR++ server client (source_sdrpp.cpp): the menu of a real server (an Airspy R2 behind sdrpp --server, captured byte for byte), and
// the whole client against a fake server on localhost that draws the Airspy source's menu as SDR++ does: rate and gain through the menu,
// the controls SDR++ greys out while streaming changed between a STOP and a START, the server's own settings, one client at a time, and a
// dropped connection.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

using namespace dect2;
using native::SdrppWidget;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// GET_UI's answer from a real SDR++ server (1.2, Airspy R2 serial 258C62DC2D43588F, 10 Msps, Linear gain 18), after the 4-byte command
static const unsigned char kRealUi[] = {
    0x00,0x00,0x00,0x00,0x80,0x01,0x04,0x16,0x00,0x23,0x23,0x73,0x64,0x72,0x70,0x70,0x5f,0x73,0x65,0x72,0x76,0x65,0x72,0x5f,
    0x73,0x72,0x63,0x5f,0x73,0x65,0x6c,0x02,0x00,0x00,0x00,0x00,0x04,0x07,0x00,0x41,0x69,0x72,0x73,0x70,0x79,0x00,0x02,0xff,
    0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x80,0x01,0x04,0x1f,0x00,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,0x64,0x65,
    0x76,0x5f,0x73,0x65,0x6c,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x02,0x00,0x00,0x00,0x00,
    0x04,0x11,0x00,0x32,0x35,0x38,0x43,0x36,0x32,0x44,0x43,0x32,0x44,0x34,0x33,0x35,0x38,0x38,0x46,0x00,0x02,0xff,0xff,0xff,
    0xff,0x00,0x80,0x00,0x04,0x1e,0x00,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,0x73,0x72,0x5f,0x73,0x65,0x6c,0x5f,
    0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x02,0x00,0x00,0x00,0x00,0x04,0x0f,0x00,0x31,0x30,0x2e,
    0x30,0x4d,0x48,0x7a,0x00,0x32,0x2e,0x35,0x4d,0x48,0x7a,0x00,0x02,0xff,0xff,0xff,0xff,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x81,0x01,0x04,0x23,0x00,0x52,0x65,0x66,0x72,0x65,0x73,0x68,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,0x72,0x65,
    0x66,0x72,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x03,0x00,0x00,0x00,0x00,0x03,0x00,0x00,
    0x00,0x00,0x00,0x85,0x00,0x00,0x82,0x00,0x02,0x03,0x00,0x00,0x00,0x04,0x25,0x00,0x41,0x69,0x72,0x73,0x70,0x79,0x47,0x61,
    0x69,0x6e,0x4d,0x6f,0x64,0x65,0x43,0x6f,0x6c,0x75,0x6d,0x6e,0x73,0x23,0x23,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,
    0x6f,0x75,0x72,0x63,0x65,0x01,0x00,0x00,0x84,0x01,0x04,0x23,0x00,0x53,0x65,0x6e,0x73,0x69,0x74,0x69,0x76,0x65,0x23,0x23,
    0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,0x67,0x6d,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,
    0x01,0x00,0x00,0x83,0x00,0x00,0x84,0x01,0x04,0x20,0x00,0x4c,0x69,0x6e,0x65,0x61,0x72,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,
    0x70,0x79,0x5f,0x67,0x6d,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x01,0x01,0x00,0x83,0x00,
    0x00,0x84,0x01,0x04,0x1e,0x00,0x46,0x72,0x65,0x65,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,0x67,0x6d,0x5f,0x41,
    0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x01,0x00,0x00,0x82,0x00,0x02,0x01,0x00,0x00,0x00,0x04,0x28,
    0x00,0x45,0x6e,0x64,0x41,0x69,0x72,0x73,0x70,0x79,0x47,0x61,0x69,0x6e,0x4d,0x6f,0x64,0x65,0x43,0x6f,0x6c,0x75,0x6d,0x6e,
    0x73,0x23,0x23,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x01,0x00,0x00,0x86,0x00,0x00,0x87,
    0x00,0x04,0x04,0x00,0x47,0x61,0x69,0x6e,0x00,0x00,0x00,0x00,0x88,0x00,0x04,0x20,0x00,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,
    0x70,0x79,0x5f,0x6c,0x69,0x6e,0x5f,0x67,0x61,0x69,0x6e,0x5f,0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,
    0x65,0x02,0x12,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x02,0x15,0x00,0x00,0x00,0x02,0x01,0x00,0x00,0x00,0x02,0x00,0x00,
    0x00,0x00,0x00,0x8b,0x00,0x04,0x1d,0x00,0x42,0x69,0x61,0x73,0x20,0x54,0x23,0x23,0x5f,0x61,0x69,0x72,0x73,0x70,0x79,0x5f,
    0x41,0x69,0x72,0x73,0x70,0x79,0x20,0x53,0x6f,0x75,0x72,0x63,0x65,0x01,0x00,
};

static const SdrppWidget* byId(const std::vector<SdrppWidget>& ui, const std::string& id) {
    for (const auto& w : ui) if (w.id == id) return &w;
    return nullptr;
}
static const RadioSetting* setting(const DeviceInfo& d, const std::string& key) {
    for (const auto& r : d.settings) if (r.key == key) return &r;
    return nullptr;
}

static void testRealMenu() {
    std::vector<SdrppWidget> ui;
    CHECK(native::sdrppParseUi(kRealUi, sizeof kRealUi, ui));
    const auto* src = byId(ui, "##sdrpp_server_src_sel");
    CHECK(src && src->kind == SdrppWidget::Combo && src->items.size() == 1 && src->items[0] == "Airspy" && src->i == 0);
    const auto* dev = byId(ui, "##_airspy_dev_sel_Airspy Source");
    CHECK(dev && dev->items.size() == 1 && dev->items[0] == "258C62DC2D43588F" && dev->label == "Device");
    const int rc = native::sdrppRateCombo(ui);
    CHECK(rc >= 0 && ui[(size_t)rc].id == "##_airspy_sr_sel_Airspy Source");
    if (rc >= 0) CHECK(ui[(size_t)rc].items.size() == 2 && native::sdrppParseRate(ui[(size_t)rc].items[0]) == 10e6 && native::sdrppParseRate(ui[(size_t)rc].items[1]) == 2.5e6);
    const auto* lin = byId(ui, "Linear##_airspy_gm_Airspy Source");
    const auto* sens = byId(ui, "Sensitive##_airspy_gm_Airspy Source");
    CHECK(lin && lin->kind == SdrppWidget::Radio && lin->b && sens && !sens->b);
    const auto* g = byId(ui, "##_airspy_lin_gain_Airspy Source");
    CHECK(g && g->kind == SdrppWidget::SliderInt && g->value == 18 && g->minV == 0 && g->maxV == 21 && g->label == "Gain" && !g->disabled);
    const auto* bt = byId(ui, "Bias T##_airspy_Airspy Source");
    CHECK(bt && bt->kind == SdrppWidget::Checkbox && !bt->b);
    const auto plan = native::sdrppGainPlan(ui);
    CHECK(plan.sliders.size() == 1 && plan.lo == 0 && plan.hi == 21 && plan.drivable);
    if (g) CHECK(native::sdrppSliderTarget(*g, plan, 9.6) == 10 && native::sdrppSliderTarget(*g, plan, 99) == 21 && native::sdrppSliderTarget(*g, plan, -5) == 0);
    // cut short, or not a draw list at all
    std::vector<SdrppWidget> bad;
    CHECK(!native::sdrppParseUi(kRealUi, sizeof kRealUi - 3, bad));
    const unsigned char junk[] = {9, 1, 2, 3};
    CHECK(!native::sdrppParseUi(junk, sizeof junk, bad));
}

static void testRates() {
    CHECK(native::sdrppParseRate("10.0MHz") == 10e6);
    CHECK(native::sdrppParseRate("2.5MHz") == 2.5e6);
    CHECK(native::sdrppParseRate("250KHz") == 250e3);
    CHECK(native::sdrppParseRate("2.048 MSPS") == 2.048e6);
    CHECK(native::sdrppParseRate("48000") == 48000);
    CHECK(native::sdrppParseRate("Auto") == 0);
    CHECK(native::sdrppParseRate("8 MHz BW") == 0);
}

// ---- a fake SDR++ server with an Airspy source -----------------------------------------------------------------------------------------
struct FakeSdrpp {
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true}, dropNow{false}, badScale{false};   // badScale: the scale field of every baseband packet is NaN (a garbled server)
    std::atomic<int> connections{0}, rejected{0};
    std::mutex mu;
    // the Airspy source's state
    int srId = 0, gm = 1, lin = 18, sens = 10, lna = 5, mix = 5, vga = 5;
    bool biasT = false, lnaAgc = false, running = false;
    int sampleType = 1, compression = -1;
    std::atomic<int> cmdDelayMs{0};          // > 0: commands are handled that much later while samples keep flowing (a slow link's queue)
    std::atomic<bool> levelFollowsGain{false};
    std::atomic<double> throttle{1.0};
    // SDR++'s connect race: the rate goes out 300 ms after the connection (its handler first stops a running source), and a GET_UI answered
    // before that leaves with a broken header, as the real server's shared send buffer does
    std::atomic<bool> racy{false};       // the share of the announced rate that gets through (a link slower than the stream: below 1)   // the samples' I is the linear gain / 32: which gain they were taken with
    double freq = 0;
    std::vector<std::string> log;   // "start", "stop", "action <id>=<value>", "freq <hz>", "type <n>"

    static void u32(std::vector<unsigned char>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((unsigned char)(v >> (8 * i))); }
    static uint32_t g32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
    // SmGui's draw list elements
    struct Dl {
        std::vector<unsigned char> b;
        void step(uint8_t s, bool sync = false) { b.push_back(0); b.push_back(s); b.push_back(sync); }
        void boolean(bool v) { b.push_back(1); b.push_back(v); }
        void i32(int v) { b.push_back(2); u32(b, (uint32_t)v); }
        void f32(float v) { uint32_t u; memcpy(&u, &v, 4); b.push_back(3); u32(b, u); }
        void str(const std::string& s) { b.push_back(4); b.push_back((unsigned char)s.size()); b.push_back((unsigned char)(s.size() >> 8)); b.insert(b.end(), s.begin(), s.end()); }
    };
    static constexpr const char* kSrc = "Airspy Source";
    static std::string id(const char* x) { return std::string(x) + kSrc; }
    std::vector<unsigned char> menu() {   // what server.cpp's drawMenu() and the Airspy source's menuHandler() record
        Dl d;
        if (running) d.step(2);
        d.step(0); d.step(0x80, true); d.str("##sdrpp_server_src_sel"); d.i32(0); d.str(std::string("Airspy\0", 7)); d.i32(-1);
        if (running) d.step(3);
        if (running) d.step(2);
        d.step(0); d.step(0x80, true); d.str(id("##_airspy_dev_sel_")); d.i32(0); d.str(std::string("258C62DC2D43588F\0", 17)); d.i32(-1);
        d.step(0x80, true); d.str(id("##_airspy_sr_sel_")); d.i32(srId); d.str(std::string("10.0MHz\0" "2.5MHz\0", 15)); d.i32(-1);
        d.step(1); d.step(0); d.step(0x81, true); d.str(id("Refresh##_airspy_refr_")); d.f32(0); d.f32(0);
        if (running) d.step(3);
        d.step(0x85); d.step(0x82); d.i32(3); d.str(id("AirspyGainModeColumns##_")); d.boolean(false);
        const char* modes[] = {"Sensitive", "Linear", "Free"};
        for (int m = 0; m < 3; m++) { if (m) d.step(0x83); d.step(0x84, true); d.str(std::string(modes[m]) + "##_airspy_gm_" + kSrc); d.boolean(gm == m); }
        d.step(0x82); d.i32(1); d.str(id("EndAirspyGainModeColumns##_")); d.boolean(false); d.step(0x86);
        auto slider = [&](const char* label, const char* key, int v, int hi) { d.step(0x87); d.str(label); d.step(0); d.step(0x88); d.str(id(key)); d.i32(v); d.i32(0); d.i32(hi); d.i32(1); d.i32(0); };
        if (gm == 0) slider("Gain", "##_airspy_sens_gain_", sens, 21);
        else if (gm == 1) slider("Gain", "##_airspy_lin_gain_", lin, 21);
        else {
            if (lnaAgc) d.step(2);
            slider("LNA Gain", "##_airspy_lna_gain_", lna, 15);
            if (lnaAgc) d.step(3);
            slider("Mixer Gain", "##_airspy_mix_gain_", mix, 15);
            slider("VGA Gain", "##_airspy_vga_gain_", vga, 15);
            d.step(0x8b, true); d.str(id("LNA AGC##_airspy_")); d.boolean(lnaAgc);
        }
        d.step(0x8b, true); d.str(id("Bias T##_airspy_")); d.boolean(biasT);
        return d.b;
    }
    static bool packet(sock_t c, uint32_t type, const std::vector<unsigned char>& body) {
        std::vector<unsigned char> m;
        u32(m, type); u32(m, (uint32_t)(8 + body.size()));
        m.insert(m.end(), body.begin(), body.end());
        return sockSendAll(c, m.data(), m.size());
    }
    static bool command(sock_t c, uint32_t type, uint32_t cmd, const std::vector<unsigned char>& arg = {}) {
        std::vector<unsigned char> b;
        u32(b, cmd);
        b.insert(b.end(), arg.begin(), arg.end());
        return packet(c, type, b);
    }
    bool sendRate(sock_t c) {
        const double r = srId == 0 ? 10e6 : 2.5e6;
        std::vector<unsigned char> a(8);
        memcpy(a.data(), &r, 8);
        return command(c, 0, 0x80, a);
    }
    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) return false;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    bool has(const std::string& line) { std::lock_guard<std::mutex> lk(mu); for (auto& l : log) if (l == line) return true; return false; }
    bool waitFor(const std::string& line, int ms = 3000) {
        for (int i = 0; i < ms / 10; i++) { if (has(line)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return false;
    }
    int indexOf(const std::string& line, int from = 0) { std::lock_guard<std::mutex> lk(mu); for (int i = from; i < (int)log.size(); i++) if (log[(size_t)i] == line) return i; return -1; }
    int lastIndexOf(const std::string& line) { std::lock_guard<std::mutex> lk(mu); for (int i = (int)log.size() - 1; i >= 0; i--) if (log[(size_t)i] == line) return i; return -1; }
    void clearLog() { std::lock_guard<std::mutex> lk(mu); log.clear(); }

    // one UI_ACTION: the id (a STRING element) and the value (any element), as SmGui applies it
    void apply(sock_t c, const unsigned char* p, size_t n) {
        if (n < 4) return;
        const bool sync = p[0] != 0;
        size_t k = 1;
        if (p[k] != 4) return;
        const size_t len = p[k + 1] | p[k + 2] << 8;
        const std::string wid((const char*)p + k + 3, len);
        k += 3 + len;
        const uint8_t t = p[k];
        int iv = 0; float fv = 0; bool bv = false;
        if (t == 1) bv = p[k + 1] != 0;
        else if (t == 2) iv = (int)g32(p + k + 1);
        else if (t == 3) { uint32_t u = g32(p + k + 1); memcpy(&fv, &u, 4); }
        std::string v = t == 1 ? (bv ? "1" : "0") : t == 2 ? std::to_string(iv) : std::to_string(fv);
        {
            std::lock_guard<std::mutex> lk(mu);
            log.push_back("action " + wid + "=" + v);
            if (wid == id("##_airspy_sr_sel_")) { srId = iv; sendRate(c); }   // core::setInputSampleRate, before the answer
            else if (wid == id("Sensitive##_airspy_gm_")) gm = 0;
            else if (wid == id("Linear##_airspy_gm_")) gm = 1;
            else if (wid == id("Free##_airspy_gm_")) gm = 2;
            else if (wid == id("##_airspy_lin_gain_")) lin = iv;
            else if (wid == id("##_airspy_sens_gain_")) sens = iv;
            else if (wid == id("##_airspy_lna_gain_")) lna = iv;
            else if (wid == id("##_airspy_mix_gain_")) mix = iv;
            else if (wid == id("##_airspy_vga_gain_")) vga = iv;
            else if (wid == id("Bias T##_airspy_")) biasT = bv;
            else if (wid == id("LNA AGC##_airspy_")) lnaAgc = bv;
        }
        if (sync) { std::vector<unsigned char> m; { std::lock_guard<std::mutex> lk(mu); m = menu(); } command(c, 1, 1, m); }
    }
    void serve(sock_t c) {
        std::vector<unsigned char> pend;
        { std::lock_guard<std::mutex> lk(mu); running = false; sampleType = 1; compression = 0; }   // server.cpp resets these per client
        const auto accepted = std::chrono::steady_clock::now();
        bool rateSent = !racy;
        if (rateSent) sendRate(c);
        auto lastSend = std::chrono::steady_clock::now(), pendAt = lastSend;
        while (run && !dropNow) {
            if (!rateSent && std::chrono::steady_clock::now() - accepted > std::chrono::milliseconds(300)) { sendRate(c); rateSent = true; }
            fd_set rd; FD_ZERO(&rd); FD_SET(c, &rd); FD_SET(ls, &rd);
            timeval tv{0, 1000};
            const int sel = select((int)std::max(c, ls) + 1, &rd, nullptr, nullptr, &tv);
            if (sel > 0 && FD_ISSET(ls, &rd)) {   // another client while this one is connected: told to go away
                sock_t o = accept(ls, nullptr, nullptr);
                if (o != kBadSock) { command(o, 0, 0x81); rejected++; std::this_thread::sleep_for(std::chrono::milliseconds(50)); sockClose(o); }
            }
            if (sel > 0 && FD_ISSET(c, &rd)) {
                unsigned char b[4096];
                const long long k = sockRecv(c, b, sizeof b);
                if (k <= 0) break;
                pend.insert(pend.end(), b, b + k);
                if (cmdDelayMs > 0) pendAt = std::chrono::steady_clock::now();
            }
            if (cmdDelayMs <= 0 || std::chrono::steady_clock::now() - pendAt >= std::chrono::milliseconds(cmdDelayMs)) {
                while (pend.size() >= 8 && pend.size() >= g32(pend.data() + 4)) {
                    const uint32_t type = g32(pend.data()), size = g32(pend.data() + 4);
                    if (type == 0 && size >= 12) {
                        const uint32_t cmd = g32(pend.data() + 8);
                        const unsigned char* a = pend.data() + 12;
                        const size_t an = size - 12;
                        if (cmd == 0 && !rateSent) {   // the race: the menu with the rate packet's 20-byte header
                            std::vector<unsigned char> m; { std::lock_guard<std::mutex> lk(mu); m = menu(); }
                            std::vector<unsigned char> b2; u32(b2, 1); u32(b2, 20); u32(b2, 0); b2.insert(b2.end(), m.begin(), m.end());
                            sockSendAll(c, b2.data(), b2.size());
                            { std::lock_guard<std::mutex> lk(mu); log.push_back("garbled"); }
                        } else if (cmd == 0) { std::vector<unsigned char> m; { std::lock_guard<std::mutex> lk(mu); m = menu(); } command(c, 1, 0, m); }
                        else if (cmd == 1) apply(c, a, an);
                        else if (cmd == 2) { std::lock_guard<std::mutex> lk(mu); running = true; log.push_back("start"); }
                        else if (cmd == 3) { std::lock_guard<std::mutex> lk(mu); running = false; log.push_back("stop"); }
                        else if (cmd == 4 && an == 8) {
                            double f; memcpy(&f, a, 8);
                            { std::lock_guard<std::mutex> lk(mu); freq = f; log.push_back("freq " + std::to_string((long long)std::llround(f))); }
                            command(c, 1, 4);
                        } else if (cmd == 6 && an == 1) { std::lock_guard<std::mutex> lk(mu); sampleType = a[0]; log.push_back("type " + std::to_string(a[0])); }
                        else if (cmd == 7 && an == 1) { std::lock_guard<std::mutex> lk(mu); compression = a[0]; }
                        else { std::vector<unsigned char> e{2}; packet(c, 6, e); }
                    }
                    pend.erase(pend.begin(), pend.begin() + size);
                }
            }
            // while running: the announced rate (times throttle) in packets every 2 ms, I = 0.5, Q = -0.25 in the sample type asked for (scale 1.0)
            bool on; int st; float level = 0.5f;
            { std::lock_guard<std::mutex> lk(mu); on = running; st = sampleType; if (levelFollowsGain) level = lin / 32.f; }
            if (on && std::chrono::steady_clock::now() - lastSend > std::chrono::milliseconds(2)) {
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::min(0.02, std::chrono::duration<double>(now - lastSend).count());
                lastSend = now;
                int count;
                { std::lock_guard<std::mutex> lk(mu); count = (int)((srId == 0 ? 10e6 : 2.5e6) * dt * throttle); }
                std::vector<unsigned char> body{0, 0, (unsigned char)st, 0};
                const float scale = badScale ? std::nanf("") : 1.0f;
                uint32_t su; memcpy(&su, &scale, 4); u32(body, su);
                for (int i = 0; i < count; i++) {
                    if (st == 2) { float re = level, im = -0.25f; uint32_t a, b2; memcpy(&a, &re, 4); memcpy(&b2, &im, 4); u32(body, a); u32(body, b2); }
                    else if (st == 1) { const int16_t re = (int16_t)std::lround(level * 32768), im = -8192; body.push_back((unsigned char)re); body.push_back((unsigned char)(re >> 8)); body.push_back((unsigned char)im); body.push_back((unsigned char)((uint16_t)im >> 8)); }
                    else { body.push_back((unsigned char)(int8_t)std::lround(level * 128)); body.push_back((unsigned char)(int8_t)-32); }
                }
                if (!packet(c, 2, body)) break;
            }
        }
    }
    void loop() {
        while (run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 100000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) continue;
            connections++;
            serve(c);
            sockShutdown(c);
            sockClose(c);
            dropNow = false;
        }
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }
};

template <class F> static bool eventually(F f, int ms = 3000) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}
#define SRV(cond) eventually([&] { std::lock_guard<std::mutex> lk(srv.mu); return (bool)(cond); })

static bool readIq(IqRing& ring, std::vector<cf32>& buf) {
    size_t got = 0;
    for (int i = 0; i < 300 && got < buf.size(); i++) { got += ring.read(buf.data() + got, buf.size() - got); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return got == buf.size();
}
static bool allIq(const std::vector<cf32>& buf, float re, float im, float tol) {
    for (size_t i = buf.size() / 2; i < buf.size(); i++) if (std::fabs(buf[i].real() - re) > tol || std::fabs(buf[i].imag() - im) > tol) return false;
    return true;
}

static void testClient() {
    FakeSdrpp srv;
    CHECK(srv.start());
    if (!srv.port) return;
    const std::string gm = "srv:##_airspy_gm_Airspy Source", bias = "srv:Bias T##_airspy_Airspy Source";
    NetRadio nr; nr.type = NetRadio::SdrppServer; nr.host = "127.0.0.1"; nr.port = srv.port;
    CHECK(netRadioDefaultPort(NetRadio::SdrppServer) == 5259 && std::string(netRadioTypeName(NetRadio::SdrppServer)) == "SDR++ server");

    // the probe reads the menu: rates, gain range and the server's own controls are known before the radio is opened
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));
    auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return; }
    DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "sdrpp" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port));
    CHECK(d.name == netRadioDeviceName(nr) && d.name.find("Airspy") != std::string::npos);
    CHECK(d.rateRanges.size() == 2 && d.rateRanges[0].first == 2.5e6 && d.rateRanges[1].first == 10e6 && d.maxRateHz == 10e6);
    CHECK(d.gainMinDb == 0 && d.gainMaxDb == 21);
    const RadioSetting* m = setting(d, gm);
    CHECK(m && m->type == RadioSetting::Choice && m->values.size() == 3 && m->values[1] == "Linear" && m->def == "Linear" && m->label == "SDR++: Gain mode");
    const RadioSetting* b = setting(d, bias);
    CHECK(b && b->type == RadioSetting::Bool && b->def == "0" && b->label == "SDR++: Bias T");
    const RadioSetting* srcSel = setting(d, "srv:##sdrpp_server_src_sel");
    CHECK(srcSel && srcSel->label == "SDR++: Server source");
    CHECK(!setting(d, "srv:##_airspy_sr_sel_Airspy Source") && !setting(d, "srv:##_airspy_lin_gain_Airspy Source"));   // OnAir drives these
    CHECK(!setting(d, "srv:Refresh##_airspy_refr_Airspy Source"));
    CHECK(setting(d, "format") && setting(d, "keepgain") && setting(d, "ppm"));

    // start: 2.2 Msps asked -> the 2.5 MHz entry, gain 10 on the linear slider, the frequency, 16-bit, no compression, then START
    auto src = makeSource(d);
    CHECK(src != nullptr);
    if (!src) { srv.stop(); return; }
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 2.2e6; s.centerHz = 100e6; s.gainDb = 10;
    err.clear();
    CHECK(src->start(s, ring, err));
    CHECK(srv.waitFor("start"));
    CHECK(srv.has("action ##_airspy_sr_sel_Airspy Source=1") && srv.has("action ##_airspy_lin_gain_Airspy Source=10") && srv.has("freq 100000000"));
    CHECK(srv.indexOf("action ##_airspy_sr_sel_Airspy Source=1") < srv.indexOf("start"));
    { std::lock_guard<std::mutex> lk(srv.mu); CHECK(srv.srId == 1 && srv.lin == 10 && srv.sampleType == 1 && srv.compression == 0 && srv.running); }
    CHECK(std::fabs(src->sampleRate() - 2.5e6) < 1);
    std::vector<cf32> buf(8192);
    CHECK(readIq(ring, buf));
    CHECK(allIq(buf, 0.5f, -0.25f, 1e-4f));
    // a second client is turned away by the server, and the probe says why
    err.clear();
    CHECK(!probeNetRadio(nr, 2000, err));
    CHECK(err.find("busy") != std::string::npos);

    // live: a faster rate (the server only takes it while stopped: STOP, the rate, START), the gain, 8-bit samples
    srv.clearLog();
    s.sampleRate = 8e6; s.gainDb = 21; s.radio["format"] = "8";
    err.clear();
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor("start"));
    const int st = srv.indexOf("stop"), sr = srv.indexOf("action ##_airspy_sr_sel_Airspy Source=0"), go = srv.lastIndexOf("start");
    CHECK(st >= 0 && sr > st && go > sr);
    CHECK(srv.has("action ##_airspy_lin_gain_Airspy Source=21") && srv.has("type 0"));
    CHECK(std::fabs(src->sampleRate() - 10e6) < 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ring.clear();
    CHECK(readIq(ring, buf));
    CHECK(allIq(buf, 0.5f, -0.25f, 1e-4f));
    // the gain alone: no STOP for a control the server takes while streaming
    srv.clearLog();
    s.gainDb = 5;
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor("action ##_airspy_lin_gain_Airspy Source=5"));
    CHECK(!srv.has("stop"));
    // the server's own settings: Free gain mode brings the LNA / mixer / VGA sliders, the gain then moves all three; bias tee on
    srv.clearLog();
    s.radio[gm] = "Free"; s.radio[bias] = "1"; s.gainDb = 45;
    CHECK(src->retune(s, err));
    CHECK(SRV(srv.gm == 2 && srv.biasT && srv.lna == 15 && srv.mix == 15 && srv.vga == 15));
    CHECK(!srv.has("stop"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the menu from the server's answer to the last gain change
    native::sdrppCaps(d);
    CHECK(d.gainMinDb == 0 && d.gainMaxDb == 45);   // three stages of 0-15
    CHECK(setting(d, gm) && setting(d, gm)->def == "Free" && setting(d, bias)->def == "1");
    s.gainDb = 22.5;   // half way: each stage half way (7.5 rounds to 8)
    CHECK(src->retune(s, err));
    CHECK(SRV(srv.lna == 8 && srv.mix == 8 && srv.vga == 8));
    // the LNA's AGC on: its slider greys out and is left alone; the gain still moves the other two
    s.radio["srv:LNA AGC##_airspy_Airspy Source"] = "1";
    s.gainDb = 0;
    CHECK(src->retune(s, err));
    CHECK(SRV(srv.lnaAgc && srv.lna == 8 && srv.mix == 0 && srv.vga == 0));
    // the gain left to the server
    srv.clearLog();
    s.radio["keepgain"] = "1"; s.gainDb = 45;
    CHECK(src->retune(s, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    { std::lock_guard<std::mutex> lk(srv.mu); CHECK(srv.mix == 0 && srv.vga == 0); }
    s.radio.erase("keepgain");
    s.radio["srv:LNA AGC##_airspy_Airspy Source"] = "0";
    s.radio[gm] = "Linear"; s.gainDb = 12;
    CHECK(src->retune(s, err));
    CHECK(SRV(srv.gm == 1 && srv.lin == 12));

    // a slow link: the server handles commands 300 ms late while its samples keep coming. A gain change returns at once (nothing waits for
    // the link), and not one sample taken with the old gain reaches the ring after it
    CHECK(SRV(srv.lin == 12));
    srv.levelFollowsGain = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    srv.cmdDelayMs = 300;
    s.gainDb = 20;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(src->retune(s, err));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
    ring.clear();
    CHECK(readIq(ring, buf));
    bool fresh = true;
    for (auto& v : buf) if (std::fabs(v.real() - 20 / 32.f) > 1e-3f) fresh = false;
    CHECK(fresh);
    srv.cmdDelayMs = 0;
    srv.levelFollowsGain = false;

    // the frequency correction: a clock 10 ppm fast is asked for 10 ppm lower
    srv.clearLog();
    s.radio["ppm"] = "10"; s.centerHz = 1e9;
    CHECK(src->retune(s, err));
    CHECK(srv.waitFor("freq " + std::to_string((long long)std::llround(1e9 / (1 + 10e-6)))));
    s.radio.erase("ppm");

    // a link that keeps up loses nothing; one that carries a third of the stream is counted as lost samples
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ring.resetDropped();
    for (int i = 0; i < 250; i++) { ring.clear(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }   // 2.5 s of the stream, read as it comes
    const uint64_t keptUp = ring.dropped();
    CHECK(keptUp < 10e6 * 0.3);   // at most one window's slack on a loaded machine
    srv.throttle = 0.33;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ring.resetDropped();
    for (int i = 0; i < 450; i++) { ring.clear(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const uint64_t slow = ring.dropped();
    CHECK(slow > 10e6 * 1.0);     // two thirds of 10 Msps for at least one full 2 s window
    printf("lost samples: link keeping up %llu, link at a third %llu\n", (unsigned long long)keptUp, (unsigned long long)slow);
    srv.throttle = 1.0;

    // the server drops the connection: the source connects again, sets everything again and starts
    srv.clearLog();
    const int before = srv.connections;
    srv.dropNow = true;
    bool back = false;
    for (int i = 0; i < 600 && !back; i++) { back = srv.connections >= before + 1 && srv.has("start"); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(back);
    CHECK(srv.has("type 0"));
    ring.clear();
    CHECK(readIq(ring, buf));
    CHECK(allIq(buf, 0.5f, -0.25f, 1e-4f));

    // stopping tells the server to stop its radio
    srv.clearLog();
    src->stop();
    CHECK(srv.waitFor("stop"));
    srv.stop();

    // not an SDR++ server: a server that answers with something else
    FakeSdrpp off;   // nothing listening on its port once stopped
    CHECK(off.start());
    const int deadPort = off.port;
    off.stop();
    NetRadio none; none.type = NetRadio::SdrppServer; none.host = "127.0.0.1"; none.port = deadPort;
    err.clear();
    CHECK(!probeNetRadio(none, 1000, err) && !err.empty());
}

// a server started with --autostart: streaming when the client connects, so SDR++ sends its rate late and garbles a GET_UI asked before it
// A garbled server (scale field NaN, or float samples that are NaN / infinity) must never put a non-finite sample into the receiver: it would stay
// in every filter and AGC until Stop.
static void testGarbledScale() {
    FakeSdrpp srv;
    CHECK(srv.start());
    if (!srv.port) return;
    NetRadio nr; nr.type = NetRadio::SdrppServer; nr.host = "127.0.0.1"; nr.port = srv.port;
    std::string err;
    CHECK(probeNetRadio(nr, 2000, err));
    auto devs = netRadioDevices({nr});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.badScale = true;
    auto src = makeSource(devs[0]);
    IqRing ring(1u << 20);
    TuneSettings s;
    s.sampleRate = 2.5e6; s.centerHz = 100e6; s.gainDb = 10;
    CHECK(src->start(s, ring, err));
    std::vector<cf32> buf(8192);
    CHECK(readIq(ring, buf));
    size_t nonFinite = 0;
    for (const cf32& v : buf) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) nonFinite++;
    CHECK(nonFinite == 0);
    src->stop();
    srv.stop();
}

static void testConnectRace() {
    FakeSdrpp srv;
    srv.racy = true;
    CHECK(srv.start());
    if (!srv.port) return;
    NetRadio nr; nr.type = NetRadio::SdrppServer; nr.host = "127.0.0.1"; nr.port = srv.port;
    std::string err;
    for (int i = 0; i < 3; i++) {
        err.clear();
        CHECK(probeNetRadio(nr, 2000, err));
        if (!err.empty()) fprintf(stderr, "probe: %s\n", err.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    auto src = makeSource(netRadioDevices({nr})[0]);
    IqRing ring(1u << 20);
    TuneSettings s; s.sampleRate = 2.2e6; s.centerHz = 100e6; s.gainDb = 10;
    err.clear();
    CHECK(src->start(s, ring, err));
    std::vector<cf32> buf(4096);
    CHECK(readIq(ring, buf));
    CHECK(!srv.has("garbled"));
    src->stop();
    srv.stop();
}

static void testPrefs() {
    NetRadio a; a.type = NetRadio::SdrppServer; a.host = "192.168.150.78"; a.port = 5259;
    NetRadio b; b.type = NetRadio::SpyServer; b.host = "spy.lan"; b.port = 5555;
    const auto back = netRadiosFromString(netRadiosToString({a, b}));
    CHECK(back.size() == 2 && back[0] == a && back[1] == b);
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    testRealMenu();
    testRates();
    testPrefs();
    testClient();
    testConnectRace();
    testGarbledScale();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("sdrpp: all passed\n");
    return 0;
}
