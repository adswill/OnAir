// Wideband LoRa survey (loraraw_survey.h).
#include "dect2/loraraw/loraraw_survey.h"
#include "loraraw_bank.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/mesh/mesh_presets.h"
#include "dect2/mesh/mesh_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace dect2 {
namespace lora {

using namespace detail;

namespace {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

const uint8_t kCommonSync[] = {0x12, 0x34, 0x2B, 0x14, 0x44, 0x24, 0x16, 0x18, 0x22};

} // namespace

// Matched spectral detection: for each LoRa bandwidth the burst-maximum of the power smoothed over that bandwidth is compared with a local
// noise floor plus a statistical margin (a few standard deviations of an average over the independent samples in the window), so a flat chirp
// band is found at about -8 dB SNR for 125 kHz instead of needing to stand out per bin. widthHz is the bandwidth of the scale that fired,
// peakDb how far the burst maximum stood above the floor, in dB.
std::vector<SurveyChannel> findActiveChannels(const cf32* x, size_t n, double fs, double marginDb) {
    std::vector<SurveyChannel> out;
    if (!x || fs <= 0) return out;
    int N = 512;
    while (N < 8192 && fs / N > 1500) N *= 2;
    const double binHz = fs / N;
    const size_t blocks = n / (size_t)N;
    if (blocks < 8) return out;
    const size_t G = std::max<size_t>(2, (size_t)std::lround(0.010 * fs / N));   // about 10 ms per group
    const size_t groups = blocks / G;
    if (groups < 2) return out;
    Fft fft(N);
    std::vector<float> win(N);
    for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (float)i / (float)N);
    const double scales[] = {500e3, 250e3, 125e3, 62.5e3, 31.25e3};
    const size_t S = sizeof scales / sizeof scales[0];
    std::vector<int> W(S);
    for (size_t k = 0; k < S; k++) W[k] = std::max(3, (int)std::lround(scales[k] / binHz));
    // two time scales: groups of about 10 ms (short frames) and of about 100 ms (long frames: SF11 / SF12 can be found well below 0 dB)
    constexpr int kTs = 2;
    const size_t tsMul[kTs] = {1, 10};
    std::vector<std::vector<std::vector<double>>> mx(kTs, std::vector<std::vector<double>>(S, std::vector<double>(N, 0.0)));
    std::vector<std::vector<std::vector<double>>> mean(kTs, std::vector<std::vector<double>>(S, std::vector<double>(N, 0.0)));
    std::vector<size_t> nEval(kTs, 0);
    std::vector<double> acc(N, 0.0), acc2(N, 0.0), p(N), cs(N + 1);
    std::vector<cf32> buf(N);
    auto evaluate = [&](int ts, const std::vector<double>& pw) {
        for (int k = 0; k < N; k++) p[(size_t)(k + N / 2) % (size_t)N] = pw[(size_t)k];   // frequency order
        cs[0] = 0;
        for (int i = 0; i < N; i++) cs[(size_t)i + 1] = cs[(size_t)i] + p[(size_t)i];
        nEval[(size_t)ts]++;
        for (size_t k = 0; k < S; k++) {
            const int h = W[k] / 2;
            for (int i = 0; i < N; i++) {
                const int lo = std::max(0, i - h), hi = std::min(N, i + h + 1);
                const double v = (cs[(size_t)hi] - cs[(size_t)lo]) / (hi - lo);
                if (v > mx[(size_t)ts][k][(size_t)i]) mx[(size_t)ts][k][(size_t)i] = v;
                mean[(size_t)ts][k][(size_t)i] += v;
            }
        }
    };
    for (size_t g = 0; g < groups; g++) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (size_t b = 0; b < G; b++) {
            const size_t at = (g * G + b) * (size_t)N;
            for (int i = 0; i < N; i++) buf[i] = x[at + (size_t)i] * win[i];
            fft.forward(buf.data());
            for (int k = 0; k < N; k++) acc[k] += (double)std::norm(buf[k]);
        }
        evaluate(0, acc);
        for (int k = 0; k < N; k++) acc2[(size_t)k] += acc[(size_t)k];
        if ((g + 1) % tsMul[1] == 0) { evaluate(1, acc2); std::fill(acc2.begin(), acc2.end(), 0.0); }
    }
    for (int ts = 0; ts < kTs; ts++) if (nEval[(size_t)ts]) for (size_t k = 0; k < S; k++) for (int i = 0; i < N; i++) mean[(size_t)ts][k][(size_t)i] /= (double)nEval[(size_t)ts];
    struct Hit { double off, bw, score, db; };
    std::vector<Hit> hits;
    const double margin = std::pow(10.0, marginDb / 10.0);
    for (int ts = 0; ts < kTs; ts++) for (size_t k = 0; k < S; k++) {
        if (fs < 2.0 * scales[k] || nEval[(size_t)ts] < 2) continue;
        const int h = W[k] / 2;
        // a local floor: the median of the group-averaged power over a wide window (the slope and ripple of a real radio's passband)
        std::vector<double> floorv(N);
        const int wnd = std::min(N / 2, std::max(8 * W[k], N / 8));
        std::vector<double> tmp;
        for (int i = 0; i < N; i += 4) {
            const int lo = std::max(0, i - wnd), hi = std::min(N, i + wnd + 1);
            tmp.assign(mean[(size_t)ts][k].begin() + lo, mean[(size_t)ts][k].begin() + hi);
            std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
            const double m = tmp[tmp.size() / 2];
            for (int j = i; j < std::min(N, i + 4); j++) floorv[(size_t)j] = m;
        }
        const double M = (double)(G * tsMul[(size_t)ts]) * (W[k] / 1.5);                      // independent complex samples in one smoothed value
        const double z = 7.5;   // trials: every group and every position of the smoothing window
        const double thr = 1.0 + z / std::sqrt(M);
        std::vector<Hit> cand;
        for (int i = h + 1; i < N - h - 1; i++) {
            if (floorv[(size_t)i] <= 0) continue;
            const double r = mx[(size_t)ts][k][(size_t)i] / floorv[(size_t)i];
            if (r < thr * margin) continue;
            if (mx[(size_t)ts][k][(size_t)i] < mx[(size_t)ts][k][(size_t)i - 1] || mx[(size_t)ts][k][(size_t)i] < mx[(size_t)ts][k][(size_t)i + 1]) continue;   // a local maximum
            cand.push_back({((double)i - N / 2) * binHz, scales[k], (r - 1.0) / (thr - 1.0), 10.0 * std::log10(r)});
        }
        std::sort(cand.begin(), cand.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
        std::vector<Hit> kept;
        for (const Hit& c : cand) {
            bool nearL = false;
            for (const Hit& q : kept) if (std::fabs(q.off - c.off) < 0.75 * scales[k]) nearL = true;
            if (!nearL) kept.push_back(c);
        }
        hits.insert(hits.end(), kept.begin(), kept.end());
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (const Hit& c : hits) { SurveyChannel ch; ch.offsetHz = c.off; ch.widthHz = c.bw; ch.peakDb = c.db; out.push_back(ch); }
    return out;
}

void classifyLora(SurveyEntry& e) {
    char b[160];
    if (!e.decoded) { e.network = "LoRa activity, not decoded"; snprintf(b, sizeof b, "%d preambles at SF%d, %.1f kHz: an implicit header, a rare sync word or a damaged signal", e.preambles, e.sf, e.bwHz / 1e3); e.detail = b; return; }
    const double mhz = e.freqHz / 1e6;
    auto nearL = [&](double target, double tolHz) { return e.freqHz > 0 && std::fabs(e.freqHz - target * 1e6) < tolHz; };
    auto bwIs = [&](double bwHz) { return std::fabs(e.bwHz - bwHz) < 0.03 * bwHz; };
    if (e.syncWord == 0x2B) {
        std::string hit;
        for (const std::string& name : meshtasticPresetNames()) {
            MeshLoraSettings s;
            if (meshtasticPreset(name, s) && s.sf == e.sf && bwIs(s.bwHz)) { hit += (hit.empty() ? "" : " / ") + name; }
        }
        e.network = hit.empty() ? "Meshtastic (custom modem settings)" : "Meshtastic " + hit;
        snprintf(b, sizeof b, "sync word 0x2B (Meshtastic), SF%d, %.1f kHz", e.sf, e.bwHz / 1e3);
        e.detail = b;
        return;
    }
    if (e.syncWord == 0x34) {
        for (int plan = 0; plan < lorawan::kPlans; plan++) {
            for (const lorawan::PlanChannel& c : lorawan::planChannels(plan)) {
                if (e.freqHz > 0 && std::fabs(c.mhz * 1e6 - e.freqHz) < 30e3 && bwIs(c.bwKhz * 1e3)) {
                    e.network = std::string("LoRaWAN ") + lorawan::planName(plan);
                    snprintf(b, sizeof b, "sync word 0x34, on the plan's %.3f MHz channel", c.mhz);
                    e.detail = b;
                    return;
                }
            }
        }
        e.network = e.freqHz > 0 ? "LoRaWAN (frequency not on a known plan)" : "LoRaWAN";
        e.detail = "sync word 0x34 (LoRaWAN public)";
        return;
    }
    if (e.syncWord == 0x12) {
        if (e.sf == 12 && bwIs(125e3) && (nearL(433.775, 20e3) || nearL(439.9125, 20e3) || nearL(434.855, 20e3) || nearL(433.900, 20e3))) {
            e.network = "LoRa APRS";
            snprintf(b, sizeof b, "sync word 0x12, SF12, 125 kHz on the APRS frequency %.4f MHz", mhz);
            e.detail = b;
            return;
        }
        const std::vector<MeshCorePresetEntry> pre = meshCoreBuiltinPresets();
        for (const MeshCorePresetEntry& p : pre) {
            if (p.sf == e.sf && std::fabs(p.bwKhz * 1e3 - e.bwHz) < 0.03 * e.bwHz && (e.freqHz <= 0 || std::fabs(p.mhz * 1e6 - e.freqHz) < 40e3)) {
                e.network = "MeshCore " + p.title;
                snprintf(b, sizeof b, "sync word 0x12 and the settings of the MeshCore preset (%.3f MHz, SF%d)", p.mhz, p.sf);
                e.detail = b;
                return;
            }
        }
        snprintf(b, sizeof b, "private LoRa (sync 0x12)");
        e.network = b;
        e.detail = "sync word 0x12 matches no known preset or plan";
        return;
    }
    snprintf(b, sizeof b, "LoRa (sync 0x%02X)", e.syncWord);
    e.network = b;
    e.detail = "an unusual sync word: a private network or a different protocol on LoRa";
}

SurveyResult loraSurvey(const cf32* x, size_t n, double fs, const SurveyOptions& o) {
    SurveyResult r;
    if (!x || n == 0 || fs <= 0) { r.note = "no samples"; return r; }
    n = std::min(n, (size_t)(o.maxSeconds * fs));
    r.seconds = (double)n / fs;
    r.channels = findActiveChannels(x, n, fs, o.activityDb);   // candidates, one per bandwidth scale that fired: a strong signal also fires its neighbours
    if (r.channels.empty()) { r.note = "no occupied channel found in the spectrum"; return r; }

    struct Cand { double off, bw; size_t chan; };
    std::vector<Cand> cands;   // strongest first
    for (size_t ci = 0; ci < r.channels.size(); ci++)
        if (fs >= Channelizer::kMinRatio * r.channels[ci].widthHz - 1) cands.push_back({r.channels[ci].offsetHz, r.channels[ci].widthHz, ci});
    std::vector<int> sfs;
    for (int sf = o.lowSf ? 5 : 7; sf <= 12; sf++) sfs.push_back(sf);
    std::vector<SurveyEntry> all;

    auto runBank = [&](const Cand& c, bool inv) {
        LabSettings s; s.bwHz = c.bw; s.offsetHz = c.off; s.invertIq = inv;
        std::vector<Params> ps;
        for (int sf : sfs) {
            s.sf = sf;
            Params p = paramsFor(s, kCommonSync[0]);
            for (size_t i = 1; i < sizeof kCommonSync; i++) p.moreSync.push_back(kCommonSync[i]);
            ps.push_back(p);
        }
        Bank b(fs, inv ? -c.off : c.off, c.bw, ps);
        b.run(x, n, inv, padFor(12, c.bw));
        std::vector<SurveyEntry> found;
        for (auto& one : b.decs) {
            const DemodStats& st = one.dm->stats();
            std::vector<RxFrame> frames = one.frames;
            uint64_t pre = st.preambles;
            if (frames.empty() && pre >= 3) {   // preambles but no frame: a rare sync word, try all of them on this combination
                Params p = one.p;
                p.moreSync.clear();
                for (int w = 0; w < 256; w++) if (w != (int)p.syncWord) p.moreSync.push_back((uint8_t)w);
                Bank b2(fs, inv ? -c.off : c.off, c.bw, {p});
                b2.run(x, n, inv, padFor(one.p.sf, c.bw));
                frames = b2.decs[0].frames;
            }
            if (frames.empty()) {
                if (pre < 3) continue;
                SurveyEntry e; e.offsetHz = c.off; e.bwHz = c.bw; e.sf = one.p.sf; e.invertIq = inv; e.preambles = (int)pre;
                e.freqHz = o.centerHz > 0 ? o.centerHz + c.off : 0;
                found.push_back(std::move(e));
                continue;
            }
            std::map<int, std::vector<const RxFrame*>> bySync;
            for (const RxFrame& f : frames) bySync[f.syncWord].push_back(&f);
            for (auto& kv : bySync) {
                SurveyEntry e; e.offsetHz = c.off; e.bwHz = c.bw; e.sf = one.p.sf; e.invertIq = inv; e.syncWord = kv.first; e.decoded = true; e.preambles = (int)pre;
                std::vector<double> snr, cfo; double first = 1e30, last = 0;
                for (const RxFrame* f : kv.second) {
                    e.frames++;
                    if (f->crcOk) e.crcOk++;
                    snr.push_back(f->snrDb); cfo.push_back(f->cfoHz);
                    const double t0 = f->startSample / b.outRate, t1 = f->endSample / b.outRate;
                    first = std::min(first, t0); last = std::max(last, t1);
                    Params q = one.p; q.syncWord = (uint8_t)kv.first;
                    if (f->hdr.cr > 0) q.cr = f->hdr.cr;
                    q.crc = f->hdr.crc;
                    e.airtimeSec += airSeconds(q, f->payload.size());
                    if (e.cr == 0) e.cr = q.cr;
                    if (f->crcOk && e.sample.empty()) e.sample.assign(f->payload.begin(), f->payload.begin() + (long)std::min<size_t>(32, f->payload.size()));
                }
                e.snrDb = median(snr); e.cfoHz = median(cfo);
                e.firstSec = first; e.lastSec = last;
                e.dutyPct = 100.0 * e.airtimeSec / r.seconds;
                e.freqHz = o.centerHz > 0 ? o.centerHz + c.off + e.cfoHz : 0;
                found.push_back(std::move(e));
            }
        }
        return found;
    };

    // strongest candidate first; a candidate sitting on a signal that already decoded is the same signal seen through another scale
    std::vector<char> done(cands.size(), 0);
    auto covered = [&](const Cand& c) {
        for (const SurveyEntry& e : all) if (e.decoded && std::fabs(e.offsetHz - c.off) < std::max(0.6 * std::max(e.bwHz, c.bw), 0.45 * (e.bwHz + c.bw))) return true;
        return false;
    };
    for (int pass = 0; pass < (o.tryInverted ? 2 : 1); pass++) {
        for (size_t ci = 0; ci < cands.size(); ci++) {
            const Cand& c = cands[ci];
            if (r.banks >= o.maxBanks) break;
            if (covered(c)) continue;
            if (pass == 1 && done[ci] == 2) continue;
            std::vector<SurveyEntry> f = runBank(c, pass == 1);
            r.banks++;
            done[ci] = 2;
            for (SurveyEntry& e : f) all.push_back(std::move(e));
        }
        if (pass == 0) std::fill(done.begin(), done.end(), 0);   // the inverted pass revisits every candidate that found nothing
    }

    // a decoded signal makes neighbouring spreading factors "hear" preambles, and a strong one can fake them at other bandwidths: keep a
    // preamble-only entry only when nothing decoded near it and it was heard often enough
    std::vector<SurveyEntry> keep;
    for (const SurveyEntry& e : all) {
        if (!e.decoded) {
            if (e.preambles < 8) continue;   // a noise burst fakes a few preambles; a beacon shows many
        }
        // nothing (no frame at all, or none with a good CRC) counts next to a signal that decoded with a good CRC: it is that signal seen off-centre
        if (e.crcOk == 0) {
            bool shadow = false;
            for (const SurveyEntry& d : all) if (d.crcOk > 0 && std::fabs(d.offsetHz - e.offsetHz) < std::max(0.6 * std::max(d.bwHz, e.bwHz), 0.45 * (d.bwHz + e.bwHz))) shadow = true;
            if (shadow) continue;
        }
        keep.push_back(e);
    }
    // the same signal found through two overlapping channel guesses
    std::vector<SurveyEntry> uniq;
    std::sort(keep.begin(), keep.end(), [](const SurveyEntry& a, const SurveyEntry& b) {
        if (a.decoded != b.decoded) return a.decoded;
        if (a.crcOk != b.crcOk) return a.crcOk > b.crcOk;
        if (a.frames != b.frames) return a.frames > b.frames;
        return a.preambles > b.preambles;
    });
    for (const SurveyEntry& e : keep) {
        bool dup = false;
        for (const SurveyEntry& u : uniq) if (u.sf == e.sf && u.syncWord == e.syncWord && u.decoded == e.decoded && std::fabs(u.bwHz - e.bwHz) < 1 && std::fabs(u.offsetHz - e.offsetHz) < 0.4 * e.bwHz && u.invertIq == e.invertIq) dup = true;
        if (!dup) uniq.push_back(e);
    }
    for (SurveyEntry& e : uniq) classifyLora(e);
    r.entries = std::move(uniq);
    char b[160];
    int dec = 0;
    for (const SurveyEntry& e : r.entries) if (e.decoded) dec++;
    snprintf(b, sizeof b, "%zu spectral candidate(s), %zu dechirp bank(s) run, %d LoRa signal(s) decoded, %d only heard", r.channels.size(), r.banks, dec, (int)r.entries.size() - dec);
    r.note = b;
    return r;
}

} // namespace lora
} // namespace dect2
