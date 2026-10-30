// nRF24L01 Enhanced ShockBurst sniffer, see nrf24_rx.h
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/nrf24/nrf24_proto.h"
#include "dect2/nrf24/nrf24_tel.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace dect2 {

namespace {

// boxSym: the length of the average over the discriminator output before the slicer, in symbols (measured: 0.5 for 250 kbit/s, 1 for the faster rates)
struct RateDef { int kbps; double bps, devHz, cutoffHz, boxSym; };
const RateDef kRates[3] = {{250, 250e3, 160e3, 450e3, 0.5}, {1000, 1e6, 160e3, 900e3, 1.0}, {2000, 2e6, 320e3, 1500e3, 1.0}};
constexpr double kPreRollSec = 70e-6;   // before the detection point: the detector is up to about a time constant late
constexpr double kMaxBurstSec = 5e-3;
constexpr double kDetCutoffHz = 450e3;   // the burst detector looks at the signal through the narrowest of the filters

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// atan2 to about 1e-4 rad: the discriminator runs 25 million times a second
inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx < 1e-30f) return 0.f;
    const float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// two cascaded running sums of length L (a triangular low-pass), complex
struct Box2 {
    std::vector<float> b1r, b1i, b2r, b2i;
    double s1r = 0, s1i = 0, s2r = 0, s2i = 0;
    size_t p1 = 0, p2 = 0, n = 1;
    double inv = 1;
    void init(int L) {
        n = (size_t)L; inv = 1.0 / (double)L;
        b1r.assign(n, 0.f); b1i = b2r = b2i = b1r;
        s1r = s1i = s2r = s2i = 0; p1 = p2 = 0;
    }
    cf32 push(cf32 x) {
        s1r += (double)x.real() - b1r[p1]; s1i += (double)x.imag() - b1i[p1];
        b1r[p1] = x.real(); b1i[p1] = x.imag();
        if (++p1 == n) p1 = 0;
        const float m1r = (float)(s1r * inv), m1i = (float)(s1i * inv);
        s2r += (double)m1r - b2r[p2]; s2i += (double)m1i - b2i[p2];
        b2r[p2] = m1r; b2i[p2] = m1i;
        if (++p2 == n) p2 = 0;
        return cf32((float)(s2r * inv), (float)(s2i * inv));
    }
};

// a burst detector: the signal through a filter, its power averaged over a few independent samples of the filtered noise (or noise alone would trigger),
// against a noise floor that follows the quiet times. activity() = power / floor.
struct Det {
    Box2 box;
    float pe = 0, nf = 0, aPe = 0.1f, aDown = 0.01f, aUp = 1e-6f;
    bool init = false;
    uint64_t count = 0, warm = 0;
    double tau = 13e-6;
    void setup(double fs, double cutoffHz) {
        box.init(std::max(1, (int)std::lround(0.32 * fs / cutoffHz)));
        tau = 6.0 / cutoffHz;
        aPe = (float)(1 - std::exp(-1.0 / (tau * fs)));
        aDown = (float)(1 - std::exp(-1.0 / (20e-6 * fs)));
        aUp = (float)(1 - std::exp(-1.0 / (50e-3 * fs)));
        pe = nf = 0; init = false; count = 0; warm = (uint64_t)(300e-6 * fs);
    }
    float update(cf32 m, bool inBurst) {
        const cf32 y = box.push(m);
        pe += (std::norm(y) - pe) * aPe;
        if (!init) { nf = pe; if (++count > warm) init = true; return 0.f; }
        if (!inBurst) { if (pe < nf) nf += (pe - nf) * aDown; else nf += (pe - nf) * aUp; }
        return pe / (nf + 1e-12f);
    }
};

struct Path {
    int idx = 0;
    int D = 1;
    double r2 = 0, sps = 8;
    int boxLen = 1;
};

} // namespace

struct Nrf24Receiver::Impl {
    std::mutex mu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0;
    bool running = false;
    cf32 rot{1, 0}, rotStep{1, 0};
    Path path[3];
    uint64_t nIn = 0, nextReport = 0;
    // the burst detector: the signal through a 450 kHz filter (a wider one, 1.5 MHz, was tried for short 2 Mbit/s frames and made the decoding worse)
    Det det[1];
    float peak = 0;
    bool inB = false;
    uint64_t bSamples = 0, lastAbove = 0;
    int below = 0, hold = 1;
    double tau = 13e-6;
    float snr = 0;
    // the raw samples: a ring for the pre-roll, then the burst. The three demodulators run on them when the burst has ended.
    std::vector<cf32> ring, burstRaw;
    size_t rpos = 0, rfill = 0, maxRaw = 0, preRaw = 0;
    std::vector<float> dBuf, pBuf;   // discriminator output and power of one path
    // an episode is one burst
    bool epActive = false, epGot = false, epStrong = false;
    uint64_t epStart = 0;
    std::vector<std::pair<nrf24::Candidate, int>> epWeak;   // CRC-8 candidates: used only when no CRC-16 frame explains the burst
    float burstSnr = 0;
    std::deque<std::pair<std::string, uint64_t>> recentKeys;   // frames already taken from another path (key, input sample)
    double powAcc = 0; uint64_t powN = 0;
    float snrEma = 0;
    // results
    std::deque<Nrf24Frame> frames;
    std::deque<Nrf24Device> devices;
    std::deque<Nrf24Burst> bursts;
    uint64_t ok = 0, bad = 0, nBursts = 0, n16 = 0, n8 = 0, nr[3] = {0, 0, 0};
    double lastFrameSec = -1e9, lastBurstSec = -1e9, lastCfo = 0;
    std::map<std::string, int> knownAddr;   // addresses found, with their CRC size (a chip keeps one)
    std::map<std::string, int> pending;
    uint64_t seq = 0;
    Nrf24Telemetry pub;

    void say(const std::string& s) { if (log) log(s); }
    double nowSec() const { return fs > 0 ? (double)nIn / fs : 0; }

    void rebuild() {
        running = fs >= nrf24Tuning().minSampleRate - 1;
        if (!running) { if (fs > 0) say("nRF24: the sample rate is too low (4 Msps at least)"); return; }
        for (int i = 0; i < 3; i++) {
            Path& p = path[i];
            p.idx = i;
            p.D = std::max(1, (int)(fs / (kRates[i].bps * 12)));
            p.r2 = fs / p.D;
            p.sps = p.r2 / kRates[i].bps;
            p.boxLen = std::max(1, (int)std::lround(0.32 * fs / kRates[i].cutoffHz));
        }
        det[0].setup(fs, kDetCutoffHz);
        tau = det[0].tau;
        hold = std::max(1, (int)(8e-6 * fs));
        inB = false; below = 0; bSamples = 0; snr = 0;
        ring.assign((size_t)std::ceil(kPreRollSec * fs) + 16, cf32(0, 0)); rpos = 0; rfill = 0;
        maxRaw = (size_t)(kMaxBurstSec * fs);
        burstRaw.clear(); burstRaw.reserve(maxRaw + ring.size());
        epActive = false; recentKeys.clear(); epWeak.clear();
        const double w = -2 * math::kPi * offsetHz / fs;
        rotStep = cf32((float)std::cos(w), (float)std::sin(w)); rot = cf32(1, 0);
        nIn = 0; nextReport = 0;
        say("nRF24: listening for Enhanced ShockBurst frames (GFSK 250 kbit/s, 1 and 2 Mbit/s tried together, CRC-8 and CRC-16, addresses of 3 to 5 bytes)");
    }

    // one path's discriminator output for the stored burst (filter, decimate, FM discriminator)
    void demod(const Path& p, std::vector<float>& d, std::vector<float>& pw) {
        Box2 box;
        box.init(p.boxLen);
        cf32 prev(1, 0);
        d.clear(); pw.clear();
        d.reserve(burstRaw.size() / (size_t)p.D + 1); pw.reserve(burstRaw.size() / (size_t)p.D + 1);
        int dc = 0;
        const float k = (float)(p.r2 / (2 * math::kPi));
        for (const cf32 x : burstRaw) {
            const cf32 y = box.push(x);
            if (++dc < p.D) continue;
            dc = 0;
            const cf32 z = y * std::conj(prev);
            prev = y;
            d.push_back(fastAtan2(z.imag(), z.real()) * k);
            pw.push_back(std::norm(y));
        }
    }

    // the bits of one path's burst: levels from the burst itself, then a clock loop on the transitions. pre = where the detector fired, activeEnd = where the signal ended
    std::vector<float> smBuf, selBuf;   // scratch, kept between bursts
    bool slice(const Path& p, const std::vector<float>& d, const std::vector<float>& pw, size_t pre, size_t activeEnd, bool invert, std::vector<uint8_t>& bits, double& midOut) {
        const size_t n = d.size();
        if (n < 32) return false;
        const size_t b = std::max<size_t>(1, (size_t)std::lround(p.sps * kRates[p.idx].boxSym));
        std::vector<float>& sm = smBuf;
        sm.resize(n);
        double acc = 0;
        for (size_t i = 0; i < n; i++) { acc += d[i]; if (i >= b) acc -= d[i - b]; sm[i] = (float)(acc / (double)std::min(i + 1, b)); }
        pre = std::min(pre, n);
        const size_t a0 = std::min(n, pre + b), a1 = std::max(a0 + 1, std::min(n, activeEnd));
        if (a1 <= a0 + 16) return false;
        std::vector<float>& s = selBuf;
        s.assign(sm.begin() + (long)a0, sm.begin() + (long)a1);
        const size_t iLo = (size_t)(0.04 * (double)(s.size() - 1)), iHi = (size_t)(0.96 * (double)(s.size() - 1));
        std::nth_element(s.begin(), s.begin() + (long)iHi, s.end());
        const float hi = s[iHi];
        std::nth_element(s.begin(), s.begin() + (long)iLo, s.begin() + (long)iHi);
        const float lo = s[iLo];
        if (hi - lo < 0.6 * (float)kRates[p.idx].devHz) return false;   // no two-level FSK of this size here
        const float mid = 0.5f * (hi + lo);
        midOut = mid;
        bits.clear();
        bool prevLev = false, haveLev = false;
        float ph = 0;
        bool locked = false;
        // where the signal really starts: the first place after the detector's pre-roll where the power (averaged over a few microseconds) is over half of
        // the burst's own level. The slicer starts there, so that noise before the burst does not move its clock.
        size_t start = pre;
        {
            const size_t pb = std::max<size_t>(1, (size_t)std::lround(std::max(p.sps * 0.5, 3e-6 * p.r2)));
            double mean = 0;
            const size_t m0 = a0 + (a1 - a0) / 4, m1 = a1 - (a1 - a0) / 4;
            for (size_t i = m0; i < m1; i++) mean += pw[i];
            mean /= (double)std::max<size_t>(1, m1 - m0);
            const size_t lim = std::min(pre + (size_t)std::ceil(tau * p.r2), n);
            double ps = 0;
            size_t first = 0;
            for (size_t i = 0; i < lim; i++) {
                ps += pw[i]; if (i >= pb) ps -= pw[i - pb];
                if (ps / (double)std::min(i + 1, pb) > mean / 2) { first = i; break; }
            }
            start = first > pb / 2 ? first - pb / 2 : 0;
        }
        for (size_t i = start; i < a1; i++) {
            const bool lev = (sm[i] > mid) != invert;
            if (!haveLev) { prevLev = lev; haveLev = true; continue; }
            if (!locked) {
                if (lev != prevLev) { locked = true; ph = 0; prevLev = lev; }   // the first transition is a bit edge
                continue;
            }
            if (lev != prevLev) ph *= 0.65f;
            prevLev = lev;
            ph += (float)(1.0 / p.sps);
            if (ph >= 0.5f) { ph -= 1.f; bits.push_back(lev ? 1 : 0); }
        }
        return bits.size() >= 40;
    }

    void addFrame(const nrf24::Candidate& c, int rateIdx) {
        const nrf24::Frame& f = c.frame;
        const std::string addr = nrf24::hex(f.address);
        const std::string content = addr + "/" + std::to_string(f.pid) + "/" + std::to_string(f.noAck) + "/" + nrf24::hex(f.payload);
        bool confirmed = true;
        if (c.weak) {
            // CRC-8 is 1 in 256 by chance per candidate: accept only an address already known, or the same content a third time
            const auto k = knownAddr.find(addr);
            if (k != knownAddr.end() && k->second == 16) return;   // this address sends CRC-16 frames: a CRC-8 match is a damaged frame that passed by chance
            if (k == knownAddr.end()) {
                if (pending.size() > 256) pending.clear();
                if (++pending[content] < 3) return;   // the third sight of the same frame
                pending.erase(content);
            }
        }
        if (knownAddr.size() > 500) knownAddr.clear();
        knownAddr[addr] = f.crcBits;
        ok++;
        (f.crcBits == 16 ? n16 : n8)++;
        nr[rateIdx]++;
        lastFrameSec = nowSec();
        const int64_t utc = wallNow();
        if (!frames.empty() && frames.front().address == addr && frames.front().payloadHex == nrf24::hex(f.payload) && frames.front().pid == f.pid && frames.front().noAck == f.noAck && nowSec() - frames.front().timeSec < 2.0) {
            frames.front().repeats++;
            frames.front().timeSec = nowSec();
        } else {
            Nrf24Frame e;
            e.utc = utc; e.timeSec = nowSec(); e.rateKbps = kRates[rateIdx].kbps; e.address = addr; e.addrLen = (int)f.address.size(); e.payloadLen = (int)f.payload.size();
            e.pid = f.pid; e.noAck = f.noAck; e.crcBits = f.crcBits; e.payloadHex = nrf24::hex(f.payload); e.text = nrf24::printable(f.payload); e.snrDb = burstSnr; e.confirmed = confirmed;
            frames.push_front(e);
            if (frames.size() > kNrf24FramesMax) frames.pop_back();
        }
        Nrf24Device* dv = nullptr;
        for (auto& x : devices) if (x.address == addr) { dv = &x; break; }
        if (!dv) {
            devices.push_front(Nrf24Device());
            dv = &devices.front();
            dv->address = addr; dv->addrLen = (int)f.address.size(); dv->firstSeen = utc;
            if (devices.size() > kNrf24DevicesMax) devices.pop_back();
            say("nRF24: new address " + addr + " (" + std::to_string(kRates[rateIdx].kbps) + " kbit/s, CRC-" + std::to_string(f.crcBits) + ")");
        }
        dv->count++; dv->lastSeen = utc; dv->rateKbps = kRates[rateIdx].kbps; dv->crcBits = f.crcBits; dv->lastPayloadLen = (int)f.payload.size();
    }


    // a frame is taken once even when more than one path found it (a signal two filters both see)
    bool takeFrame(const nrf24::Candidate& c, int rateIdx) {
        const std::string key = nrf24::hex(c.frame.address) + "/" + nrf24::hex(c.frame.payload) + "/" + std::to_string(c.frame.pid) + "/" + std::to_string(c.frame.noAck);
        for (const auto& r : recentKeys) if (r.first == key && nIn - r.second < (uint64_t)(2e-3 * fs)) return false;
        recentKeys.push_back({key, nIn});
        if (recentKeys.size() > 16) recentKeys.pop_front();
        const uint64_t before = ok;
        addFrame(c, rateIdx);
        if (ok != before) { epGot = true; return true; }
        return false;
    }

    // the burst has ended: the three demodulators run on the stored samples, then the sniffer on the bits of each
    void endBurst() {
        inB = false;
        const double dur = (double)lastAbove / fs;
        if (dur >= 20e-6) {
            static const int order[3] = {1, 2, 0};   // 1 and 2 Mbit/s are the common ones
            for (int oi = 0; oi < 3; oi++) {
                const int pi = order[oi];
                const Path& p = path[pi];
                demod(p, dBuf, pBuf);
                const size_t pre = preRaw / (size_t)p.D, activeEnd = (preRaw + (size_t)lastAbove) / (size_t)p.D  + 1;   // the averages lag: the last bits are decided a little after the signal ends
                std::vector<uint8_t> bits;
                double mid = 0;
                std::vector<nrf24::Candidate> cand;
                for (int pol = 0; pol < 2; pol++) {   // the radio may give the spectrum mirrored: then the bits are inverted
                    if (!slice(p, dBuf, pBuf, pre, activeEnd, pol == 1, bits, mid)) continue;
                    std::vector<nrf24::Candidate> c = nrf24::sniff(bits);
                    bool strong = false;
                    for (const auto& x : c) if (!x.weak) strong = true;
                    if (strong) lastCfo = mid;
                    cand.insert(cand.end(), c.begin(), c.end());
                }
                burstSnr = snr;
                for (auto& c : cand) {
                    // the burst must be as long as the frame the candidate claims, at this path's data rate (a frame read at the wrong rate, or a chance match, is not)
                    const double bitsTotal = 8 + 8 * (double)c.frame.address.size() + 9 + 8 * (double)c.frame.payload.size() + c.frame.crcBits;
                    const double expect = bitsTotal / kRates[pi].bps, tol = 0.2 * expect + 3 * tau + 6e-6;
                    if (std::fabs(dur - expect) > tol) continue;
                    if (c.weak) { epWeak.push_back({c, pi}); continue; }
                    takeFrame(c, pi);
                    epStrong = true;
                }
                if (epStrong) break;   // a CRC-16 frame explains the burst: the other rates are not tried
            }
        }
        burstRaw.clear();
        if (epActive) {
            epActive = false;
            if (!epStrong) for (auto& w : epWeak) takeFrame(w.first, w.second);   // CRC-8 candidates count only when no CRC-16 frame explains the burst
            epWeak.clear();
            if (!epGot && dur >= 20e-6) {
                bad++;
                Nrf24Burst b;
                b.utc = wallNow(); b.timeSec = nowSec(); b.durationUs = dur * 1e6; b.snrDb = snr;
                bursts.push_front(b);
                if (bursts.size() > kNrf24BurstsMax) bursts.pop_back();
            }
        }
    }

    void sample(const cf32 m) {
        powAcc += std::norm(m); powN++;
        ring[rpos] = m;
        if (++rpos == ring.size()) rpos = 0;
        if (rfill < ring.size()) rfill++;
        const float act = det[0].update(m, inB);   // power over noise floor
        auto snrNow = [&] { return (float)(10 * std::log10(std::max(1e-3f, act - 1))); };
        if (!inB) {
            if (act > 6.f) {
                inB = true; below = 0; bSamples = 0; peak = act; lastAbove = 0; snr = snrNow();
                if (!epActive) { epActive = true; epGot = false; epStrong = false; epWeak.clear(); nBursts++; }
                lastBurstSec = nowSec();
                burstRaw.clear();   // the pre-roll, oldest first (the newest sample is in it)
                const size_t nR = ring.size(), first = rfill < nR ? 0 : rpos, cnt = std::min(rfill, nR);
                for (size_t k = 0; k < cnt; k++) burstRaw.push_back(ring[(first + k) % nR]);
                preRaw = burstRaw.size();
            }
        } else {
            bSamples++;
            if (burstRaw.size() < maxRaw + ring.size()) burstRaw.push_back(m);
            if (act > peak) peak = act;
            if (act > 0.5f * peak) lastAbove = bSamples;
            lastBurstSec = nowSec();
            if ((bSamples & 255) == 0) { snr = 0.9f * snr + 0.1f * snrNow(); snrEma = snr; }
            if (act < 3.f) { if (++below >= hold) endBurst(); } else below = 0;
        }
    }

    void process(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const cf32 m = x[i] * rot;
            rot *= rotStep;
            sample(m);
            nIn++;
        }
        rot /= std::abs(rot);
    }

    void publish() {
        const double t = nowSec();
        Nrf24Telemetry o;
        o.timeSec = t; o.inputRate = fs;
        o.dataValid = t - lastFrameSec < 30.0;
        o.state = o.dataValid ? 2 : (t - lastBurstSec < 30.0 ? 1 : 0);
        o.cfoHz = lastCfo; o.snrDb = snrEma;
        o.levelDb = powN ? (float)(10 * std::log10(powAcc / (double)powN + 1e-20)) : -200.f;
        powAcc = 0; powN = 0;
        o.blocksOk = ok; o.blocksBad = bad; o.bursts = nBursts; o.crc16Frames = n16; o.crc8Frames = n8; o.frames250 = nr[0]; o.frames1M = nr[1]; o.frames2M = nr[2];
        o.frames.assign(frames.begin(), frames.end());
        o.devices.assign(devices.begin(), devices.end());
        o.burstLog.assign(bursts.begin(), bursts.end());
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }
};

Nrf24Receiver::Nrf24Receiver() : p_(std::make_unique<Impl>()) {}
Nrf24Receiver::~Nrf24Receiver() = default;

void Nrf24Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->fs = r; p_->rebuild(); }
void Nrf24Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetHz) { p_->offsetHz = hz; if (p_->fs > 0) p_->rebuild(); } }
bool Nrf24Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->running; }
void Nrf24Receiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); if (p_->fs > 0) p_->rebuild(); }
void Nrf24Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void Nrf24Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.frames.clear(); m.devices.clear(); m.bursts.clear(); m.knownAddr.clear(); m.pending.clear();
    m.ok = m.bad = m.nBursts = m.n16 = m.n8 = 0; m.nr[0] = m.nr[1] = m.nr[2] = 0; m.lastFrameSec = m.lastBurstSec = -1e9;
    m.publish();
}

void Nrf24Receiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.running || n == 0) return;
    m.process(x, n);
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.fs;
        m.publish();
    }
}

void Nrf24Receiver::flush() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->inB) p_->endBurst();
    p_->publish();
}

bool Nrf24Receiver::telemetry(Nrf24Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning nrf24Tuning() {
    ModeTuning t;
    t.stdMode = 211; t.id = "nrf24"; t.name = "nRF24L01";
    t.minMhz = 2400; t.maxMhz = 2526; t.defMhz = 2402;
    t.sampleRate = 8e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 5;
    t.minSampleRate = 4e6;
    t.tuneOffsetHz = 1.5e6;   // the radio sits 1.5 MHz above the channel: the signal (+-1.3 MHz at 2 Mbit/s) stays clear of the DC spike
    return t;
}

} // namespace dect2
