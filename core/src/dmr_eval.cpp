// Compare what the DMR receiver reported with what the test signal sent (see dmr_eval.h).
#include "dect2/dmr_eval.h"
#include "dect2/dmr_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>

namespace dect2 {

DmrScore dmrScore(const std::vector<DmrTruth>& truth, const DmrTelemetry& t, double secs, double margin, double skip, double offset) {
    DmrScore s;
    std::vector<char> used(t.callLog.size(), 0);
    double frames = 0, expect = 0;
    char b[256];
    for (const DmrTruth& tr : truth) {
        if (tr.startSec < skip || tr.endSec > secs - margin || !tr.finished) continue;
        if (tr.kind <= 2) {
            s.voiceSent++;
            if (!tr.alias.empty()) s.aliasSent++;
            bool found = false, ids = false;
            // the entry with the right IDs if there is one at about that time, else the nearest one
            int pick = -1;
            for (size_t i = 0; i < t.callLog.size(); i++) {
                const DmrCall& c = t.callLog[i];
                if (used[i] || c.kind > 2 || std::fabs(c.startSec - offset - tr.startSec) > 0.5) continue;
                if (pick < 0) pick = (int)i;
                if (c.idsKnown && c.src == tr.src && c.dst == tr.dst) { pick = (int)i; break; }
            }
            for (size_t i = (size_t)std::max(pick, 0); pick >= 0 && i == (size_t)pick; i++) {
                const DmrCall& c = t.callLog[i];
                used[i] = 1;
                ids = true;
                if (c.idsKnown && c.src == tr.src && c.dst == tr.dst && c.kind == tr.kind && c.voiceFrames >= (int)(0.6 * 3 * tr.voiceBursts)) {
                    found = true;
                    frames += c.voiceFrames; expect += 3.0 * tr.voiceBursts;
                    if (!tr.alias.empty() && c.alias == tr.alias) s.aliasFound++;
                    else if (!tr.alias.empty()) { snprintf(b, sizeof b, "  call at %.2f s: alias sent \"%s\", logged \"%s\"\n", tr.startSec, tr.alias.c_str(), c.alias.c_str()); s.report += b; }
                } else {
                    s.idsWrong++;
                    snprintf(b, sizeof b, "  call at %.2f s (%u -> %u, kind %d, %d bursts): logged %u -> %u kind %d ids %d frames %d\n", tr.startSec, tr.src, tr.dst, tr.kind, tr.voiceBursts, c.src, c.dst, c.kind, c.idsKnown, c.voiceFrames);
                    s.report += b;
                }
                break;
            }
            if (found) s.voiceFound++;
            else if (!ids) { snprintf(b, sizeof b, "  call at %.2f s (%u -> %u) not logged\n", tr.startSec, tr.src, tr.dst); s.report += b; }
        } else if (tr.kind == 3) {
            s.messagesSent++;
            bool found = false;
            for (const DmrMessage& m : t.messages)
                if (m.text == tr.text && m.src == tr.src && m.dst == tr.dst && m.crcOk) found = true;
            if (found) s.messagesFound++;
            else { snprintf(b, sizeof b, "  message at %.2f s \"%s\" not received\n", tr.startSec, tr.text.c_str()); s.report += b; }
            for (size_t i = 0; i < t.callLog.size(); i++)
                if (t.callLog[i].kind == 3 && std::fabs(t.callLog[i].startSec - offset - tr.startSec) < 0.6 && t.callLog[i].src == tr.src) used[i] = 1;
        } else {
            s.controlSent++;
            bool found = false;
            for (size_t i = 0; i < t.callLog.size(); i++) {
                const DmrCall& c = t.callLog[i];
                if (c.kind == 4 && std::fabs(c.startSec - offset - tr.startSec) < 0.6 && c.src == tr.src && c.dst == tr.dst) { found = true; used[i] = 1; }
            }
            if (found) s.controlFound++;
            else { snprintf(b, sizeof b, "  control message at %.2f s (opcode %02X) not received\n", tr.startSec, tr.csbkOpcode); s.report += b; }
        }
    }
    // entries inside the evaluated window that matched nothing
    for (size_t i = 0; i < t.callLog.size(); i++) {
        const DmrCall& c = t.callLog[i];
        if (used[i] || c.startSec - offset < skip || c.endSec - offset > secs - margin) continue;
        bool known = false;
        for (const DmrTruth& tr : truth)
            if (std::fabs(c.startSec - offset - tr.startSec) < 1.0 && tr.src == c.src) known = true;
        if (!known) {
            s.falseCalls++;
            snprintf(b, sizeof b, "  unexpected log entry at %.2f s: kind %d %u -> %u\n", c.startSec, c.kind, c.src, c.dst);
            s.report += b;
        }
    }
    s.frameRatio = expect > 0 ? frames / expect : 0;
    return s;
}

DmrRunResult dmrRun(const DmrScenario& s) {
    DmrRunResult r;
    DmrSignal sig(s.cfg);
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(s.cfg.rate);
    std::vector<cf32> buf(s.chunk);
    const size_t total = (size_t)(s.secs * s.cfg.rate);
    uint64_t seq = 0;
    bool reset = false;
    double resetTime = 0;
    for (size_t done = 0; done < total; done += s.chunk) {
        const size_t n = std::min(s.chunk, total - done);
        sig.generate(buf.data(), n);
        if (s.mod) s.mod(buf.data(), n, done, s.cfg.rate);
        if (s.quantise)
            for (size_t i = 0; i < n; i++) {
                auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
                buf[i] = cf32(q(buf[i].real()), q(buf[i].imag()));
            }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n);
        r.cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (s.resetAt > 0 && !reset && (double)(done + n) / s.cfg.rate >= s.resetAt) { rx.reset(); reset = true; resetTime = (double)(done + n) / s.cfg.rate; }
        DmrTelemetry t;
        if (rx.telemetry(t, seq)) {
            if (t.seq <= seq) r.seqOk = false;
            seq = t.seq;
            r.tel = t;
            if (t.state == 2 && r.lockedAt < 0) r.lockedAt = (double)(done + n) / s.cfg.rate;
        }
    }
    r.truth = sig.truth();
    // after a reset the receiver's clock starts again
    r.sc = dmrScore(r.truth, r.tel, s.secs, 2.5, s.skip, 0.05 - resetTime);
    return r;
}

} // namespace dect2
