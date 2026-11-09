// LoRa demodulator for the Signal Lab (loraraw_lab.h): fixed settings and the automatic parameter search.
#include "dect2/loraraw/loraraw_lab.h"
#include "loraraw_bank.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace dect2 {
namespace lora {

using namespace detail;

LabResult labDemodulate(const cf32* x, size_t n, double fs, const LabSettings& s) {
    LabResult r;
    if (!x || n == 0 || fs <= 0 || s.bwHz <= 0 || fs < Channelizer::kMinRatio * s.bwHz - 1) return r;
    const Params p = paramsFor(s, (uint8_t)s.syncWord);
    Bank b(fs, s.invertIq ? -s.offsetHz : s.offsetHz, s.bwHz, {p});   // the offset is where the signal sits in the samples as given: conjugating mirrors it
    b.run(x, n, s.invertIq, padFor(p.sf, s.bwHz));
    for (const RxFrame& f : b.decs[0].frames) r.frames.push_back(toLab(f, s, b.outRate));
    r.stats = b.decs[0].dm->stats();
    return r;
}

LabDetect labAutoDetect(const cf32* x, size_t n, double fs, const LabDetectOptions& o) {
    LabDetect d;
    if (!x || n == 0 || fs <= 0) { d.note = "no samples"; return d; }
    n = std::min(n, (size_t)(o.maxSeconds * fs));
    double off = o.offsetHz;
    double bw99 = 0;
    if (o.autoOffset) {
        std::vector<signallab::cf> v(x, x + std::min(n, (size_t)(fs * 2)));   // two seconds are enough for the width and the centre
        const signallab::ChannelInfo ci = signallab::measureChannel(v, fs);
        if (ci.snrDb > 6 && ci.bw99Hz > 0) { off = ci.offsetHz; bw99 = ci.bw99Hz; }
    }
    std::vector<int> sfs;
    for (int sf = o.lowSf ? 5 : 7; sf <= 12; sf++) sfs.push_back(sf);
    static const uint8_t kCommon[] = {0x12, 0x34, 0x2B, 0x14, 0x44, 0x24, 0x16, 0x18, 0x22};

    struct Hit { LabCandidate c; std::vector<RxFrame> frames; double outRate; };
    for (int pass = 0; pass < (o.tryInverted ? 2 : 1) && !d.found; pass++) {
        const bool inv = pass == 1;
        std::vector<Hit> hits;
        for (double bw : kBws) {
            if (fs < Channelizer::kMinRatio * bw - 1) continue;
            if (bw99 > 0 && (bw < 0.2 * bw99 || bw > 3.0 * bw99)) continue;   // the measured width rules this bandwidth out
            LabSettings s;
            s.bwHz = bw; s.offsetHz = off; s.invertIq = inv;
            std::vector<Params> ps;
            for (int sf : sfs) {
                s.sf = sf;
                Params p = paramsFor(s, kCommon[0]);
                for (size_t i = 1; i < sizeof kCommon; i++) p.moreSync.push_back(kCommon[i]);
                ps.push_back(p);
            }
            Bank b(fs, inv ? -off : off, bw, ps);
            b.run(x, n, inv, padFor(12, bw));
            for (auto& one : b.decs) {
                const DemodStats& st = one.dm->stats();
                if (st.preambles == 0 && one.frames.empty()) continue;
                Hit h;
                h.c.sf = one.p.sf; h.c.bwHz = bw; h.c.preambles = (int)st.preambles;
                for (const RxFrame& f : one.frames) { h.c.frames++; if (f.crcOk) h.c.crcOk++; }
                h.frames = one.frames; h.outRate = b.outRate;
                hits.push_back(std::move(h));
            }
        }
        // preambles heard but no frame: the sync word is a rare one, try all 256 on those combinations
        for (Hit& h : hits) {
            if (h.c.frames > 0 || h.c.preambles < 2) continue;
            LabSettings s; s.sf = h.c.sf; s.bwHz = h.c.bwHz; s.offsetHz = off; s.invertIq = inv;
            Params p = paramsFor(s, 0x12);
            for (int w = 0; w < 256; w++) if (w != 0x12) p.moreSync.push_back((uint8_t)w);
            Bank b(fs, inv ? -off : off, h.c.bwHz, {p});
            b.run(x, n, inv, padFor(h.c.sf, h.c.bwHz));
            h.frames = b.decs[0].frames; h.outRate = b.outRate;
            for (const RxFrame& f : h.frames) { h.c.frames++; if (f.crcOk) h.c.crcOk++; }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            const int sa = a.c.crcOk * 100 + a.c.frames * 10, sb = b.c.crcOk * 100 + b.c.frames * 10;
            return sa != sb ? sa > sb : a.c.preambles > b.c.preambles;
        });
        for (const Hit& h : hits) d.candidates.push_back(h.c);
        if (hits.empty() || hits[0].c.frames == 0) continue;
        const Hit& best = hits[0];
        d.found = true;
        d.frames = best.c.frames; d.crcOk = best.c.crcOk;
        LabSettings& s = d.settings;
        s.sf = best.c.sf; s.bwHz = best.c.bwHz; s.offsetHz = off; s.invertIq = inv;
        const RxFrame* ref = &best.frames[0];
        for (const RxFrame& f : best.frames) if (f.crcOk) { ref = &f; break; }
        s.cr = ref->hdr.cr > 0 ? ref->hdr.cr : 5;
        s.crc = ref->hdr.crc;
        s.syncWord = ref->syncWord;
        s.ldro = -1;
        // the low data rate optimisation changes the payload layout: if no CRC passed, try the other setting
        if (best.c.crcOk == 0 && s.crc) {
            LabSettings t = s;
            t.ldro = autoLdro(s.sf, s.bwHz) ? 0 : 1;
            const LabResult r2 = labDemodulate(x, n, fs, t);
            int ok2 = 0;
            for (const LabFrame& f : r2.frames) if (f.crcOk) ok2++;
            if (ok2 > 0) { s.ldro = t.ldro; d.crcOk = ok2; d.frames = (int)r2.frames.size(); }
        }
    }
    char b[240];
    if (d.found) {
        snprintf(b, sizeof b, "SF%d, %.1f kHz, coding rate 4/%d, sync word 0x%02X, CRC %s, %s%s: %d frame(s), %d with a good CRC", d.settings.sf, d.settings.bwHz / 1e3,
                 d.settings.cr, d.settings.syncWord, d.settings.crc ? "on" : "off", d.settings.ldro < 0 ? "automatic LDRO" : d.settings.ldro ? "LDRO on" : "LDRO off",
                 d.settings.invertIq ? ", inverted IQ" : "", d.frames, d.crcOk);
        d.note = b;
    } else if (!d.candidates.empty()) {
        snprintf(b, sizeof b, "chirp preambles heard (best SF%d at %.1f kHz) but no frame decoded: an implicit header, a damaged capture, or a low SNR", d.candidates[0].sf, d.candidates[0].bwHz / 1e3);
        d.note = b;
    } else d.note = "no LoRa preamble found at any spreading factor or bandwidth";
    return d;
}

} // namespace lora
} // namespace dect2
