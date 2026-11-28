// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder blocks added for issue #666: Walsh coding (k bits -> a 2^k chip Walsh-Hadamard code word, decoded by the fast Walsh
// transform), the custom payload helpers, and the basic OFDM block (modulator and receiver; the FFT is the repo's dsp/fftutil.h).
// OFDM frame: two identical training symbols (timing, carrier offset and the channel from them), then data symbols; every symbol has
// a cyclic prefix; pilots (known BPSK) on every n-th used carrier track the common phase; the data carriers hold BPSK or QPSK.
#include "dect2/modebuilder/modebuilder.h"
#include "ofdm_rx.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2::modebuilder {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ---------------------------------------------------------------- Walsh
namespace stage {
namespace {
uint8_t parity(unsigned x) { x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return (uint8_t)(x & 1); }
}
std::vector<uint8_t> walshEncode(const std::vector<uint8_t>& bits, int k) {
    const size_t n = (size_t)1 << k;
    std::vector<uint8_t> out;
    out.reserve((bits.size() / (size_t)k + 1) * n);
    for (size_t i = 0; i < bits.size(); i += (size_t)k) {
        unsigned v = 0;
        for (int b = 0; b < k; b++) v = (v << 1) | (i + (size_t)b < bits.size() ? (unsigned)(bits[i + (size_t)b] & 1) : 0u);
        for (size_t j = 0; j < n; j++) out.push_back(parity((unsigned)(v & j)));
    }
    return out;
}

std::vector<uint8_t> walshDecode(const std::vector<uint8_t>& chips, int k, size_t nBits) {
    const size_t n = (size_t)1 << k;
    std::vector<uint8_t> out;
    std::vector<int> w(n);
    for (size_t p = 0; p + n <= chips.size() && out.size() < nBits; p += n) {
        for (size_t j = 0; j < n; j++) w[j] = chips[p + j] & 1 ? -1 : 1;
        for (size_t h = 1; h < n; h <<= 1)   // fast Walsh-Hadamard transform
            for (size_t i = 0; i < n; i += h << 1)
                for (size_t j = i; j < i + h; j++) { const int a = w[j], b = w[j + h]; w[j] = a + b; w[j + h] = a - b; }
        size_t best = 0;
        for (size_t v = 1; v < n; v++) if (w[v] > w[best]) best = v;
        for (int b = k - 1; b >= 0; b--) out.push_back((uint8_t)((best >> b) & 1));
    }
    out.resize(nBits, 0);
    return out;
}

size_t chipBits(const Definition& d, size_t cb) {
    if (d.walshK < 2) return cb;
    return (cb + (size_t)d.walshK - 1) / (size_t)d.walshK * ((size_t)1 << d.walshK);
}

std::vector<uint8_t> frameBody(const Definition& d, const std::vector<uint8_t>& frame) {
    std::vector<uint8_t> bits = bytesToBits(frame, d.lsbFirst), body;
    if (d.lenMode == LenMode::EndFlag) {
        body = d.bitStuffing ? hdlcStuff(bits) : bits;
        const std::vector<uint8_t> fl = d.flag.toBits();
        body.insert(body.end(), fl.begin(), fl.end());
    } else {
        body = fecEncode(d, bits);
        interleave(body, d.ilRows, d.ilCols, true);
    }
    scramble(body, d, true);
    if (d.walshK >= 2) body = walshEncode(body, d.walshK);
    return body;
}
}   // namespace stage

// ---------------------------------------------------------------- payload
std::vector<uint8_t> Definition::payloadBytes() const {
    std::vector<uint8_t> o;
    if (payloadKind == PayloadKind::Text) o.assign(payloadData.begin(), payloadData.end());
    else if (payloadKind == PayloadKind::Hex) {
        int hi = -1;
        for (char c : payloadData) {
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { o.push_back((uint8_t)(hi << 4 | v)); hi = -1; }
        }
    }
    return o;
}

// ---------------------------------------------------------------- OFDM layout
double Definition::ofdmSampleRate() const { return symbolRate * (ofdmFft + ofdmCp); }
int Definition::ofdmDataCarriers() const {
    int n = 0;
    for (int j = 0; j < ofdmUsed; j++) if (!(ofdmPilotEvery > 0 && j % ofdmPilotEvery == 0)) n++;
    return n;
}
int Definition::ofdmBitsPerSymbol() const { return ofdmDataCarriers() * (order >= 4 ? 2 : 1); }

namespace {
// used carriers in ascending frequency: bins -U/2..-1, 1..U/2 (DC unused)
int binOf(const Definition& d, int j) {
    const int h = d.ofdmUsed / 2;
    return j < h ? d.ofdmFft - (h - j) : j - h + 1;
}
bool isPilot(const Definition& d, int j) { return d.ofdmPilotEvery > 0 && j % d.ofdmPilotEvery == 0; }
uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
float trainValue(int j) { return hash32((uint32_t)j * 2654435761u + 12345u) & 1 ? -1.f : 1.f; }
float pilotSign(int t, int j) { return hash32((uint32_t)(t * 977 + j * 31 + 5)) & 1 ? -1.f : 1.f; }

// one OFDM symbol with cyclic prefix from the values of the used carriers
std::vector<cf> makeSymbol(const Definition& d, Fft& fft, const std::vector<cf>& X) {
    const int N = d.ofdmFft, CP = d.ofdmCp;
    std::vector<cf> sp((size_t)N, cf(0, 0));
    for (int j = 0; j < d.ofdmUsed; j++) sp[(size_t)binOf(d, j)] = X[(size_t)j];
    fft.inverse(sp.data());
    const float sc = 1.f / std::sqrt((float)d.ofdmUsed);
    std::vector<cf> o;
    o.reserve((size_t)(N + CP));
    for (int i = N - CP; i < N; i++) o.push_back(sp[(size_t)i] * sc);
    for (int i = 0; i < N; i++) o.push_back(sp[(size_t)i] * sc);
    return o;
}
std::vector<cf> trainingValues(const Definition& d) {
    std::vector<cf> X((size_t)d.ofdmUsed);
    for (int j = 0; j < d.ofdmUsed; j++) X[(size_t)j] = cf(trainValue(j), 0);
    return X;
}
int dataSymbols(const Definition& d) {
    const size_t bits = stage::codedBits(d, (size_t)d.longestFrameBytes() * 8);
    return (int)((bits + (size_t)d.ofdmBitsPerSymbol() - 1) / (size_t)d.ofdmBitsPerSymbol());
}
}   // namespace

std::vector<cf> ofdmFrame(const Definition& d, const std::vector<uint8_t>& frame) {
    Fft fft(d.ofdmFft);
    std::vector<uint8_t> bits = stage::frameBody(d, frame);
    const int nData = dataSymbols(d), bpc = d.order >= 4 ? 2 : 1;
    bits.resize((size_t)nData * (size_t)d.ofdmBitsPerSymbol(), 0);
    std::vector<cf> out;
    const std::vector<cf> t = makeSymbol(d, fft, trainingValues(d));
    out.insert(out.end(), t.begin(), t.end());
    out.insert(out.end(), t.begin(), t.end());
    size_t bp = 0;
    const float q = 1.f / std::sqrt(2.f);
    for (int s = 0; s < nData; s++) {
        std::vector<cf> X((size_t)d.ofdmUsed);
        for (int j = 0; j < d.ofdmUsed; j++) {
            if (isPilot(d, j)) { X[(size_t)j] = cf(pilotSign(s, j), 0); continue; }
            if (bpc == 1) X[(size_t)j] = cf(bits[bp++] ? -1.f : 1.f, 0);
            else { const float re = bits[bp++] ? -q : q, im = bits[bp++] ? -q : q; X[(size_t)j] = cf(re, im); }
        }
        const std::vector<cf> sym = makeSymbol(d, fft, X);
        out.insert(out.end(), sym.begin(), sym.end());
    }
    return out;
}

// ---------------------------------------------------------------- OFDM receiver
void Receiver::OfdmImpl::configure(double rate, double extra) {
    fsIn = rate > 0 ? rate : d.sampleRate;
    extraOff = extra;
    fsO = d.ofdmSampleRate();
    std::lock_guard<std::mutex> lk(mu);
    buf.clear();
    base = scan = 0;
    phase = 0;
    inPos = 0;
    prevIn = recentSum = cf(0, 0);
    recent.clear();
    inCount = 0;
    out.clear();
    st = RxStats{};
    st.fs1 = fsO;
    st.symbolRateHz = d.symbolRate;
}

void Receiver::OfdmImpl::feed(const cf* x, size_t n) {
    const double w = -2 * kPi * (d.offsetHz + extraOff) / fsIn, step = fsIn / fsO;
    const int avgN = std::max(1, (int)std::floor(step));   // a crude anti-alias average when the input is faster than the block
    std::vector<cf> mixed;
    mixed.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const cf y = x[i] * cf((float)std::cos(phase), (float)std::sin(phase));
        phase += w;
        if (phase > kPi) phase -= 2 * kPi;
        if (phase < -kPi) phase += 2 * kPi;
        mixed.push_back(y);
    }
    std::lock_guard<std::mutex> lk(mu);
    // running average over avgN samples, then linear interpolation to fsO
    for (size_t i = 0; i < n; i++) {
        cf y = mixed[i];
        if (avgN > 1) {
            recent.push_back(y);
            recentSum += y;
            if ((int)recent.size() > avgN) { recentSum -= recent.front(); recent.erase(recent.begin()); }
            y = recentSum / (float)recent.size();
        }
        const long long idx = inCount++;
        if (idx == 0) { prevIn = y; continue; }
        while (inPos < (double)idx) {
            const float f = (float)(inPos - (double)(idx - 1));
            buf.push_back(prevIn + (y - prevIn) * f);
            inPos += step;
        }
        prevIn = y;
    }
}

void Receiver::OfdmImpl::process(bool final) {
    std::lock_guard<std::mutex> lk(mu);
    const int N = d.ofdmFft, CP = d.ofdmCp, Ls = N + CP, L2 = 2 * Ls;
    const int nData = dataSymbols(d), bpc = d.order >= 4 ? 2 : 1;
    const size_t frameLen = (size_t)(2 + nData) * (size_t)Ls;
    Fft fft(N);
    const std::vector<cf> tr = makeSymbol(d, fft, trainingValues(d));
    double Et = 0;
    for (const cf& v : tr) Et += std::norm(v);
    std::vector<Frame> got;
    auto metric = [&](size_t l, cf& c1, cf& c2) {
        c1 = c2 = cf(0, 0);
        double e1 = 0, e2 = 0;
        for (int i = 0; i < Ls; i++) {
            const cf a = buf[l + (size_t)i], b = buf[l + (size_t)(Ls + i)];
            c1 += a * std::conj(tr[(size_t)i]);
            c2 += b * std::conj(tr[(size_t)i]);
            e1 += std::norm(a);
            e2 += std::norm(b);
        }
        if (e1 < 1e-12 || e2 < 1e-12) return 0.0;
        return (std::abs(c1) / std::sqrt(e1 * Et) + std::abs(c2) / std::sqrt(e2 * Et)) / 2;
    };
    size_t l = (size_t)std::max(0LL, scan - base);
    const double thr = 0.45;
    while (true) {
        if (buf.size() < l + (size_t)L2 + 8) break;
        cf c1, c2;
        if (metric(l, c1, c2) < thr) { l++; continue; }
        // one training symbol of the pair already gives half the metric (0.5), so the trigger can be a whole symbol early: the peak is the
        // best position from here to one symbol on
        if (!final && buf.size() < l + (size_t)(Ls + 4 + L2)) break;
        size_t best = l;
        double bm = 0;
        for (size_t k = l > 2 ? l - 2 : 0; k <= l + (size_t)Ls + 3 && buf.size() >= k + (size_t)L2; k++) {
            cf a, b;
            const double m = metric(k, a, b);
            if (m > bm) { bm = m; best = k; }
        }
        l = best;
        if (buf.size() < l + frameLen && !final) break;   // wait for the rest of the frame
        cf a, b;
        metric(l, a, b);
        const double om = std::arg(b * std::conj(a)) / Ls;   // rad per sample from the two identical symbols
        Fft f2(N);
        const int back = std::min(2, CP / 2);
        auto spectrum = [&](int sym, std::vector<cf>& Y) {
            std::vector<cf> t((size_t)N, cf(0, 0));
            const long long w0 = (long long)l + (long long)sym * Ls + CP - back;
            for (int i = 0; i < N; i++) {
                const long long p = w0 + i;
                if (p >= (long long)buf.size()) break;
                const double ang = -om * (double)(p - (long long)l);
                t[(size_t)i] = buf[(size_t)p] * cf((float)std::cos(ang), (float)std::sin(ang));
            }
            f2.forward(t.data());
            // the window starts `back` samples early: remove the linear phase (it is the same for every symbol, so H would absorb it, but keep it tidy)
            Y.resize((size_t)d.ofdmUsed);
            for (int j = 0; j < d.ofdmUsed; j++) Y[(size_t)j] = t[(size_t)binOf(d, j)];
        };
        std::vector<cf> Y1, Y2, Yd;
        spectrum(0, Y1);
        spectrum(1, Y2);
        std::vector<cf> H((size_t)d.ofdmUsed);
        double sig = 0, noi = 0;
        for (int j = 0; j < d.ofdmUsed; j++) {
            const cf avg = (Y1[(size_t)j] + Y2[(size_t)j]) * 0.5f;
            H[(size_t)j] = avg / trainValue(j);
            sig += std::norm(avg);
            noi += std::norm(Y1[(size_t)j] - Y2[(size_t)j]) / 4;
        }
        std::vector<uint8_t> bits;
        bits.reserve((size_t)nData * (size_t)d.ofdmBitsPerSymbol());
        for (int s = 0; s < nData; s++) {
            spectrum(2 + s, Yd);
            std::vector<cf> E((size_t)d.ofdmUsed);
            cf acc(0, 0);
            for (int j = 0; j < d.ofdmUsed; j++) {
                E[(size_t)j] = std::norm(H[(size_t)j]) > 1e-12 ? Yd[(size_t)j] / H[(size_t)j] : cf(0, 0);
                if (isPilot(d, j)) acc += E[(size_t)j] * pilotSign(s, j);
            }
            const cf rot = std::abs(acc) > 1e-9 ? std::conj(acc) / std::abs(acc) : cf(1, 0);   // the common phase error from the pilots
            for (int j = 0; j < d.ofdmUsed; j++) {
                if (isPilot(d, j)) continue;
                const cf z = E[(size_t)j] * rot;
                bits.push_back(z.real() < 0 ? 1 : 0);
                if (bpc == 2) bits.push_back(z.imag() < 0 ? 1 : 0);
            }
        }
        Frame f;
        size_t used = 0;
        decodeFrameBits(d, bits, 0, true, f, used);
        f.syncErrors = 0;
        f.snrDb = noi > 0 ? 10 * std::log10(std::max(1e-9, sig / noi)) : 60;
        f.timeSec = (double)(base + (long long)l) / fsO;
        st.syncHits++;
        if (!f.bytes.empty() && f.good()) { got.push_back(std::move(f)); st.frames++; st.snrDb = got.back().snrDb; }
        else if (d.showBadFrames && !f.bytes.empty()) { got.push_back(std::move(f)); st.badFrames++; }
        l += frameLen;
    }
    // drop what is behind the search position
    const size_t drop = std::min(buf.size(), l);
    buf.erase(buf.begin(), buf.begin() + (std::ptrdiff_t)drop);
    base += (long long)drop;
    scan = base;
    st.symbols = (uint64_t)(base / Ls);
    st.open = !buf.empty();
    for (Frame& f : got) out.push_back(std::move(f));
    if (out.size() > 20000) out.erase(out.begin(), out.begin() + (std::ptrdiff_t)(out.size() - 20000));
}

void Receiver::OfdmImpl::flush() {
    const size_t pad = (size_t)(d.ofdmFft + d.ofdmCp) * 4;
    std::vector<cf> z((size_t)(pad * std::max(1.0, fsIn / fsO)) + 16, cf(0, 0));
    feed(z.data(), z.size());
    process(true);
}
std::vector<Frame> Receiver::OfdmImpl::take() {
    std::lock_guard<std::mutex> lk(mu);
    std::vector<Frame> v;
    v.swap(out);
    return v;
}
RxStats Receiver::OfdmImpl::stats() const { std::lock_guard<std::mutex> lk(mu); return st; }

}   // namespace dect2::modebuilder
