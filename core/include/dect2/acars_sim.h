// The ACARS generator straight into the receiver, with the list of what was sent checked off against what came out. Used by the
// tests and by acarstool sim.
#pragma once
#include "acars_gen.h"
#include "acars_rx.h"
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AcarsSimCfg {
    AcarsGenOptions gen;
    SynthConfig syn;
    double rate = 2e6;
    double secs = 30;
    size_t chunk = 65536;
    bool quant8 = false;                  // round to 8 bits like a HackRF or an RTL-SDR
    cf32 dc{0.f, 0.f};                    // added to every sample
    double gapAt = -1, gapSec = 0.020;    // drop this much signal at that time (samples vanish: the stream is shorter)
    double resetAt = -1;                  // call reset() then
    std::vector<double> userChannels;     // setChannels()
    double thrDb = 8;
    bool centerSet = true;                // call setCenterHz(gen.centerHz)
    double offsetHz = 0;                  // setSignalOffset
};

struct AcarsSimResult {
    int sent = 0;                         // blocks that were sent and ended well before the end (and clear of a gap or reset)
    int decoded = 0;                      // of those, decoded with exactly the right content
    int wrong = 0;                        // decoded messages that match nothing that was sent
    int total = 0;                        // everything that came out
    double rtf = 0;                       // seconds of signal per second of receiver time
    std::vector<AcarsMessage> msgs;       // oldest first
    AcarsTelemetry last;
    std::vector<AcarsSent> missing;
    std::vector<AcarsSent> sentAll;       // everything the generator sent
};

inline std::string acarsSimKey(const std::string& reg, const std::string& lab, char bid, const std::string& text) {
    std::string t;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\r') { if (i + 1 >= text.size() || text[i + 1] != '\n') t += '\n'; }
        else t += c;
    }
    std::string r = reg;
    while (!r.empty() && r[0] == '.') r.erase(0, 1);
    return r + "|" + lab + "|" + bid + "|" + t;
}

inline AcarsSimResult runAcarsSim(AcarsSimCfg c) {
    AcarsSimResult res;
    c.gen.log = std::make_shared<AcarsSentLog>();
    auto g = makeAcarsSynthEx(c.gen, c.syn, c.rate);
    AcarsReceiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(c.offsetHz);
    if (c.centerSet) rx.setCenterHz(c.gen.centerHz);
    if (!c.userChannels.empty()) rx.setChannels(c.userChannels);
    rx.setThresholdDb(c.thrDb);
    std::vector<cf32> buf(c.chunk);
    size_t left = (size_t)(c.secs * c.rate);
    const size_t total = left;
    double trx = 0;
    uint64_t last = 0, lastSerial = 0;
    bool gapDone = c.gapAt < 0, resetDone = c.resetAt < 0;
    double gapStart = -1, gapEnd = -1, resetTime = -1;
    size_t done = 0;
    while (left) {
        size_t n = std::min(left, c.chunk);
        if (!gapDone && (double)(done + n) / c.rate > c.gapAt) {         // the chunk that holds the gap: cut it up
            const size_t pre = (size_t)std::max(0.0, c.gapAt * c.rate - (double)done);
            n = std::min(n, pre);
            if (n == 0) {
                const size_t drop = (size_t)(c.gapSec * c.rate);
                std::vector<cf32> junk(std::min(drop, (size_t)65536));
                size_t d = drop;
                while (d) { const size_t k = std::min(d, junk.size()); g->generate(junk.data(), k); d -= k; }
                gapStart = (double)done / c.rate; gapEnd = gapStart + c.gapSec;
                done += drop; left = left > drop ? left - drop : 0;
                gapDone = true;
                continue;
            }
        }
        if (!resetDone && (double)(done + n) / c.rate > c.resetAt) {
            const size_t pre = (size_t)std::max(0.0, c.resetAt * c.rate - (double)done);
            if (pre == 0) { rx.reset(); resetDone = true; resetTime = (double)done / c.rate; continue; }
            n = std::min(n, pre);
        }
        g->generate(buf.data(), n);
        if (c.quant8)
            for (size_t i = 0; i < n; i++) {
                const float re = std::max(-1.f, std::min(1.f, buf[i].real() + c.dc.real())), im = std::max(-1.f, std::min(1.f, buf[i].imag() + c.dc.imag()));
                buf[i] = cf32(std::round(re * 127.f) / 127.f, std::round(im * 127.f) / 127.f);
            }
        else if (c.dc != cf32(0.f, 0.f))
            for (size_t i = 0; i < n; i++) buf[i] += c.dc;
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n);
        trx += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        left -= n; done += n;
        AcarsTelemetry tt;
        while (rx.telemetry(tt, last)) {
            last = tt.seq; res.last = tt;
            for (auto it = tt.messages.rbegin(); it != tt.messages.rend(); ++it)
                if (it->serial > lastSerial) { res.msgs.push_back(*it); lastSerial = it->serial; }
        }
    }
    res.rtf = (double)total / c.rate / std::max(trx, 1e-9);
    res.total = (int)res.msgs.size();
    // the answer key: what was sent. The time the receiver sees is the generator's time minus the dropped signal after the gap.
    std::multimap<std::string, int> got;
    for (size_t i = 0; i < res.msgs.size(); i++) got.emplace(acarsSimKey(res.msgs[i].reg, res.msgs[i].label, res.msgs[i].blockId, res.msgs[i].text), (int)i);
    std::vector<char> used(res.msgs.size(), 0);
    std::lock_guard<std::mutex> lk(c.gen.log->mu);
    res.sentAll = c.gen.log->sent;
    for (const auto& s : c.gen.log->sent) {
        std::string lab(2, ' '); lab[0] = s.spec.label[0]; lab[1] = s.spec.label[1];
        std::string body = s.spec.text;
        const bool dl = s.spec.blockId >= '0' && s.spec.blockId <= '9';
        if (dl && body.size() >= 10) body = body.substr(10);
        const std::string key = acarsSimKey(s.spec.reg, lab, s.spec.blockId, body);
        auto it = got.find(key);
        bool ok = false;
        if (it != got.end()) {
            for (auto r = got.equal_range(key); r.first != r.second; ++r.first)
                if (!used[(size_t)r.first->second]) { used[(size_t)r.first->second] = 1; ok = true; break; }
        }
        // blocks near the end, the gap or the reset do not count
        if (s.endSec > c.secs - 1.2) continue;
        if (gapStart >= 0 && s.endSec > gapStart - 0.15 && s.startSec < gapEnd + 0.15) continue;
        if (resetTime >= 0 && s.endSec > resetTime - 0.1 && s.startSec < resetTime + 0.9) continue;
        if (gapStart >= 0 && s.startSec > gapEnd) {}
        res.sent++;
        if (ok) res.decoded++; else res.missing.push_back(s);
    }
    for (size_t i = 0; i < res.msgs.size(); i++) {
        if (used[i]) continue;
        // a message that was sent but is not counted above (end, gap, reset) is not wrong
        bool known = false;
        for (const auto& s : c.gen.log->sent) {
            std::string lab(2, ' '); lab[0] = s.spec.label[0]; lab[1] = s.spec.label[1];
            std::string body = s.spec.text;
            if (s.spec.blockId >= '0' && s.spec.blockId <= '9' && body.size() >= 10) body = body.substr(10);
            if (acarsSimKey(s.spec.reg, lab, s.spec.blockId, body) == acarsSimKey(res.msgs[i].reg, res.msgs[i].label, res.msgs[i].blockId, res.msgs[i].text)) { known = true; break; }
        }
        if (!known) res.wrong++;
    }
    return res;
}

} // namespace dect2
