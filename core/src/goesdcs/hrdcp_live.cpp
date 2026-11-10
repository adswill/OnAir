// Meteosat HRDCP receiver for the engine (see hrdcp_live.h). Written from EUMETSAT TD 16; no code from other decoders.
//   Front    : fourth-order CIC decimator, the capture down to about 800 kHz
//   Scanner  : averaged spectrum (about 50 Hz bins), the 2 s carrier that opens every message is a line over the noise
//   Slot     : one message: mixer, CIC to ~9.6 kHz, kept until decoded; carrier frequency from the carrier, root raised cosine matched filter,
//              preamble + marker by correlation in six pieces (phase, frequency, level, timing), offset QPSK with a carrier loop and a Gardner loop,
//              soft Viterbi, then the randomised Reed-Solomon frames and the CRC (hrdcp_proto.cpp)
#include "dect2/goesdcs/hrdcp_live.h"
#include "dect2/goesdcs/hrdcp_proto.h"
#include "dect2/goesdcs/goesdcs_proto.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/rrc.h"
#include "dcs_cic.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kFrontRate = 800e3;
constexpr double kSlotRate = 9600;
constexpr int kMaxSlots = 6;
constexpr int kMfSpan = 5;                 // matched filter: symbols each side
constexpr int kRef = hrdcp::kPreambleBits + hrdcp::kAsmBits;   // 192 known symbols
constexpr int kChunks = 6, kChunk = kRef / kChunks;

const std::vector<int8_t>& refSymbols() {   // +1 for a 0 bit, -1 for a 1 bit
    static const std::vector<int8_t> r = [] {
        std::vector<int8_t> v;
        for (int k = 0; k < 4; k++) for (int i = 31; i >= 0; i--) v.push_back((hrdcp::kPreambleWord >> i & 1) ? -1 : 1);
        for (int i = 63; i >= 0; i--) v.push_back((hrdcp::kAsm >> i & 1) ? -1 : 1);
        return v;
    }();
    return r;
}

struct Slot {
    int id = 0;
    double freq = 0;                       // nominal offset from the centre (the line), Hz
    cf32 ph{1, 0}, rot{1, 0};
    int renorm = 0;
    dcsdsp::Cic4 cic;
    double rs = kSlotRate, P = 8;          // baseband rate, samples a symbol
    float snr = 0;
    double bornSec = 0, lastSeenSec = 0, syncSec = 0;
    int64_t zb = 0;                        // absolute index of z[0]
    std::vector<cf32> z, y;                // baseband (derotated by f0 once known) and matched filter output, from zb
    int64_t yNext = 0;                     // absolute index of the next y to compute
    bool haveF = false;
    double f0 = 0;                         // carrier offset from the line, Hz (from the 2 s carrier)
    cf32 dph{1, 0}, drot{1, 0};
    int64_t zTotal = 0;
    int state = 0;                         // 0 carrier: looking for the marker, 1 data, 2 finished
    int tries = 0;
    // demodulator
    double t = 0, Pn = 8, theta = 0, omega = 0, tErr = 0;
    float amp = 1;
    cf32 prevB{0, 0};
    float prevI = 0, prevQ = 0;
    bool havePrev = false;
    uint64_t nsym = 0;
    meteor::Viterbi27 vit;
    bool hdrDone = false;
    int needFrames = 1;
    double evm = 0;
    std::vector<float> h;                  // matched filter taps
    int L = 0;
};

inline cf32 interp(const std::vector<cf32>& y, double x) {   // Catmull-Rom at a fractional index of y
    const int64_t i = (int64_t)std::floor(x);
    const float f = (float)(x - (double)i);
    if (i < 1 || i + 2 >= (int64_t)y.size()) return cf32(0, 0);
    const cf32 p0 = y[(size_t)i - 1], p1 = y[(size_t)i], p2 = y[(size_t)i + 1], p3 = y[(size_t)i + 2];
    return p1 + 0.5f * f * (p2 - p0 + f * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + f * (3.f * (p1 - p2) + p3 - p0)));
}

std::vector<uint8_t> packBits(const std::vector<uint8_t>& bits, size_t nbytes) {
    std::vector<uint8_t> o(std::min(nbytes, bits.size() / 8), 0);
    for (size_t i = 0; i < o.size(); i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) v = (uint8_t)(v << 1 | (bits[i * 8 + (size_t)k] & 1));
        o[i] = v;
    }
    return o;
}

} // namespace

struct HrdcpReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, fw = 0;
    std::atomic<double> centreReq{0}, thrReq{12};
    std::atomic<bool> resetReq{false}, clearReq{false}, logReq{false};
    std::atomic<int64_t> startReq{0};
    std::string logPath, logPathReq;
    dcsdsp::Cic4 front;
    int D = 1;
    std::vector<cf32> fbuf;
    // scanner
    int N = 0, M = 1, frames = 0, fill = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fr;
    std::vector<double> acc;
    double binHz = 0;
    struct Cand { double f; int seen; double snr; };
    std::vector<Cand> cands;
    struct Ignore { double f; double until; };
    std::vector<Ignore> ignore;
    std::vector<Slot> slots;
    int nextSlot = 1;
    // results
    std::vector<DcsMessage> msgs;
    std::vector<DcsPlatform> plats;
    uint32_t nextId = 1;
    uint64_t okN = 0, badN = 0, carriersSeen = 0;
    int64_t baseUtc = 0;
    uint64_t nIn = 0, nextPub = 0;
    double timeSec = 0;
    std::vector<uint8_t> wf;
    std::vector<float> spec;
    float noiseDb = -200, bestSnr = 0;
    double bestCfo = 0;
    bool dirty = true;
    DcsTelemetry pub;

    static int64_t nowUtc() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

    void resetState() {
        slots.clear(); cands.clear(); ignore.clear();
        const double r = rateSet > 0 ? rateSet : 1e6;
        D = std::max(1, (int)std::floor(r / kFrontRate));
        front.configure(D);
        fw = r / D;
        const double n = std::log2(std::max(1.0, fw / 50.0));
        N = 1 << std::max(9, std::min(14, (int)std::lround(n)));
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * math::kPi * (i + 0.5) / N));
        fr.assign((size_t)N, cf32(0, 0));
        fill = 0; frames = 0;
        acc.assign((size_t)N, 0.0);
        binHz = fw / N;
        M = std::max(1, (int)std::lround(0.1 * fw / N));
        wf.assign((size_t)kDcsWfRows * kDcsWfCols, 0);
        spec.assign(kDcsWfCols, 0.f);
        const int64_t s = startReq.load();
        baseUtc = s ? s : nowUtc();
        nIn = 0; nextPub = 0; timeSec = 0; bestSnr = 0;
        dirty = true;
    }

    // ----- scanner -----
    void spectrumDone() {
        std::vector<double> p((size_t)N);
        for (int k = 0; k < N; k++) p[(size_t)((k + N / 2) % N)] = acc[(size_t)k] / (double)M;
        std::fill(acc.begin(), acc.end(), 0.0);
        std::vector<double> s = p;
        std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end());
        const double floorv = std::max(1e-30, s[s.size() / 2]);
        noiseDb = (float)(10 * std::log10(floorv / ((double)N * (double)N * 0.375) + 1e-30));
        std::copy(wf.begin() + kDcsWfCols, wf.end(), wf.begin());
        for (int c = 0; c < kDcsWfCols; c++) {
            const int a = (int)((int64_t)c * N / kDcsWfCols), b = std::max(a + 1, (int)((int64_t)(c + 1) * N / kDcsWfCols));
            double mx = 0;
            for (int j = a; j < b; j++) mx = std::max(mx, p[(size_t)j] / floorv);
            const float db = (float)(10 * std::log10(std::max(mx, 1e-3)));
            spec[(size_t)c] = std::max(0.f, db);
            wf[(size_t)(kDcsWfRows - 1) * kDcsWfCols + (size_t)c] = (uint8_t)std::max(0.f, std::min(255.f, db / 40.f * 255.f));
        }
        const double thr = std::pow(10.0, thrReq.load() / 10.0);
        std::vector<Cand> found;
        for (int i = 4; i < N - 4; i++) {
            if (std::abs(i - N / 2) <= 1) continue;   // the radio's DC spike
            const double v = p[(size_t)i];
            if (v < thr * floorv) continue;
            bool top = true;
            for (int j = i - 4; j <= i + 4; j++) if (p[(size_t)j] > v || (j < i && p[(size_t)j] == v)) { top = false; break; }
            if (!top) continue;
            const double a = std::log(p[(size_t)i - 1] + 1e-30), b = std::log(v), c = std::log(p[(size_t)i + 1] + 1e-30);
            const double den = a - 2 * b + c;
            const double d = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
            found.push_back({((double)(i - N / 2) + d) * binHz, 1, 10 * std::log10(v / floorv)});
        }
        std::vector<Cand> next;
        for (const auto& f : found) {
            Cand c = f;
            for (const auto& o : cands) if (std::fabs(o.f - f.f) < 100) { c.seen = o.seen + 1; break; }
            next.push_back(c);
            for (auto& g : ignore) if (std::fabs(g.f - f.f) < 300) g.until = std::max(g.until, timeSec + 1.5);
            for (auto& sl : slots) if (std::fabs(sl.freq - f.f) < 300) sl.lastSeenSec = timeSec;
        }
        cands = next;
        ignore.erase(std::remove_if(ignore.begin(), ignore.end(), [&](const Ignore& g) { return g.until < timeSec; }), ignore.end());
        for (const auto& c : cands) {
            if (c.seen < 2) continue;
            bool have = (int)slots.size() >= kMaxSlots;
            for (const auto& sl : slots) if (std::fabs(sl.freq - c.f) < 1200) have = true;
            for (const auto& g : ignore) if (std::fabs(g.f - c.f) < 300) have = true;
            if (!have) spawn(c.f, (float)c.snr);
        }
    }

    void spawn(double f, float snr) {
        Slot s;
        s.id = nextSlot++;
        s.freq = f; s.snr = snr;
        const double ds = std::max(1.0, std::round(fw / kSlotRate));
        s.cic.configure((int)ds);
        s.rs = fw / ds;
        s.P = s.rs / hrdcp::kSymbolRate;
        s.rot = std::polar(1.f, (float)(-2 * math::kPi * f / fw));
        s.bornSec = timeSec; s.lastSeenSec = timeSec;
        s.L = (int)std::ceil(kMfSpan * s.P);
        s.h.resize((size_t)(2 * s.L + 1));
        double e = 0;
        for (int k = -s.L; k <= s.L; k++) { const double v = dsp::rrc(k / s.P, 0.5); s.h[(size_t)(k + s.L)] = (float)v; e += v * v; }
        for (auto& v : s.h) v = (float)(v / std::sqrt(e));
        s.z.reserve((size_t)(s.rs * 8));
        carriersSeen++;
        slots.push_back(std::move(s));
        dirty = true;
    }

    // ----- one message -----
    void fail(Slot& s, bool counted) {
        if (counted) badN++;
        s.state = 2;
        ignore.push_back({s.freq, timeSec + 1.5});
        dirty = true;
    }

    // the carrier frequency from the first 0.45 s of the slot (the 2 s carrier)
    void estimateCarrier(Slot& s) {
        const size_t a = (size_t)(0.02 * s.rs), b = (size_t)(0.45 * s.rs);
        std::complex<double> r1(0, 0);
        for (size_t n = a; n + 1 < b; n++) r1 += std::complex<double>(s.z[n + 1] * std::conj(s.z[n]));
        const double f1 = std::arg(r1) * s.rs / (2 * math::kPi);
        const int lag = 32;
        std::complex<double> rl(0, 0);
        for (size_t n = a; n + lag < b; n++) rl += std::complex<double>(s.z[n + lag] * std::conj(s.z[n]));
        rl *= std::polar(1.0, -2 * math::kPi * f1 * lag / s.rs);
        s.f0 = f1 + std::arg(rl) * s.rs / (2 * math::kPi * lag);
        s.haveF = true;
        s.drot = std::polar(1.f, (float)(-2 * math::kPi * s.f0 / s.rs));
        s.dph = cf32(1, 0);
        for (auto& v : s.z) { v *= s.dph; s.dph *= s.drot; }
        s.dph /= std::abs(s.dph);
    }

    void filterTo(Slot& s) {   // matched filter output up to what z allows
        const int64_t zEnd = s.zb + (int64_t)s.z.size();
        while (s.yNext + s.L < zEnd) {
            const int64_t c = s.yNext - s.zb;
            cf32 v(0, 0);
            for (int k = -s.L; k <= s.L; k++) {
                const int64_t j = c + k;
                if (j >= 0) v += s.z[(size_t)j] * s.h[(size_t)(k + s.L)];
            }
            s.y.push_back(v);
            s.yNext++;
        }
    }

    // preamble + marker: correlation in six pieces, summed without their phases (frequency errors of a few hertz cost nothing)
    bool findMarker(Slot& s) {
        const auto& r = refSymbols();
        int off[kRef];
        for (int k = 0; k < kRef; k++) off[k] = (int)std::lround(k * s.P);
        const int64_t last = (int64_t)s.y.size() - off[kRef - 1] - 2;
        double best = 0; int64_t bi = -1;
        cf32 bc[kChunks];
        for (int64_t i = (int64_t)(0.05 * s.rs); i < last; i++) {
            double m = 0, mag = 0;
            cf32 cc[kChunks];
            for (int c = 0; c < kChunks; c++) {
                cf32 a(0, 0);
                for (int k = c * kChunk; k < (c + 1) * kChunk; k++) { const cf32 v = s.y[(size_t)(i + off[k])]; a += v * (float)r[(size_t)k]; mag += std::abs(v); }
                cc[c] = a; m += std::abs(a);
            }
            const double rho = m / (mag + 1e-20);
            if (rho > best) { best = rho; bi = i; std::copy(cc, cc + kChunks, bc); }
        }
        if (bi < 0 || best < 0.5) return false;
        // phase and frequency from the pieces
        double ph[kChunks], xs[kChunks];
        double sumMag = 0;
        for (int c = 0; c < kChunks; c++) {
            ph[c] = std::arg(bc[c]); xs[c] = c * kChunk + 0.5 * (kChunk - 1); sumMag += std::abs(bc[c]);
            if (c > 0) { while (ph[c] - ph[c - 1] > math::kPi) ph[c] -= 2 * math::kPi; while (ph[c] - ph[c - 1] < -math::kPi) ph[c] += 2 * math::kPi; }
        }
        double mx = 0, my = 0;
        for (int c = 0; c < kChunks; c++) { mx += xs[c]; my += ph[c]; }
        mx /= kChunks; my /= kChunks;
        double sxy = 0, sxx = 0;
        for (int c = 0; c < kChunks; c++) { sxy += (xs[c] - mx) * (ph[c] - my); sxx += (xs[c] - mx) * (xs[c] - mx); }
        const double slope = sxx > 0 ? sxy / sxx : 0;   // rad per symbol
        s.omega = slope;
        s.theta = my + slope * (kRef - mx) - math::kPi / 4;   // the phase of g at the first data symbol (the pieces hold g (1 + j))
        s.amp = (float)(sumMag / (kRef * std::sqrt(2.0)));
        s.t = (double)bi + kRef * s.P;                         // y index (relative to zb) of the first data symbol on I
        s.Pn = s.P;
        s.state = 1;
        s.syncSec = timeSec;
        s.vit.reset();
        s.nsym = 0; s.hdrDone = false; s.needFrames = 1; s.havePrev = false; s.evm = 0;
        dirty = true;
        return true;
    }

    void demod(Slot& s) {
        const double kp = 0.0055, ki = 3.1e-5;      // carrier loop, about 5 Hz
        const double tkp = 0.02, tki = 0.0002;      // timing loop
        while (s.t + s.Pn * 0.5 + 3 < (double)s.y.size()) {
            const cf32 rA = std::polar(1.f / s.amp, (float)-s.theta), rB = std::polar(1.f / s.amp, (float)-(s.theta + 0.5 * s.omega));
            const cf32 a = interp(s.y, s.t) * rA, b = interp(s.y, s.t + 0.5 * s.Pn) * rB;
            const float I = a.real(), Q = b.imag();
            const double ep = (I >= 0 ? 1 : -1) * a.imag() - (Q >= 0 ? 1 : -1) * b.real();
            s.omega += ki * ep;
            s.theta += s.omega + kp * ep;
            if (s.theta > math::kPi) s.theta -= 2 * math::kPi; else if (s.theta < -math::kPi) s.theta += 2 * math::kPi;
            double et = 0;
            if (s.havePrev) et = s.prevB.real() * (s.prevI - I) + a.imag() * (s.prevQ - Q);
            et = std::max(-2.0, std::min(2.0, et));
            s.Pn += tki * et;
            s.Pn = std::max(s.P * 0.998, std::min(s.P * 1.002, s.Pn));
            s.t += s.Pn + tkp * et;
            s.prevB = b; s.prevI = I; s.prevQ = Q; s.havePrev = true;
            s.evm += ((std::fabs(I) - 1) * (std::fabs(I) - 1) + (std::fabs(Q) - 1) * (std::fabs(Q) - 1) - s.evm) * 0.002;
            auto q = [](float v) { return (int)std::max(-127.f, std::min(127.f, std::round(v * 32.f))); };
            s.vit.push(q(-I), q(Q));
            s.nsym++;
            // header: the first frame
            if (!s.hdrDone && s.vit.out().size() >= (size_t)hrdcp::kFrameCoded * 8) {
                hrdcp::Decoded d;
                hrdcp::decodeMessage(packBits(s.vit.out(), hrdcp::kFrameCoded), d);
                if (!d.headerOk) { fail(s, true); return; }
                s.hdrDone = true;
                s.needFrames = hrdcp::framesFor((size_t)hrdcp::kHeaderBytes + d.hdr.length + hrdcp::kCrcBytes);
                dirty = true;
            }
            if (s.hdrDone && s.nsym >= (uint64_t)s.needFrames * hrdcp::kFrameCoded * 8 + 8) {
                s.vit.flush();
                hrdcp::Decoded d;
                hrdcp::decodeMessage(packBits(s.vit.out(), (size_t)s.needFrames * hrdcp::kFrameCoded), d);
                finish(s, d);
                return;
            }
            if (s.nsym > (uint64_t)(70 * hrdcp::kSymbolRate)) { fail(s, true); return; }
        }
        // keep memory small: drop what the demodulator is past
        const int64_t drop = (int64_t)std::floor(s.t) - 64;
        if (drop > 40000) {
            s.z.erase(s.z.begin(), s.z.begin() + drop);
            s.y.erase(s.y.begin(), s.y.begin() + drop);
            s.zb += drop; s.t -= (double)drop;
        }
    }

    void finish(Slot& s, const hrdcp::Decoded& d) {
        s.state = 2;
        ignore.push_back({s.freq, timeSec + 1.5});
        dirty = true;
        if (!d.headerOk || !d.complete || !d.crcOk) { badN++; if (log) log(hrdcp::describe(d)); return; }
        DcsMessage m;
        m.id = nextId++;
        m.wallTime = baseUtc + (int64_t)std::floor(s.bornSec);
        m.address = d.hdr.address; m.addressOk = true; m.addressFixed = d.addressFixed;
        m.bps = 1200; m.manchester = false;
        const double rel = s.freq + s.f0;
        const double cen = centreReq.load();
        m.freqAbsolute = cen > 0;
        m.freqHz = cen > 0 ? cen + rel : rel;
        m.plan = (int)dcs::Plan::Meteosat;
        m.channel = cen > 0 ? dcs::channelOf(cen + rel, dcs::Plan::Meteosat) : 0;
        m.snrDb = s.snr;
        m.eot = true;
        m.sequence = d.hdr.sequence; m.hrType = d.hdr.type; m.health = d.hdr.health; m.compression = d.hdr.compression;
        m.crcOk = d.crcOk; m.frames = d.frames; m.framesBad = d.framesBad; m.rsCorrected = d.rsCorrected;
        m.raw = d.data;
        bool text = !d.data.empty();
        for (uint8_t c : d.data) if (c >= 0x80 || (c < 0x20 && c != '\r' && c != '\n' && c != '\t')) { text = false; break; }
        if (text) dcs::interpret(m);
        else {
            m.format = 2;
            char b[4];
            for (size_t i = 0; i < d.data.size() && i < 512; i++) { snprintf(b, sizeof b, "%02X", d.data[i]); m.text += b; if (i + 1 < d.data.size()) m.text += ' '; }
        }
        okN++;
        addMessage(m);
    }

    void addMessage(const DcsMessage& m) {
        msgs.insert(msgs.begin(), m);
        if (msgs.size() > kDcsMsgMax) msgs.pop_back();
        DcsPlatform* pl = nullptr;
        for (auto& p : plats) if (p.address == m.address) { pl = &p; break; }
        if (!pl) { plats.push_back(DcsPlatform()); pl = &plats.back(); pl->address = m.address; pl->firstTime = m.wallTime; }
        pl->count++; pl->lastTime = m.wallTime; pl->channel = m.channel; pl->bps = m.bps; pl->snrDb = m.snrDb; pl->format = m.format;
        std::string t = m.text;
        for (auto& c : t) if (c == '\n') c = ' ';
        pl->last = t.substr(0, 80);
        std::stable_sort(plats.begin(), plats.end(), [](const DcsPlatform& a, const DcsPlatform& b) { return a.lastTime > b.lastTime; });
        if (plats.size() > kDcsPlatMax) plats.pop_back();
        if (!logPath.empty()) {
            FILE* f = fopen(logPath.c_str(), "ab");
            if (f) {
                fseek(f, 0, SEEK_END);
                if (ftell(f) == 0) fputs("id,unix_time,address,channel,freq_hz,sequence,type,health,bytes,rs_corrected,format,data\n", f);
                std::string q;
                for (char c : m.text) { if (c == '"') q += "\"\""; else q += (c == '\n' ? ' ' : c); }
                fprintf(f, "%u,%lld,%s,%d,%.0f,%d,%s,%s,%zu,%d,%s,\"%s\"\n", m.id, (long long)m.wallTime, dcs::addressHex(m.address).c_str(), m.channel, m.freqHz, m.sequence,
                        m.hrType ? "alert" : "self-timed", hrdcp::healthText(m.health).c_str(), m.raw.size(), m.rsCorrected,
                        m.format == 2 ? "binary" : m.format == 1 ? "pseudo-binary" : "ascii", q.c_str());
                fclose(f);
            }
        }
        if (log) {
            char b[200];
            snprintf(b, sizeof b, "HRDCP: %s ch %d seq %d, %zu bytes, %.1f dB: %.60s", dcs::addressHex(m.address).c_str(), m.channel, m.sequence, m.raw.size(), (double)m.snrDb, m.text.c_str());
            log(b);
        }
        dirty = true;
    }

    void stepSlot(Slot& s) {
        if (s.state == 2) return;
        if (!s.haveF) {
            if ((double)s.z.size() < 0.46 * s.rs) return;
            estimateCarrier(s);
        }
        filterTo(s);
        if (s.state == 0) {
            const double have = (double)s.z.size() / s.rs;
            if ((s.tries == 0 && have >= 2.6) || (s.tries == 1 && have >= 4.5)) {
                s.tries++;
                if (!findMarker(s) && s.tries >= 2) { fail(s, false); return; }
            }
        }
        if (s.state == 1) demod(s);
    }

    void processSlots(const cf32* x, size_t n) {
        for (auto& s : slots) {
            if (s.state == 2) continue;
            for (size_t i = 0; i < n; i++) {
                cf32 v = x[i] * s.ph;
                s.ph *= s.rot;
                if (++s.renorm >= 2048) { s.renorm = 0; s.ph /= std::abs(s.ph); s.dph /= std::abs(s.dph); }
                cf32 zz;
                if (s.cic.push(v, zz)) {
                    if (s.haveF) { zz *= s.dph; s.dph *= s.drot; }
                    s.z.push_back(zz);
                    s.zTotal++;
                }
            }
            stepSlot(s);
        }
        const size_t before = slots.size();
        slots.erase(std::remove_if(slots.begin(), slots.end(), [](const Slot& s) { return s.state == 2; }), slots.end());
        if (slots.size() != before) dirty = true;
    }

    void publish() {
        pub.seq++;
        pub.state = okN ? 2 : (!slots.empty() ? 1 : 0);
        pub.dataValid = okN > 0;
        pub.blocksOk = okN; pub.blocksBad = badN;
        pub.timeSec = timeSec; pub.inputRate = rateSet; pub.workRate = fw; pub.binHz = binHz; pub.centreHz = centreReq.load();
        pub.noiseDb = noiseDb;
        pub.plan = (int)dcs::Plan::Meteosat;
        pub.carriersSeen = carriersSeen;
        pub.messages = msgs;
        pub.platforms = plats;
        pub.carriers.clear();
        float best = -1;
        for (const auto& s : slots) {
            DcsCarrier c;
            c.offsetHz = s.freq + s.f0;
            const double cen = centreReq.load();
            c.channel = cen > 0 ? dcs::channelOf(cen + c.offsetHz, dcs::Plan::Meteosat) : 0;
            c.snrDb = s.snr;
            c.state = s.state == 1 ? 2 : 0;
            c.bps = s.state == 1 ? 1200 : 0;
            c.bytes = s.state == 1 ? (uint32_t)(s.nsym / 8) : 0;
            pub.carriers.push_back(c);
            if (s.snr > best) { best = s.snr; bestSnr = s.snr; bestCfo = c.offsetHz; }
        }
        pub.cfoHz = bestCfo; pub.snrDb = bestSnr;
        pub.waterfall = wf;
        pub.spectrum = spec;
        pub.bandLowHz = -fw / 2; pub.bandHighHz = fw / 2;
        pub.logging = !logPath.empty(); pub.logPath = logPath;
        dirty = false;
    }
};

HrdcpReceiver::HrdcpReceiver() : p_(std::make_unique<Impl>()) {}
HrdcpReceiver::~HrdcpReceiver() = default;

void HrdcpReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->resetReq = true;
}
void HrdcpReceiver::setSignalOffset(double) {}
void HrdcpReceiver::setCenterHz(double hz) { p_->centreReq = hz; }
bool HrdcpReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= hrdcpTuning().minSampleRate - 1;
}
void HrdcpReceiver::reset() { p_->resetReq = true; }
void HrdcpReceiver::clearResults() { p_->clearReq = true; }
void HrdcpReceiver::setThresholdDb(double db) { p_->thrReq = std::max(6.0, std::min(25.0, db)); }
void HrdcpReceiver::setLogFile(const std::string& path) { std::lock_guard<std::mutex> lk(p_->mu); p_->logPathReq = path; p_->logReq = true; }
void HrdcpReceiver::setStartUtc(int64_t utcSec) { p_->startReq = utcSec; p_->resetReq = true; }
void HrdcpReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool HrdcpReceiver::telemetry(DcsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void HrdcpReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.logReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.logPath = m.logPathReq; m.dirty = true; }
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.resetState();
        m.publish();
    }
    if (m.clearReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.msgs.clear(); m.plats.clear(); m.okN = 0; m.badN = 0; m.nextId = 1;
        m.dirty = true;
    }
    if (n == 0 || m.rateSet < hrdcpTuning().minSampleRate - 1 || !m.fft) return;
    const size_t chunk = 65536;
    for (size_t off = 0; off < n; off += chunk) {
        const size_t k = std::min(chunk, n - off);
        m.fbuf.clear();
        for (size_t i = 0; i < k; i++) {
            cf32 v = x[off + i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            cf32 o;
            if (m.front.push(v, o)) m.fbuf.push_back(o);
        }
        m.nIn += k;
        std::lock_guard<std::mutex> lk(m.mu);
        for (size_t i = 0; i < m.fbuf.size(); i++) {
            m.fr[(size_t)m.fill] = m.fbuf[i] * m.win[(size_t)m.fill];
            if (++m.fill == m.N) {
                m.fill = 0;
                std::vector<cf32> t = m.fr;
                m.fft->forward(t.data());
                for (int j = 0; j < m.N; j++) m.acc[(size_t)j] += (double)std::norm(t[(size_t)j]);
                if (++m.frames >= m.M) {
                    m.frames = 0;
                    m.timeSec = ((double)(m.nIn - k) + (double)(i + 1) * m.D) / m.rateSet;
                    m.spectrumDone();
                    m.dirty = true;
                }
            }
        }
        m.timeSec = (double)m.nIn / m.rateSet;
        m.processSlots(m.fbuf.data(), m.fbuf.size());
        if (m.dirty || m.nIn >= m.nextPub) {
            m.nextPub = m.nIn + (uint64_t)(0.25 * m.rateSet);
            m.publish();
        }
    }
}

ModeTuning hrdcpTuning() {
    ModeTuning t;
    t.stdMode = 702; t.id = "hrdcp"; t.name = "Meteosat HRDCP";
    t.minMhz = 401.0; t.maxMhz = 403.0; t.defMhz = 402.3;   // the HRDCP part of the regional band, 402.2005 - 402.4345 MHz
    t.sampleRate = 1e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 48e3;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = true;
    return t;
}

} // namespace dect2
