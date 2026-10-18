// Wireless M-Bus receiver for the engine (see wmbus_live.h).
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_tel.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace dect2 {

namespace {
using dect2::math::kPi;
using namespace wmbus;

int64_t wallSec() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int pop64(uint64_t x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
};
struct BiState { double z1 = 0, z2 = 0; };
inline double biRun(const Biquad& b, BiState& s, double x) {
    const double y = b.b0 * x + s.z1;
    s.z1 = b.b1 * x - b.a1 * y + s.z2;
    s.z2 = b.b2 * x - b.a2 * y;
    return y;
}

constexpr size_t kMaxChips = 1000;
}

struct WmbusLiveReceiver::Impl {
    // ---- one chip demodulator: clock recovery, sync search and byte assembly for one mode of one channel
    struct Chip {
        Phy phy = Phy::S1;
        bool tc = false;                 // T1 and C1 share the sync word: the format marker tells them apart
        double chipRate = 0, sps = 1, phase = 0, accSum = 0;
        int accN = 0;
        bool prevSlice = false;
        uint64_t sh = 0;
        int seen = 0;
        bool collecting = false, invert = false;
        Coding coding = Coding::Nrz;
        bool formatB = false;
        std::vector<uint8_t> pend;       // T / C: the first 8 chips, until the marker is known
        bool decided = false;
        int chipIdx = 0;                 // chips in the current symbol
        unsigned symAcc = 0;
        int nib = -1;
        int manPrev = -1;
        std::vector<uint8_t> bytes;
        size_t need = 0;
        Phy usedPhy = Phy::S1;
        float rssi = -200;
        double cfoHz = 0;
        std::vector<float> soft;
    };
    struct Chan {
        ChannelDef def;
        double offHz = 0;
        double loRe = 1, loIm = 0, stRe = 1, stIm = 0;
        int loCount = 0;
        double accRe = 0, accIm = 0;
        int accN = 0;
        int64_t nIn = 0, k = 0;
        double binEnd = 0, binW = 1;
        Biquad bq[2];
        BiState sI[2], sQ[2];
        double prevRe = 0, prevIm = 0;
        bool havePrev = false;
        double dc = 0, dcAlpha = 0.03;
        double pSlow = 0, pFast = 0, fastAlpha = 0.01, noise = 1e-12;
        int nColl = 0;
        std::vector<Chip> chips;
        uint64_t frames = 0, bad = 0;
        int index = 0;
    };

    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0;
    std::atomic<double> centreReq{0};
    std::atomic<bool> resetReq{true}, clearReq{false};

    double rate = 0, centre = 0;
    std::vector<Chan> chans;
    int64_t nIn = 0;
    double nextReport = 0, nextSecond = 1;
    bool dirty = true;
    WmbusTelemetry pub;
    std::deque<WmbusFrameLog> flog;
    std::vector<WmbusDevice> devs;
    std::deque<float> levelHist;
    std::vector<int16_t> trace;
    uint64_t okN = 0, badN = 0, encN = 0, syncN = 0, falseN = 0;
    double lastCfo = 0;
    float bestSnr = 0, bestLevel = -200;
    int stateNow = 0;

    double tNow() const { return rate > 0 ? (double)nIn / rate : 0; }

    void resetState() {
        rate = rateSet;
        centre = centreReq.load();
        chans.clear();
        nIn = 0; nextReport = 0; nextSecond = 1;
        if (rate <= 0) return;
        const auto plan = channelPlan(centre, rate);
        int idx = 0;
        for (const auto& d : plan) {
            Chan c;
            c.def = d;
            c.index = idx++;
            const double c0 = centre > 0 ? centre : 868.625e6;
            c.offHz = d.freqHz - c0;
            const double w = -2 * kPi * c.offHz / rate;
            c.stRe = std::cos(w); c.stIm = std::sin(w);
            c.binW = rate / d.fsOut;
            c.binEnd = c.binW;
            const double q[2] = {0.5412, 1.3066};
            for (int i = 0; i < 2; i++) c.bq[i].lowpass(d.lpfHz, d.fsOut, q[i]);
            double slowest = 1e9;
            for (Phy p : d.phys) slowest = std::min(slowest, phySpec(p).chipRate);
            c.dcAlpha = 1.0 / (8.0 * d.fsOut / slowest);
            c.fastAlpha = 1.0 / (16.0 * d.fsOut / slowest);
            bool tcDone = false;
            for (Phy p : d.phys) {
                if ((p == Phy::T1 || p == Phy::C1A) && tcDone) continue;
                Chip ch;
                ch.phy = p == Phy::C1A ? Phy::T1 : p;
                ch.tc = (p == Phy::T1 || p == Phy::C1A);
                tcDone |= ch.tc;
                ch.chipRate = phySpec(p).chipRate;
                ch.sps = d.fsOut / ch.chipRate;
                c.chips.push_back(ch);
            }
            chans.push_back(std::move(c));
        }
        // the table of channels for the interface
        pub.channels.clear();
        for (const auto& c : chans) {
            WmbusChannelInfo ci;
            ci.name = c.def.label; ci.freqHz = c.def.freqHz;
            for (Phy p : c.def.phys) { if (!ci.modes.empty()) ci.modes += " "; ci.modes += phyName(p); }
            std::string m;
            for (Phy p : c.def.phys) { if (p == Phy::C1A) m += "C1"; else { if (!m.empty()) m += " "; m += phyName(p); } }
            ci.modes = m;
            pub.channels.push_back(ci);
        }
        dirty = true;
    }

    float toDb(double p) const { return p > 1e-20 ? (float)(10 * std::log10(p)) : -200.f; }

    void publish() {
        pub.seq++;
        pub.state = okN || badN ? 2 : (bestSnr > 10 ? 1 : 0);
        pub.cfoHz = lastCfo;
        pub.snrDb = bestSnr;
        pub.levelDb = bestLevel;
        pub.dataValid = okN > 0;
        pub.blocksOk = okN; pub.blocksBad = badN;
        pub.timeSec = tNow(); pub.inputRate = rate; pub.centreHz = centre;
        pub.syncs = syncN; pub.encryptedFrames = encN; pub.falseSync = falseN;
        for (size_t i = 0; i < chans.size() && i < pub.channels.size(); i++) {
            pub.channels[i].levelDb = toDb(chans[i].pSlow);
            pub.channels[i].frames = chans[i].frames; pub.channels[i].bad = chans[i].bad;
        }
        pub.devices = devs;
        pub.log.assign(flog.begin(), flog.end());
        pub.levelHist.assign(levelHist.begin(), levelHist.end());
        pub.trace = trace;
    }

    void abortFrame(Chan& c, Chip& ch, bool falseSync) {
        if (ch.collecting) c.nColl--;
        ch.collecting = false;
        ch.seen = 0;
        if (falseSync) falseN++;
    }

    void finish(Chan& c, Chip& ch) {
        Frame f;
        const bool fb = ch.formatB;
        const bool ok = parseFrame(ch.bytes.data(), ch.bytes.size(), fb, f);
        const Phy ph = ch.tc ? (fb ? Phy::C1B : (ch.coding == Coding::ThreeOfSix ? Phy::T1 : Phy::C1A)) : ch.phy;
        const int64_t now = wallSec();
        WmbusFrameLog lg;
        lg.wallTime = now; lg.phy = (int)ph; lg.channel = c.index; lg.rssiDb = ch.rssi; lg.crcOk = ok;
        lg.hex = f.hex();
        if (lg.hex.empty()) { char b[4]; for (uint8_t x : ch.bytes) { snprintf(b, sizeof b, "%02X", x); lg.hex += b; } }
        if (!ok) {
            badN++; c.bad++;
            lg.summary = f.error.empty() ? "CRC failed" : f.error;
            lg.mfr = f.mfr; lg.id = f.id;
        } else {
            okN++; c.frames++;
            lg.mfr = f.mfr; lg.id = f.id; lg.type = f.typeName; lg.encrypted = f.encrypted;
            std::string s;
            if (f.encrypted) { s = "encrypted payload"; encN++; }
            else {
                for (size_t i = 0; i < f.records.size() && i < 4; i++) { if (i) s += "; "; s += f.records[i].quantity + " " + f.records[i].value; }
                if (f.records.empty()) s = f.payload.empty() ? "no data" : (f.hasManufacturerData ? "manufacturer data" : f.ciName);
            }
            lg.summary = s;
            WmbusDevice* dv = nullptr;
            for (auto& d : devs) if (d.id == f.id && d.mfr == f.mfr && d.type == f.typeName) { dv = &d; break; }
            if (!dv) { devs.push_back(WmbusDevice()); dv = &devs.back(); dv->mfr = f.mfr; dv->id = f.id; dv->type = f.typeName; dv->firstSeen = now; }
            dv->version = f.version; dv->phy = (int)ph; dv->count++; dv->lastSeen = now; dv->rssiDb = ch.rssi;
            dv->encrypted = f.encrypted; dv->last = s; dv->records = f.records; dv->status = f.statusText;
            std::stable_sort(devs.begin(), devs.end(), [](const WmbusDevice& a, const WmbusDevice& b) { return a.lastSeen > b.lastSeen; });
            if (devs.size() > kWmbusDevMax) devs.pop_back();
            if (log) { char b[200]; snprintf(b, sizeof b, "wM-Bus %s %s %s: %s", phyName(ph), f.mfr.c_str(), f.id.c_str(), s.c_str()); log(b); }
        }
        flog.push_front(lg);
        while (flog.size() > kWmbusLogMax) flog.pop_back();
        lastCfo = ch.cfoHz;
        trace.clear();
        for (float v : ch.soft) trace.push_back((int16_t)std::max(-32000.f, std::min(32000.f, v * 16384.f)));
        dirty = true;
        abortFrame(c, ch, false);
    }

    // one decoded byte; returns false when the frame is complete or dropped
    void addByte(Chan& c, Chip& ch, uint8_t b) {
        ch.bytes.push_back(b);
        if (ch.bytes.size() == 1) {
            if (b < 9) { abortFrame(c, ch, true); return; }
            ch.need = airLength(b, ch.formatB);
            if (ch.need > 300) { abortFrame(c, ch, true); return; }
        }
        if (ch.bytes.size() >= ch.need && ch.need) finish(c, ch);
    }

    void symbolChip(Chan& c, Chip& ch, int bit) {
        switch (ch.coding) {
        case Coding::Nrz:
            ch.symAcc = (ch.symAcc << 1) | (unsigned)bit;
            if (++ch.chipIdx == 8) { const uint8_t b = (uint8_t)ch.symAcc; ch.chipIdx = 0; ch.symAcc = 0; addByte(c, ch, b); }
            break;
        case Coding::Manchester:
            if (ch.manPrev < 0) { ch.manPrev = bit; break; }
            if (ch.manPrev == bit) { abortFrame(c, ch, true); return; }
            ch.symAcc = (ch.symAcc << 1) | (unsigned)ch.manPrev;
            ch.manPrev = -1;
            if (++ch.chipIdx == 8) { const uint8_t b = (uint8_t)ch.symAcc; ch.chipIdx = 0; ch.symAcc = 0; addByte(c, ch, b); }
            break;
        case Coding::ThreeOfSix:
            ch.symAcc = (ch.symAcc << 1) | (unsigned)bit;
            if (++ch.chipIdx == 6) {
                const int n = decode36(ch.symAcc);
                ch.chipIdx = 0; ch.symAcc = 0;
                if (n < 0) { abortFrame(c, ch, true); return; }
                if (ch.nib < 0) ch.nib = n; else { const uint8_t b = (uint8_t)(ch.nib << 4 | n); ch.nib = -1; addByte(c, ch, b); }
            }
            break;
        }
    }

    void onChip(Chan& c, Chip& ch, int bit, double soft) {
        ch.sh = (ch.sh << 1) | (uint64_t)bit;
        if (ch.seen < 64) ch.seen++;
        if (!ch.collecting) {
            if (ch.seen < 48) return;
            const PhySpec& sp = phySpec(ch.tc ? Phy::T1 : ch.phy);
            const uint64_t w = ch.sh & 0xFFFF;
            const int e = pop64(w ^ sp.sync), ei = pop64((~w & 0xFFFF) ^ sp.sync);
            if (e > 1 && ei > 1) return;
            const bool inv = ei < e;
            uint64_t pre = (ch.sh >> 16) & 0xFFFFFFFFull;
            if (inv) pre = ~pre & 0xFFFFFFFFull;
            if (pop64((pre ^ (pre >> 1)) & 0x7FFFFFFFull) < 26) return;
            if (!c.def.wide) {   // 12.5 kHz neighbours overlap: the strongest of them owns the signal
                for (const auto& o : chans) if (&o != &c && !o.def.wide && std::fabs(o.def.freqHz - c.def.freqHz) < 15e3 && o.pFast > 1.5 * c.pFast) return;
            }
            ch.collecting = true; ch.invert = inv; c.nColl++;
            syncN++;
            ch.formatB = false; ch.decided = !ch.tc; ch.coding = ch.tc ? Coding::Nrz : sp.coding;
            if (!ch.tc) ch.coding = sp.coding;
            if (ch.phy == Phy::N1) ch.coding = Coding::Nrz;
            ch.pend.clear(); ch.bytes.clear(); ch.need = 0; ch.chipIdx = 0; ch.symAcc = 0; ch.nib = -1; ch.manPrev = -1; ch.soft.clear();
            ch.rssi = toDb(c.pFast);
            ch.cfoHz = c.dc * c.def.fsOut / (2 * kPi);
            return;
        }
        if (ch.soft.size() < kMaxChips) ch.soft.push_back((float)(ch.invert ? -soft : soft));
        const int b = ch.invert ? !bit : bit;
        if (ch.tc && !ch.decided) {
            ch.pend.push_back((uint8_t)b);
            if (ch.pend.size() < 8) return;
            unsigned v = 0;
            for (size_t i = 0; i < ch.pend.size(); i++) v = (v << 1) | ch.pend[i];
            // mode C: 0x54 then 0xCD (format A) or 0x3D (format B); 010101 is not a 3-of-6 word, so T1 never starts with 0x54
            if (ch.pend.size() == 8 && v == 0x54) return;
            if (ch.pend.size() > 8 && ch.pend.size() < 16) return;
            ch.decided = true;
            if (ch.pend.size() == 16 && ((v & 0xFF) == 0xCD || (v & 0xFF) == 0x3D)) { ch.coding = Coding::Nrz; ch.formatB = (v & 0xFF) == 0x3D; }
            else {
                ch.coding = Coding::ThreeOfSix;
                std::vector<uint8_t> pend = ch.pend;
                for (uint8_t x : pend) { if (!ch.collecting) return; symbolChip(c, ch, x); }
            }
            return;
        }
        symbolChip(c, ch, b);
    }

    // one working-rate sample of a channel
    void sampleOut(Chan& c, double re, double im) {
        c.k++;
        double ar = re, ai = im;
        for (int i = 0; i < 2; i++) { ar = biRun(c.bq[i], c.sI[i], ar); ai = biRun(c.bq[i], c.sQ[i], ai); }
        const double p = ar * ar + ai * ai;
        c.pSlow += (p - c.pSlow) * (1.0 / 2000);
        c.pFast += (p - c.pFast) * c.fastAlpha;
        double d = 0;
        if (c.havePrev) d = std::atan2(ai * c.prevRe - ar * c.prevIm, ar * c.prevRe + ai * c.prevIm);
        c.prevRe = ar; c.prevIm = ai; c.havePrev = true;
        if (c.nColl == 0) c.dc += (d - c.dc) * c.dcAlpha;
        if ((c.k & 255) == 0) {   // noise floor: the lowest slow power, creeping up
            if (c.k > 8000) c.noise = std::min(c.noise * 1.0003, c.pSlow);
            if (c.noise < 1e-12) c.noise = 1e-12;
        }
        const double dn = d - c.dc;
        const double norm = 1.0 / (2 * kPi * phySpec(c.def.phys[0]).devHz / c.def.fsOut);
        const bool s = dn > 0;
        for (auto& ch : c.chips) {
            if (s != ch.prevSlice) {
                const double err = ch.phase < 0.5 ? ch.phase : ch.phase - 1.0;
                ch.phase -= 0.12 * err;
                ch.prevSlice = s;
            }
            ch.phase += 1.0 / ch.sps;
            if (ch.phase >= 0.25 && ch.phase < 0.75) { ch.accSum += dn; ch.accN++; }
            if (ch.phase >= 1.0) {
                ch.phase -= 1.0;
                const double avg = ch.accN ? ch.accSum / ch.accN : dn;
                ch.accSum = 0; ch.accN = 0;
                onChip(c, ch, avg > 0 ? 1 : 0, avg * norm);
            }
        }
    }
};

WmbusLiveReceiver::WmbusLiveReceiver() : p_(std::make_unique<Impl>()) {}
WmbusLiveReceiver::~WmbusLiveReceiver() = default;

void WmbusLiveReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->resetReq = true;
}
void WmbusLiveReceiver::setSignalOffset(double) {}
void WmbusLiveReceiver::setCenterHz(double hz) { if (hz != p_->centreReq.exchange(hz)) p_->resetReq = true; }
bool WmbusLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= wmbusTuning().minSampleRate - 1;
}
void WmbusLiveReceiver::reset() { p_->resetReq = true; }
void WmbusLiveReceiver::clearResults() { p_->clearReq = true; }
void WmbusLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool WmbusLiveReceiver::telemetry(WmbusTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void WmbusLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.resetState();
        m.publish();
    }
    if (m.clearReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.devs.clear(); m.flog.clear(); m.okN = m.badN = m.encN = m.syncN = m.falseN = 0;
        for (auto& c : m.chans) c.frames = c.bad = 0;
        m.dirty = true;
    }
    if (n == 0 || m.rate < wmbusTuning().minSampleRate - 1 || m.chans.empty()) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.nIn++;
        for (auto& c : m.chans) {
            c.accRe += re * c.loRe - im * c.loIm;
            c.accIm += re * c.loIm + im * c.loRe;
            c.accN++;
            const double nr = c.loRe * c.stRe - c.loIm * c.stIm, ni = c.loRe * c.stIm + c.loIm * c.stRe;
            c.loRe = nr; c.loIm = ni;
            if (++c.loCount >= 4096) { c.loCount = 0; const double a = 1.0 / std::sqrt(c.loRe * c.loRe + c.loIm * c.loIm); c.loRe *= a; c.loIm *= a; }
            c.nIn++;
            if ((double)c.nIn >= c.binEnd) {
                const double inv = 1.0 / (double)c.accN;
                const double ar = c.accRe * inv, ai = c.accIm * inv;
                c.accRe = c.accIm = 0; c.accN = 0;
                c.binEnd = (double)(c.k + 1 + 1) * c.binW;
                m.sampleOut(c, ar, ai);
            }
        }
        if (m.tNow() >= m.nextReport) {
            m.nextReport += 0.25;
            std::lock_guard<std::mutex> lk(m.mu);
            float best = -200, snr = 0;
            for (auto& c : m.chans) {
                const float lv = m.toDb(c.pSlow);
                const float sn = m.toDb(c.pSlow / std::max(c.noise, 1e-12));
                if (lv > best) { best = lv; snr = sn; }
            }
            m.bestLevel = best; m.bestSnr = snr;
            if (m.tNow() >= m.nextSecond) {
                m.nextSecond += 1;
                m.levelHist.push_back(best);
                while (m.levelHist.size() > kWmbusHist) m.levelHist.pop_front();
            }
            m.publish();
            m.dirty = false;
        } else if (m.dirty) {
            std::lock_guard<std::mutex> lk(m.mu);
            m.publish();
            m.dirty = false;
        }
    }
}

ModeTuning wmbusTuning() {
    ModeTuning t;
    t.stdMode = 88; t.id = "wmbus"; t.name = "Wireless M-Bus (meters)";
    t.minMhz = 150; t.maxMhz = 880; t.defMhz = 868.625;
    t.sampleRate = 2e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 1e6;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
