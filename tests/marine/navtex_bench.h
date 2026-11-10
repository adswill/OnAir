// NAVTEX threshold bench (SENSITIVITY-3.9-B): 100 Bd FSK +-85 Hz at the marine 24 kHz base rate, AWGN measured in 300 Hz, optional
// carrier offset and drift. Runs the real FskSlicer + Spectrum centre finder + NavtexDecoder (the chain MarineReceiver::feed uses after its
// front end). A unit is one message received complete with every character right.
#pragma once
#include "dect2/dsp/gen_util.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/marine/marine_navtex.h"
#include "dect2/marine/marine_rx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace sens39b {

struct NavtexResult { int sent = 0, clean = 0, msgs = 0; uint64_t chars = 0; double cpuPerSec = 0; };

// lines = body lines after the time line (1: about 70 characters, 6: about 400 like the long test texts of the generator)
inline std::string navtexMsg(int k, int lines = 1) {
    char b[200];
    snprintf(b, sizeof b, "ZCZC AA%02d\r\n0100%02d UTC JAN 26\r\n", k % 100, k % 60);
    std::string s = b;
    for (int i = 0; i < lines; i++) {
        snprintf(b, sizeof b, "TEST MESSAGE %d LINE %d FROM STATION A. 518 KHZ, 52-10.5N 004-21.0E.\r\n", k, i + 1);
        s += lines == 1 ? std::string("TEST MESSAGE ") + std::to_string(k) + " FROM STATION A. 518 KHZ.\r\n" : std::string(b);
    }
    return s + "NNNN\r\n";
}

// nMsg messages, each after `idleSec` of idle signal; offHz carrier offset, driftHzPerMin linear drift.
inline NavtexResult navtexRun(double snrDb, int nMsg, uint32_t seed, double offHz = 0, double driftHzPerMin = 0, double idleSec = 3.0, int lines = 1, double inRate = 0, float gain = 1.f) {
    using namespace dect2;
    using namespace dect2::marine;
    const double fs = inRate > 0 ? inRate : kBaseRate;   // inRate > 0: the whole MarineReceiver at that input rate
    std::vector<uint8_t> bits;
    std::vector<std::string> texts;
    for (int k = 0; k < nMsg; k++) {
        std::vector<int> tok((size_t)std::max(20, (int)std::lround(idleSec * 100.0 / 14.0)), -1);
        const std::string t = navtexMsg(k + (int)(seed % 50) * 7, lines);
        texts.push_back(t);
        for (uint8_t c : navtexEncode(t)) tok.push_back(c);
        for (int i = 0; i < 12; i++) tok.push_back(-1);
        const std::vector<uint8_t> b = codesToBits(sitorBSlots(tok));
        bits.insert(bits.end(), b.begin(), b.end());
    }
    const double sps = fs / 100.0;
    std::mt19937 rng(seed * 2654435761u + 17);
    genutil::PortableNormal nd;
    const double sigma = std::sqrt((fs / 300.0) / std::pow(10.0, snrDb / 10.0) / 2.0);
    // random start inside the first symbol, so the bit clock is not aligned with the slicer's
    const size_t lead = (size_t)(rng() % (uint32_t)(fs * 0.2));
    std::vector<cf32> x(lead + (size_t)(bits.size() * sps) + (size_t)(fs * 0.2));
    double ph = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const long bi = i >= lead ? (long)((double)(i - lead) / sps) : -1;
        const bool on = bi >= 0 && bi < (long)bits.size();
        const double f = (on ? (bits[(size_t)bi] ? 85.0 : -85.0) : 0.0) + offHz + driftHzPerMin * (double)i / fs / 60.0;
        ph += 2 * M_PI * f / fs;
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        const cf32 s = on ? cf32((float)std::cos(ph), (float)std::sin(ph)) : cf32(0, 0);
        x[i] = gain * (s + cf32((float)(sigma * nd(rng)), (float)(sigma * nd(rng))));
    }
    NavtexResult r;
    r.sent = nMsg;
    Spectrum spec;
    FskSlicer fsk;
    NavtexDecoder nav[2] = {NavtexDecoder(false), NavtexDecoder(true)};
    std::vector<bool> got((size_t)nMsg, false);
    auto cb = [&](const NavtexMessage& m) {
        r.msgs++;
        if (getenv("NAVDBG")) fprintf(stderr, "msg %s complete %d errors %u chars %zu\n", m.header.c_str(), m.complete, m.errors, m.text.size());
        if (!m.complete || m.errors) return;
        for (int k = 0; k < nMsg; k++) {
            if (got[(size_t)k]) continue;
            const std::string& t = texts[(size_t)k];
            // the body as the decoder reports it: lines between the header line and NNNN, '\n' separated
            const size_t a = t.find("\r\n") + 2, e = t.find("NNNN");
            std::string body;
            for (size_t i = a; i < e; i++) if (t[i] != '\r') body += t[i];
            while (!body.empty() && body.back() == '\n') body.pop_back();
            std::string mt = m.text;
            while (!mt.empty() && (mt.back() == '\n' || mt.back() == '\r')) mt.pop_back();
            if (mt == body) { got[(size_t)k] = true; r.clean++; return; }
        }
    };
    nav[0].setCallback(cb); nav[1].setCallback(cb);
    for (auto& d : nav) d.setCharCallback([&](int, bool) { r.chars++; });
    const auto t0 = std::chrono::steady_clock::now();
    if (inRate > 0) {
        MarineReceiver rx;
        rx.setSilent(true); rx.configure(fs); rx.setService(1); rx.setFrequencyHz(518e3);
        const size_t blk = 65536;
        for (size_t o = 0; o < x.size(); o += blk) rx.feed(x.data() + o, std::min(blk, x.size() - o));
        MarineTelemetry tel;
        rx.telemetry(tel, 0);
        for (const auto& m : tel.navtex) for (int k = 0; k < m.repeats; k++) cb(m);
        r.cpuPerSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / ((double)x.size() / fs);
        return r;
    }
    const size_t blk = 2400;
    for (size_t o = 0; o < x.size(); o += blk) {
        const size_t n = std::min(blk, x.size() - o);
        if (spec.push(x.data() + o, n)) {
            const Spectrum::Fsk fi = spec.findFsk(170.0, 600.0);
            if (fi.found) fsk.setCentreHz(fi.centreHz);
        }
        fsk.process(x.data() + o, n, [&](float v) { nav[0].pushBit(v); nav[1].pushBit(v); });
    }
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.cpuPerSec = cpu / ((double)x.size() / fs);
    return r;
}

} // namespace sens39b
