// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab: the whole pipeline. Stage 1 isolates the channel; stage 2 ranks the modulation families; for the best few, stage 3
// estimates the rate and recovers symbols, and stages 4-5 try the line codes, descramblers and (PSK) phase rotations, keeping the
// combination whose frames carry a checksum, or failing that the most consistent sync word. Stage 6 describes the fields.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dect2::signallab {

namespace {
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[400];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::vector<float> smooth(const std::vector<float>& v, int L) {
    if (L <= 1) return v;
    std::vector<float> o(v.size());
    double acc = 0;
    for (size_t i = 0; i < v.size(); i++) { acc += v[i]; if (i >= (size_t)L) acc -= v[i - L]; o[i] = (float)(acc / std::min<double>((double)i + 1, L)); }
    return o;
}

struct Down {
    LineCodeGuess line;
    std::vector<LineCodeGuess> ranked;
    std::vector<Bits> bits;
    FrameInfo frame;
    std::vector<ChecksumHit> checks;
    double score = -1;
};

bool natural(const ChecksumHit& h) { return h.coverStart == 0 || h.coversSync; }
double downScore(const FrameInfo& f, const std::vector<ChecksumHit>& c) {
    // a checksum that covers the frame from its first bit also confirms where the frame starts
    if (!c.empty()) return 10 + 5.0 * c[0].rate() + checkAlgos()[(size_t)c[0].algo].width / 32.0 + (natural(c[0]) ? 1 : 0);
    return f.syncCandidates.empty() ? 0 : f.sync.score;
}

// the checksum fits the inverted frames: turn them (and the sync word) the right way up
void normalisePolarity(Down& d) {
    if (d.checks.empty() || !d.checks[0].inverted) return;
    for (Bits& b : d.frame.frames) b = invert(b);
    for (Bits& b : d.bits) b = invert(b);
    d.frame.sync.pattern = invert(d.frame.sync.pattern);
    for (ChecksumHit& h : d.checks) h.inverted = !h.inverted;
    d.line.inverted = !d.line.inverted;
}

void trimToChecksum(FrameInfo& f, const std::vector<ChecksumHit>& c) {
    if (c.empty() || c[0].lengthFromEnd) return;
    const size_t end = c[0].coverEnd + (size_t)checkAlgos()[(size_t)c[0].algo].width;
    f.frameBits = end;
    f.variableLength = false;
    for (Bits& b : f.frames) if (b.size() > end) b.resize(end);
}

// GF(2) polynomials as bit masks (bit k = x^k)
uint64_t pmul(uint64_t a, uint64_t b) { uint64_t r = 0; for (int i = 0; i < 64 && b >> i; i++) if ((b >> i) & 1) r ^= a << i; return r; }
uint64_t pmod(uint64_t a, uint64_t g) {
    int dg = 63; while (dg >= 0 && !((g >> dg) & 1)) dg--;
    for (int i = 63; i >= dg && dg >= 0; i--) if ((a >> i) & 1) a ^= g << (i - dg);
    return a;
}
// The decoders are filters over GF(2): NRZI is 1 + x, a self-synchronising descrambler 1 + its taps. When the CRC's generator (or its
// reverse, for LSB-first bytes) divides that, every stretch of the decoded stream is a code word of it, whatever was sent: such a hit
// says nothing about the frame.
void dropArtifacts(std::vector<ChecksumHit>& hits, const LineCodeGuess& g) {
    uint64_t p = 1;
    if (g.code == LineCode::NRZI || g.code == LineCode::Differential) p = pmul(p, 3);
    if (g.scrambler >= 0 && !scramblers()[(size_t)g.scrambler].additive) p = pmul(p, ((uint64_t)scramblers()[(size_t)g.scrambler].taps << 1) | 1);
    if (p == 1) return;
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const ChecksumHit& h) {
        const CheckAlgo& a = checkAlgos()[(size_t)h.algo];
        if (a.kind != CheckKind::CRC) return false;
        const uint64_t G = (1ull << a.width) | a.poly;
        uint64_t R = 0;
        for (int i = 0; i <= a.width; i++) if ((G >> i) & 1) R |= 1ull << (a.width - i);
        return pmod(p, G) == 0 || pmod(p, R) == 0;
    }), hits.end());
}

// stages 4-5 on the slicer's bits
Down downstream(const std::vector<Bits>& raw) {
    Down best;
    if (raw.empty()) return best;
    std::vector<LineCodeGuess> rk = rankLineCodes(raw);
    best.ranked = rk;
    // each line code's best variant (so one code with every descrambler cannot crowd the others out), each of those without a
    // descrambler, the next best overall, and plain NRZ / NRZI always
    std::vector<LineCodeGuess> tryList;
    auto have = [&](LineCode c, int sc) { for (const auto& t : tryList) if (t.code == c && t.scrambler == sc) return true; return false; };
    auto plain = [&](LineCode c) { LineCodeGuess g; g.code = c; g.score = -1; for (const auto& r : rk) if (r.code == c && r.scrambler < 0) { g = r; break; } return g; };
    std::vector<LineCode> codes;
    for (const LineCodeGuess& g : rk) {
        if (std::find(codes.begin(), codes.end(), g.code) != codes.end() || codes.size() >= 4) continue;
        codes.push_back(g.code);
        tryList.push_back(g);
        if (g.scrambler >= 0 && !have(g.code, -1)) tryList.push_back(plain(g.code));
    }
    for (const LineCodeGuess& g : rk) { if (tryList.size() >= 8) break; if (!have(g.code, g.scrambler)) tryList.push_back(g); }
    for (LineCode c : {LineCode::NRZ, LineCode::NRZI}) if (!have(c, -1)) tryList.push_back(plain(c));
    int additiveTried = 0, variantsTried = 0;
    for (const LineCodeGuess& g : tryList) {
        Down d;
        d.line = g;
        for (const Bits& r : raw) d.bits.push_back(applyLineGuess(r, g));
        d.frame = findFrames(d.bits);
        d.checks = findChecksums(d.frame.frames, d.frame.sync.pattern);
        dropArtifacts(d.checks, g);
        d.score = downScore(d.frame, d.checks) + 0.05 * g.score;
        // other starts / lengths of the sync word (stage 5 is not sure where it ends): keep the one a checksum confirms
        if ((d.checks.empty() || !natural(d.checks[0])) && variantsTried < 3 && d.frame.syncCandidates.size() > 1) {
            variantsTried++;
            for (size_t v = 1; v < d.frame.syncCandidates.size() && v < 28; v++) {
                FrameInfo f = framesFor(d.bits, d.frame.syncCandidates[v]);
                if (f.frames.size() < 2) continue;
                auto ck = findChecksums(f.frames, f.sync.pattern);
                dropArtifacts(ck, g);
                if (ck.empty()) continue;
                const double sc = downScore(f, ck) + 0.05 * g.score;
                if (sc > d.score) {
                    f.syncCandidates = d.frame.syncCandidates; f.preambleBits = d.frame.preambleBits; f.autocorrPeaks = d.frame.autocorrPeaks;
                    d.frame = std::move(f); d.checks = std::move(ck); d.score = sc;
                    if (natural(d.checks[0])) break;
                }
            }
        }
        // additive (frame-synchronous) whitening: descramble each frame from its start with the register all ones
        if (d.checks.empty() && additiveTried < 4 && d.frame.frames.size() >= 2 && !d.frame.syncCandidates.empty()) {
            additiveTried++;
            for (size_t si = 0; si < scramblers().size(); si++) {
                if (!scramblers()[si].additive) continue;
                FrameInfo f = d.frame;
                for (Bits& b : f.frames) b = descramble(b, scramblers()[si]);
                auto ck = findChecksums(f.frames, f.sync.pattern);
                dropArtifacts(ck, g);
                if (ck.empty()) continue;
                const double sc = downScore(f, ck) + 0.05 * g.score;
                if (sc > d.score) { d.frame = f; d.checks = ck; d.score = sc; d.line.scrambler = (int)si; }
            }
        }
        trimToChecksum(d.frame, d.checks);
        normalisePolarity(d);
        if (d.score > best.score) { auto rk2 = best.ranked; best = std::move(d); best.ranked = rk2; }
    }
    return best;
}

// PSK/QAM symbols turned by r quarter (QAM) or 1/M (PSK) turns
Symbols rotate(const Symbols& s, Mod m, int r) {
    Symbols o = s;
    if (m == Mod::QAM) {
        for (int& k : o.sym) { int i = k / 4, q = k % 4; for (int t = 0; t < r; t++) { const int ni = 3 - q, nq = i; i = ni; q = nq; } k = i * 4 + q; }
    } else for (int& k : o.sym) k = (k + r) % s.order;
    return o;
}

// best agreement of b with a over n bits, b shifted by whole symbols (step bits) up to 4 symbols either way
double agreeBest(const Bits& a, const Bits& b, size_t n, int step) {
    double best = 0;
    for (int d = -4 * step; d <= 4 * step; d += step) {
        size_t ok = 0, t = 0;
        for (size_t i = 0; i < n; i++) {
            const long j = (long)i + d;
            if (i >= a.size() || j < 0 || j >= (long)b.size()) continue;
            t++; ok += a[i] == b[(size_t)j];
        }
        if (t > 32) best = std::max(best, (double)ok / (double)t);
    }
    return best;
}

struct Attempt {
    Mod mod = Mod::Unknown; int order = 0;
    RateResult rate;
    double modIndex = 0, spacingHz = 0, devHz = 0;
    std::vector<Symbols> sym;
    std::vector<Bits> raw;
    Down down;
    double score = -1;
    std::vector<std::string> log;
};

Attempt attempt(const std::vector<cf>& xc, double fs, const ChannelInfo& ch, const ModResult& mr, Mod mod, int order, const Options& o) {
    Attempt at;
    at.mod = mod; at.order = order;
    std::vector<Burst> segs = ch.bursts;
    if (segs.empty()) segs.push_back({0, xc.size()});
    (void)mr;
    const double bw = std::max(1.0, ch.bw99Hz);
    const bool linear = mod == Mod::PSK || mod == Mod::QAM;
    // linear modulations: the carrier first (x^M line), so the I / Q rails carry the symbols for the rate estimators
    std::vector<cf> xr = xc;
    const int Mp = mod == Mod::QAM ? 4 : order;
    if (linear) {
        const double off = powerLawOffset(xc, fs, Mp, &segs);
        xr = mix(xc, fs, -off);
        at.log.push_back(fmt("  carrier from the x^%d line: %+.1f Hz", Mp, off));
    }
    // stage 3a: rate
    std::vector<std::vector<float>> ss;
    std::vector<std::vector<cf>> cs;
    const int L = std::max(1, (int)std::lround(0.5 * fs / bw));
    for (const Burst& b : segs) {
        std::vector<cf> seg(xr.begin() + (long)b.start, xr.begin() + (long)b.end);
        if (!linear) { ss.push_back(smooth(symbolSignal(seg, fs, mod), L)); continue; }
        // follow the carrier phase block by block (x^M, ambiguity kept continuous) and put the points on the rails
        const double ref = (Mp == 4) ? M_PI / 4 : 0.0, amb = 2 * M_PI / Mp;
        const size_t B = 256;
        std::vector<float> I(seg.size()), Q(seg.size());
        double prev = 0;
        bool first = true;
        for (size_t s0 = 0; s0 < seg.size(); s0 += B) {
            std::complex<double> acc = 0;
            for (size_t i = s0; i < std::min(seg.size(), s0 + B); i++) { std::complex<double> p = 1, z = seg[i]; for (int k = 0; k < Mp; k++) p *= z; acc += p / std::max(1e-12, std::pow(std::abs(z), Mp - 2)); }
            double ph = (std::arg(acc) - Mp * ref) / Mp;
            if (!first) ph += amb * std::round((prev - ph) / amb);
            first = false; prev = ph;
            const cf rot = std::polar(1.f, (float)-ph);
            for (size_t i = s0; i < std::min(seg.size(), s0 + B); i++) { const cf z = seg[i] * rot; I[i] = z.real(); Q[i] = z.imag(); }
        }
        cs.push_back(seg);
        ss.push_back(I);
        if (Mp >= 4) ss.push_back(Q);
    }
    std::vector<RateEstimate> est;
    est.push_back(rateSpectralLine(ss, fs));
    est.push_back(rateZeroCrossing(ss, fs));
    est.push_back(rateAutocorr(ss, fs));
    if (linear || mod == Mod::OOK || mod == Mod::ASK) {
        if (!linear) for (const Burst& b : segs) cs.push_back(std::vector<cf>(xc.begin() + (long)b.start, xc.begin() + (long)b.end));
        est.push_back(rateCyclo(cs, fs));
    }
    at.rate = combineRates(est);
    if (o.forceBaud > 0) { at.rate.baud = o.forceBaud; at.rate.confidence = 1; }
    for (const auto& e : est) at.log.push_back(fmt("  rate by %s: %.1f Bd (strength %.2f)", rateMethodName(e.method), e.baud, e.strength));
    if (at.rate.baud <= 0 || fs / at.rate.baud < 1.8) { at.log.push_back("  no usable symbol rate"); return at; }
    const double baud = at.rate.baud;
    // stage 3b: symbols per burst
    for (const Burst& b : segs) {
        std::vector<cf> seg(xr.begin() + (long)b.start, xr.begin() + (long)b.end);
        at.sym.push_back(recoverSymbols(seg, fs, baud, mod, mod == Mod::MSK || mod == Mod::GMSK ? 2 : order));
    }
    // FSK: tone spacing from the recovered levels (Hz, the symbol signal is the instantaneous frequency); index 0.5 is MSK / GMSK
    if (mod == Mod::FSK || mod == Mod::MSK || mod == Mod::GMSK) {
        double span = 0, evm = 0; size_t n = 0;
        for (const Symbols& s : at.sym) if (s.levels.size() >= 2 && s.sym.size() > 16) { span += (s.levels.back() - s.levels.front()) * (double)s.sym.size(); evm += s.evm * (double)s.sym.size(); n += s.sym.size(); }
        if (n) {
            span /= (double)n; evm /= (double)n;
            at.spacingHz = span / std::max(1, (mod == Mod::FSK ? order : 2) - 1);
            at.devHz = span / 2;
            at.modIndex = at.spacingHz / baud;
            // (the matched filter pulls GMSK's levels in, so its index reads low)
            if (mod == Mod::FSK && order == 2 && o.forceMod == Mod::Unknown && at.modIndex > 0.25 && at.modIndex < 0.65) at.mod = evm > 0.22 ? Mod::GMSK : Mod::MSK;
        }
    }
    // PSK of 4 or more phases: line the bursts' phase ambiguity up with the first, then try each overall rotation
    const int M = mod == Mod::QAM ? 4 : mod == Mod::PSK ? order : 1;
    if (M >= 4 && at.sym.size() > 1) {
        const Bits ref = symbolsToBits(at.sym[0], mod, o.gray);
        for (size_t i = 1; i < at.sym.size(); i++) {
            double best = -1; int br = 0;
            for (int r = 0; r < M; r++) { const double a = agreeBest(ref, symbolsToBits(rotate(at.sym[i], mod, r), mod, o.gray), 160, mod == Mod::QAM ? 4 : M == 8 ? 3 : 2); if (a > best) { best = a; br = r; } }
            at.sym[i] = rotate(at.sym[i], mod, br);
        }
    }
    double bestScore = -1;
    int bestR = 0;
    for (int r = 0; r < std::max(1, M); r++) {
        std::vector<Bits> raw;
        for (const Symbols& s : at.sym) raw.push_back(symbolsToBits(M >= 4 ? rotate(s, mod, r) : s, mod, o.gray));
        Down d = downstream(raw);
        if (d.score > bestScore) { bestScore = d.score; bestR = r; at.down = std::move(d); at.raw = std::move(raw); }
    }
    if (M >= 4 && bestR) {
        for (Symbols& s : at.sym) s = rotate(s, mod, bestR);
        at.log.push_back(fmt("  phase rotation %d/%d fits best", bestR, M));
    }
    // how well the symbols sit on the constellation / levels: QPSK read as 16-QAM (or the reverse) scatters
    double evm = 0; size_t ns = 0;
    for (const Symbols& s : at.sym) { evm += s.evm * (double)s.sym.size(); ns += s.sym.size(); }
    evm = ns ? evm / (double)ns : 1;
    at.score = bestScore + 0.5 * at.rate.confidence + 0.5 * (1 - std::min(1.0, evm));
    // PSK has one ring, 16-QAM three (radii 1 : 2.2 : 3 for sqrt(2), sqrt(10), sqrt(18)): the spread of |z| tells them apart
    if (linear) {
        double m1 = 0, m2 = 0; size_t n = 0;
        for (const Symbols& s : at.sym) for (size_t i = s.points.size() / 10; i < s.points.size(); i++) { const double a = std::abs(s.points[i]); m1 += a; m2 += a * a; n++; }
        if (n > 32) {
            m1 /= (double)n; m2 /= (double)n;
            const double cv = m1 > 0 ? std::sqrt(std::max(0.0, m2 - m1 * m1)) / m1 : 0;
            // (decisive: QPSK read as 16-QAM is QPSK with zero bits woven in, and a CRC codeword squared is still one)
            if (mod == Mod::QAM && cv < 0.24) { at.score -= 100; at.log.push_back(fmt("  one amplitude ring (spread %.2f): not QAM", cv)); }
            if (mod == Mod::PSK && cv > 0.3) { at.score -= 100; at.log.push_back(fmt("  several amplitude rings (spread %.2f): not PSK", cv)); }
        }
        // a finer constellation always fits the points better; but QPSK read as 8PSK uses every other point only (BPSK as QPSK
        // likewise): then the lower order is the signal
        if (mod == Mod::PSK && order >= 4) {
            double ev = 0, od = 0;
            for (const Symbols& s : at.sym) for (size_t i = s.sym.size() / 10; i < s.sym.size(); i++) (s.sym[i] % 2 ? od : ev) += 1;
            const double share = ev + od > 0 ? std::min(ev, od) / (ev + od) : 0;
            if (ev + od > 64 && share < 0.12) { at.score -= 100; at.log.push_back(fmt("  only every other of the %d phases is used (%.0f %%): fewer phases", order, share * 100)); }
        }
    }
    return at;
}
}

Analysis analyse(const std::vector<cf>& x, double fs, const Options& o) {
    Analysis a;
    a.fs = fs;
    ChannelOptions co;
    co.minGapSec = o.minGapSec;
    const ChannelInfo ch0 = measureChannel(x, fs, co);
    a.channel = ch0;
    if (ch0.bw99Hz <= 0) { a.log.push_back("1 channel: no signal above the noise"); return a; }
    double fs2 = fs;
    const double bwc = ch0.bw99Hz * 1.25;
    std::vector<cf> xc = bwc * 2 < fs ? channelise(x, fs, ch0.offsetHz, bwc, fs2) : mix(x, fs, -ch0.offsetHz);
    ChannelInfo ch = measureChannel(xc, fs2, co);
    if (std::fabs(ch.offsetHz) > fs2 / 1024) xc = mix(xc, fs2, -ch.offsetHz);
    ch.offsetHz += ch0.offsetHz;
    ch.snrDb = ch0.snrDb;   // the channel filter takes the out-of-band noise away: the floor must come from the full capture
    a.channel = ch;
    a.fs = fs2;
    a.log.push_back(fmt("1 channel: %.0f Hz wide (99 %%), -3 dB %.0f Hz, carrier %+.0f Hz, SNR %.1f dB, %zu burst(s), duty %.0f %%%s",
                        ch.bw99Hz, ch.bw3dBHz, ch.offsetHz, ch.snrDb, ch.bursts.size(), ch.dutyCycle * 100, ch.continuous ? " (continuous)" : ""));
    if (ch.burstPeriodSec > 0) a.log.push_back(fmt("  bursts repeat every %.2f ms", ch.burstPeriodSec * 1000));
    // stage 2
    a.mod = classify(xc, fs2, ch.bw99Hz, &ch.bursts);
    for (const ModGuess& g : a.mod.ranked) a.log.push_back(fmt("2 modulation: %s%s%d %.0f %% - %s", modName(g.mod), g.order ? "-" : "", g.order, g.confidence * 100, g.why.c_str()));
    if (a.mod.ranked.empty()) a.log.push_back("2 modulation: no family fits well");
    std::vector<std::pair<Mod, int>> cands;
    if (o.forceMod != Mod::Unknown) cands.push_back({o.forceMod, o.forceOrder ? o.forceOrder : (o.forceMod == Mod::QAM ? 16 : 2)});
    else {
        for (const ModGuess& g : a.mod.ranked) if (cands.size() < 4 && g.confidence >= 0.05 && g.mod != Mod::OFDM) cands.push_back({g.mod, std::max(2, g.order)});
        if (cands.empty()) cands = {{Mod::FSK, 2}, {Mod::OOK, 2}, {Mod::PSK, 2}, {Mod::PSK, 4}, {Mod::PSK, 8}};
    }
    Attempt best;
    for (const auto& c : cands) {
        Attempt t = attempt(xc, fs2, ch, a.mod, c.first, c.second, o);
        if (t.score > best.score) best = std::move(t);
    }
    a.usedMod = best.mod; a.usedOrder = best.mod == Mod::MSK || best.mod == Mod::GMSK ? 2 : best.order;
    a.rate = best.rate;
    a.mod.modIndex = best.modIndex;
    if (best.spacingHz > 0) { a.mod.toneSpacingHz = best.spacingHz; a.mod.deviationHz = best.devHz; }
    a.log.push_back(fmt("3 symbol rate: %.2f Bd (confidence %.0f %%), using %s-%d", a.rate.baud, a.rate.confidence * 100, modName(a.usedMod), a.usedOrder));
    a.log.insert(a.log.end(), best.log.begin(), best.log.end());
    if (best.mod == Mod::FSK || best.mod == Mod::MSK || best.mod == Mod::GMSK)
        a.log.push_back(fmt("  deviation %.0f Hz, tone spacing %.0f Hz, modulation index %.2f", a.mod.deviationHz, a.mod.toneSpacingHz, a.mod.modIndex));
    a.symbols = best.sym;
    a.rawBits = best.raw;
    a.lineCodes = best.down.ranked;
    a.line = best.down.line;
    a.bits = best.down.bits;
    a.frame = best.down.frame;
    a.checks = best.down.checks;
    {
        double evm = 0; size_t n = 0;
        for (const Symbols& s : a.symbols) { evm += s.evm * (double)s.sym.size(); n += s.sym.size(); }
        if (n) a.log.push_back(fmt("  %zu symbols, EVM %.0f %%", n, evm / (double)n * 100));
    }
    std::string sc = a.line.scrambler >= 0 ? std::string(", descrambler ") + scramblers()[(size_t)a.line.scrambler].name : "";
    a.log.push_back(fmt("4 line code: %s%s%s", (a.usedMod == Mod::PSK && a.line.code == LineCode::NRZI) ? "differential" : lineCodeName(a.line.code), sc.c_str(), a.line.inverted ? ", inverted" : ""));
    if (!a.frame.syncCandidates.empty())
        a.log.push_back(fmt("5 frames: preamble %zu bits, sync %s (%zu bits, %d hits), %zu frames of %zu bits%s", a.frame.preambleBits, bitsToHex(a.frame.sync.pattern).c_str(),
                            a.frame.sync.pattern.size(), a.frame.sync.hits, a.frame.frames.size(), a.frame.frameBits, a.frame.variableLength ? " (variable length)" : ""));
    else a.log.push_back("5 frames: no sync word found");
    if (!a.checks.empty()) {
        const ChecksumHit& h = a.checks[0];
        a.log.push_back(fmt("  checksum %s over bits %s%zu..%zu, %s first%s, %d of %d frames", checkAlgos()[(size_t)h.algo].name, h.coversSync ? "sync+" : "", h.coverStart,
                            h.lengthFromEnd ? (size_t)0 : h.coverEnd, h.lsbFirst ? "LSB" : "MSB", h.littleEndian ? ", stored little-endian" : "", h.matched, h.tested));
    } else if (!a.frame.frames.empty()) a.log.push_back("  no checksum from the catalogue fits");
    // a repeating chip code in a continuous stream: spread spectrum
    if (a.bits.size() == 1 && a.bits[0].size() > 4096) {
        const auto r = bitAutocorr(a.bits[0], 1100);
        for (size_t l : {11, 13, 15, 31, 63, 127, 255, 511, 1023})
            if (l < r.size() && std::fabs(r[l]) > 0.7) { a.mod.spreadHint = true; a.log.push_back(fmt("  the chips repeat every %zu: a spreading code (DSSS)?", l)); break; }
    }
    a.fields = suggestFields(a.frame.frames, a.checks);
    a.columns = columnStats(a.frame.frames);
    a.log.push_back(fmt("6 fields: %zu suggested", a.fields.size()));
    return a;
}

Analysis analyseBits(const std::vector<Bits>& streams, double baud) {
    Analysis a;
    a.rate.baud = baud;
    a.rawBits = streams;
    Down d = downstream(streams);
    a.lineCodes = d.ranked; a.line = d.line; a.bits = d.bits; a.frame = d.frame; a.checks = d.checks;
    a.fields = suggestFields(a.frame.frames, a.checks);
    a.columns = columnStats(a.frame.frames);
    a.log.push_back(fmt("4 line code: %s", lineCodeName(a.line.code)));
    a.log.push_back(fmt("5 frames: %zu, sync %s", a.frame.frames.size(), bitsToHex(a.frame.sync.pattern).c_str()));
    return a;
}

void applyLineCode(Analysis& a, const LineCodeGuess& g) {
    Down d;
    d.line = g;
    for (const Bits& r : a.rawBits) d.bits.push_back(applyLineGuess(r, g));
    d.frame = findFrames(d.bits);
    if (g.scrambler >= 0 && scramblers()[(size_t)g.scrambler].additive) for (Bits& b : d.frame.frames) b = descramble(b, scramblers()[(size_t)g.scrambler]);
    d.checks = findChecksums(d.frame.frames, d.frame.sync.pattern);
    dropArtifacts(d.checks, g);
    trimToChecksum(d.frame, d.checks);
    normalisePolarity(d);
    a.line = d.line; a.bits = std::move(d.bits); a.frame = std::move(d.frame); a.checks = std::move(d.checks);
    a.fields = suggestFields(a.frame.frames, a.checks);
    a.columns = columnStats(a.frame.frames);
}

void reframe(Analysis& a, const Bits& sync, size_t frameBits) {
    SyncCandidate sc;
    sc.pattern = sync;
    FrameInfo f = framesFor(a.bits, sc, frameBits);
    f.preambleBits = a.frame.preambleBits;
    f.syncCandidates = a.frame.syncCandidates;
    if (a.line.scrambler >= 0 && scramblers()[(size_t)a.line.scrambler].additive)
        for (Bits& b : f.frames) b = descramble(b, scramblers()[(size_t)a.line.scrambler]);
    a.frame = f;
    a.checks = findChecksums(a.frame.frames, sync);
    trimToChecksum(a.frame, a.checks);
    a.fields = suggestFields(a.frame.frames, a.checks);
    a.columns = columnStats(a.frame.frames);
}

} // namespace dect2::signallab
