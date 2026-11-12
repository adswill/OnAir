// Zigbee / IEEE 802.15.4 monitor (see zigbee_rx.h).
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/wirecap/wire.h"
#include "dect2/results/results_io.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/voicekeys/voice_keys.h"
#include "zigbee_bpsk.h"
#include "dect2/thread/thread_proto.h"
#include "dect2/map/mesh_topology_feed.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kOut = 8e6;              // demodulator rate: 4 samples per chip
constexpr int kSps = 4;
constexpr int kChipSamples = kSps;
constexpr int kSymSamples = 32 * kSps;    // one symbol
constexpr int kMean = 128;                // the running mean the chips are sliced against: 32 chips
constexpr int kMaxSyms = 12 + 2 + 2 + 2 * 127;   // up to 12 preamble symbols after the candidate, SFD, PHR, the longest PSDU
constexpr int kNeed = kMaxSyms * kSymSamples + 64;
constexpr int kHist = 4 * kSymSamples + 64;
constexpr int kErr = 7;                   // chip errors allowed in a preamble symbol (of 32)

int popc(uint32_t v) { return dect2::bits::popcount(v); }

// The sign of the frequency during chip n of a symbol (+1: the phase advances by pi/2): c'_n c'_{n-1} (-1)^(n+1) for the chips c' = +-1 (derived from
// the O-QPSK half-sine definition: even chips on I, odd on Q delayed by one chip)
struct Tables {
    float d[16][32];
    uint32_t r0 = 0;
    Tables() {
        for (int s = 0; s < 16; s++) {
            const uint32_t t = zigbee::kChipTable[s];
            for (int n = 0; n < 32; n++) {
                const int cn = (t >> (31 - n) & 1) ? 1 : -1;
                const int cp = (t >> (31 - (n + 31) % 32) & 1) ? 1 : -1;   // chip n - 1 (chip 31 of the same symbol before chip 0: the preamble's case)
                d[s][n] = (float)(cn * cp * (n % 2 == 0 ? -1 : 1));
            }
        }
        for (int n = 0; n < 32; n++) if (d[0][n] > 0) r0 |= 1u << (31 - n);
    }
};
const Tables& tables() { static const Tables t; return t; }
}

struct ZigbeeReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0, centerSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    // front end: mixer, low-pass FIR (computed only where an output sample needs it), linear interpolation to 8 Msps
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    int L = 1;
    std::vector<float> taps, hr, hi;      // hr / hi: the input history written twice, so a window is contiguous
    int hp = 0;
    double step = 1;
    double outRate = kOut, cutoff = 1.3e6;   // demodulator rate and front-end cut-off: the O-QPSK values, or the BPSK ones
    bool thread = false;                    // Thread mode: frames are read as Thread / 6LoWPAN
    int phy = 0;                            // 0 2.4 GHz O-QPSK, 1 868 MHz BPSK (20 kbit/s), 2 915 MHz BPSK (40 kbit/s)
    Bpsk154Demod bp;
    int64_t outK = 0;
    std::complex<float> yF{0, 0};
    // demodulator at 8 Msps
    std::complex<float> yring[kSps] = {};
    float meanBuf[kMean] = {}; int meanPos = 0; double meanSum = 0;
    uint32_t reg[kSps] = {};
    uint64_t hlo[kSps] = {}, hhi[kSps] = {};
    int64_t skipTrig[kSps] = {};
    std::vector<float> vb, pb;            // phase change over the last chip window, and power, from global index base
    int64_t base = 0, g = -1;
    struct Cand { int64_t at; };
    std::deque<Cand> cands;
    int64_t decodedUntil = 0;
    // results
    ZigbeeTelemetry tel, pub;
    MeshTopology topo;                 // links and routers of every PAN heard (mesh_topology.h)
    double topoSnapAt = -1e9;
    std::unordered_map<uint32_t, size_t> panIdx;     // pan << 8 | channel
    std::unordered_map<uint32_t, size_t> shortIdx;   // pan << 16 | short
    std::unordered_map<uint64_t, size_t> extIdx;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;
    double nextPps = 1; int ppsAcc = 0;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    int hintChannel() const {
        const double c = centerHz.load();
        return c > 0 ? zigbee::channelCodeOf((c + offset) / 1e6) : 0;
    }

    // which PHY the tuned frequency means: below 1 GHz the sub-GHz BPSK PHY (868.3 MHz or the 915 MHz channels), else O-QPSK; no centre known: O-QPSK
    void choosePhy() {
        const double c = centerHz.load();
        const double mhz = (c + offset) / 1e6;
        phy = 0;
        if (c > 0 && mhz < 1000.0) phy = (std::fabs(mhz - 868.3) < 5.0) ? 1 : 2;
        if (phy == 0) { outRate = kOut; cutoff = 1.3e6; }
        else {
            const double chip = phy == 1 ? 300e3 : 600e3;
            outRate = 4 * chip; cutoff = 1.1 * chip;
            bp.init(chip);
            bp.onFrame = [this](const Bpsk154Demod::Result& r) { bpskFrame(r); };
        }
    }
    void bpskFrame(const Bpsk154Demod::Result& r) {
        const std::vector<uint8_t>& psdu = r.psdu;
        const uint16_t calc = zigbee::crc16(psdu.data(), psdu.size() - 2);
        const uint16_t fcs = (uint16_t)(psdu[psdu.size() - 2] | psdu[psdu.size() - 1] << 8);
        if (calc != fcs) { tel.blocksBad++; return; }
        snrAvg = tel.blocksOk == 0 ? r.snrDb : 0.8 * snrAvg + 0.2 * r.snrDb;
        tel.cfoHz = r.cfoHz;
        accept(psdu, r.rssiDb, r.endSample / outRate);
    }

    void initFront() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        choosePhy();
        step = rate / outRate;
        // windowed sinc, cut-off 1.3 MHz: the 2 MHz channel's main lobe passes, the next channel (5 MHz away) and the noise beyond do not
        // (BPSK: 1.1 times the chip rate, the raised-cosine signal's width)
        L = std::max(15, (int)std::ceil(3.3 * rate / (cutoff * 1.23))) | 1;
        taps.assign((size_t)L, 0.f);
        const double fc = cutoff / rate;
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (L - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (float& t : taps) t = (float)(t / sum);
        std::reverse(taps.begin(), taps.end());   // dot with the history oldest first
        hr.assign((size_t)L * 2, 0.f); hi.assign((size_t)L * 2, 0.f); hp = 0;
        outK = 0; nIn = 0;
        yF = std::complex<float>(0, 0);
    }

    void resetDemod() {
        for (auto& y : yring) y = 0;
        for (float& b : meanBuf) b = 0;
        meanPos = 0; meanSum = 0;
        for (uint32_t& r : reg) r = 0;
        for (auto& h : hlo) h = 0;
        for (auto& h : hhi) h = 0;
        for (auto& s : skipTrig) s = 0;
        vb.clear(); pb.clear(); base = 0; g = -1; decodedUntil = 0;
        cands.clear();
    }

    // The tables and the frame log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool resClear = false, resLoad = false;
    ZigbeeTelemetry loaded;
    double loadedAt = 0;
    static void moveLists(ZigbeeTelemetry& to, ZigbeeTelemetry& from) {
        to.pans = std::move(from.pans); to.devices = std::move(from.devices); to.log = std::move(from.log); to.topo = std::move(from.topo);
        from.pans.clear(); from.devices.clear(); from.log.clear(); from.topo = TopoSnapshot();
    }
    static void shiftTimes(ZigbeeTelemetry& t, double dt) {
        for (auto& p : t.pans) { p.firstSec += dt; p.lastSec += dt; }
        for (auto& d : t.devices) { d.firstSec += dt; d.lastSec += dt; }
        for (auto& f : t.log) f.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { ZigbeeTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        initFront();
        resetDemod();
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        shiftTimes(tel, -tel.timeSec);    // the lists stay (clearResults): the clock starts again
        tel.timeSec = 0;
        nextPps = rate > 0 ? rate : 1; ppsAcc = 0;
        tel.ppsHist.clear(); tel.pps = 0;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = ZigbeeTelemetry(); tel.seq = seq; tel.inputRate = rate;
        panIdx.clear(); shortIdx.clear(); extIdx.clear();
        topo.clear(); topoSnapAt = -1e9;
        snrAvg = 0;
        nextPps = rate > 0 ? (double)nIn + rate : 1; ppsAcc = 0;
    }

    // ---------------------------------------------------------------- front end

    void input(float re, float im) {
        // the two complex products written out (the same bits as std::complex<double>, without its NaN-recovery call)
        const double lr = lo.real(), li = lo.imag(), sr = loStep.real(), si = loStep.imag();
        const std::complex<double> v((double)re * lr - (double)im * li, (double)re * li + (double)im * lr);
        lo = std::complex<double>(lr * sr - li * si, lr * si + li * sr);
        if (++loCount >= 4096) { lo /= std::abs(lo); loCount = 0; }
        hr[(size_t)hp] = hr[(size_t)(hp + L)] = (float)v.real();
        hi[(size_t)hp] = hi[(size_t)(hp + L)] = (float)v.imag();
        hp = hp + 1 == L ? 0 : hp + 1;
        const int64_t i = nIn;
        double t = outK * step;
        int64_t f = (int64_t)std::floor(t);
        if (i != f && i != f + 1) return;
        float o[2];
        genutil::dot2(taps.data(), hr.data() + hp, hi.data() + hp, L, o);
        const std::complex<float> y(o[0], o[1]);
        for (;;) {
            t = outK * step; f = (int64_t)std::floor(t);
            const float fr = (float)(t - (double)f);
            if (i == f) {
                yF = y;
                if (fr < 1e-6f) { outK++; demod(y); continue; }
                break;
            }
            if (i == f + 1) { outK++; demod(yF * (1 - fr) + y * fr); continue; }
            break;
        }
    }

    // ---------------------------------------------------------------- chip detector

    __attribute__((always_inline)) void demod(std::complex<float> y) {   // inlined: a call per sample stored the two floats and loaded them as one 8-byte value (store-forwarding stall)
        if (phy) { bp.push(y); return; }
        g++;
        const int ph = (int)(g & (kSps - 1));
        const std::complex<float> z = y * std::conj(yring[ph]);   // y four samples ago: one chip
        yring[ph] = y;
        const float v = std::atan2(z.imag(), z.real());
        // 4.0: the chip value is the imaginary part of the phase step over its running mean amplitude (peak pi/2 at the nominal step, 0 where the step is a
        // click, small where the signal faded): a click of the discriminator at low SNR no longer swamps a symbol correlation, a weak chip counts less
        // (docs/SENSITIVITY-4.0-A3.md)
        zAvg += (std::abs(z) - zAvg) * (1.f / 256);
        vb.push_back(std::clamp(1.5707963f * z.imag() / std::max(zAvg, 1e-9f), -2.5f, 2.5f));
        pb.push_back(std::norm(y));
        meanSum += v - meanBuf[meanPos]; meanBuf[meanPos] = v; meanPos = (meanPos + 1) % kMean;
        const uint32_t bit = v > (float)(meanSum / kMean) ? 1u : 0u;
        reg[ph] = (reg[ph] << 1) | bit;
        const bool m = popc(reg[ph] ^ tables().r0) <= kErr;
        hhi[ph] = (hhi[ph] << 1) | (hlo[ph] >> 63);
        hlo[ph] = (hlo[ph] << 1) | (m ? 1u : 0u);
        if (m && (hlo[ph] >> 32 & 1) && (hhi[ph] & 1) && g >= skipTrig[ph]) {   // three symbols of the preamble in a row
            cands.push_back({g});
            skipTrig[ph] = g + 2 * kSymSamples;
            tel.preambles++;
        }
    }

    float zAvg = 0;
    float at(int64_t k) const {
        const int64_t i = k - base;
        return i >= 0 && i < (int64_t)vb.size() ? vb[(size_t)i] : 0.f;
    }

    void process() {
        const int64_t end = base + (int64_t)vb.size();
        while (!cands.empty() && cands.front().at + kNeed < end) {
            const Cand c = cands.front();
            cands.pop_front();
            if (c.at < decodedUntil) continue;
            decode(c);
        }
        // drop what no candidate needs any more
        const int64_t keepFrom = (cands.empty() ? end : cands.front().at) - kHist;
        if (keepFrom - base > 65536) {
            const size_t drop = (size_t)(keepFrom - base);
            vb.erase(vb.begin(), vb.begin() + (ptrdiff_t)drop);
            pb.erase(pb.begin(), pb.begin() + (ptrdiff_t)drop);
            base += (int64_t)drop;
        }
    }

    // ---------------------------------------------------------------- frame decoder

    // the correlation of the 31 chips n = 1 .. 31 of a symbol ending (before it) at sample B with every sequence; the best symbol
    int symbol(int64_t B, double w0, double& best, double* resid) const {
        const Tables& T = tables();
        float vv[32];
        for (int n = 1; n < 32; n++) vv[n] = at(B + (int64_t)kChipSamples * (n + 1)) - (float)w0;
        int bs = 0; double bsc = -1e30;
        for (int s = 0; s < 16; s++) {
            double sc = 0;
            for (int n = 1; n < 32; n++) sc += T.d[s][n] * vv[n];
            if (sc > bsc) { bsc = sc; bs = s; }
        }
        best = bsc;
        if (resid) {
            double e = 0;
            for (int n = 1; n < 32; n++) { const double r = vv[n] - (kPi / 2) * T.d[bs][n]; e += r * r; }
            *resid = e / 31;
        }
        return bs;
    }

    void decode(const Cand& cd) {
        const Tables& T = tables();
        // the alignment: the sample shift (within one chip phase) and the carrier offset that make the preamble symbols fit best
        int64_t B0 = cd.at;
        double bestSc = -1e30, w0 = 0;
        for (int s = -1; s <= 1; s++) {
            const int64_t B = cd.at + s;
            if (B - 3 * kSymSamples + kChipSamples < base) continue;
            double sum = 0; int cnt = 0;
            for (int j = -3; j <= -1; j++)
                for (int n = 1; n < 32; n++) { sum += at(B + (int64_t)j * kSymSamples + kChipSamples * (n + 1)) - (kPi / 2) * T.d[0][n]; cnt++; }
            const double w = cnt ? sum / cnt : 0;
            double sc = 0;
            for (int j = -3; j <= -1; j++)
                for (int n = 1; n < 32; n++) sc += T.d[0][n] * (at(B + (int64_t)j * kSymSamples + kChipSamples * (n + 1)) - w);
            if (sc > bestSc) { bestSc = sc; B0 = B; w0 = w; }
        }
        if (bestSc < 0.35 * (kPi / 2) * 93) return;   // not a preamble: noise that happened to match
        int64_t B = B0;
        // the symbols after the preamble
        int zeros = 0, state = 0;   // state: 0 preamble, 1 SFD second symbol, 2 PHR first, 3 PHR second, 4 PSDU
        int len = 0, nibbles = 0, got = 0;
        std::vector<uint8_t> psdu;
        double residSum = 0; int residN = 0;
        int phrLo = 0;
        for (int m = 0; m < kMaxSyms; m++) {
            // the symbol, tried with the timing a sample early and late: the clock of the sender is not the receiver's
            double sc[3], rs[3];
            int sy[3];
            for (int k = 0; k < 3; k++) sy[k] = symbol(B + k - 1, w0, sc[k], &rs[k]);
            int pick = 1;
            if (sc[0] > sc[1] * 1.04 && sc[0] >= sc[2]) pick = 0;
            else if (sc[2] > sc[1] * 1.04) pick = 2;
            const int s = sy[pick];
            B += pick - 1;
            // follow the carrier offset: the mean chip phase after taking the decided symbol out
            {
                double e = 0;
                for (int n = 1; n < 32; n++) e += at(B + kChipSamples * (n + 1)) - (kPi / 2) * T.d[s][n];
                w0 = 0.9 * w0 + 0.1 * (e / 31);
            }
            residSum += rs[pick]; residN++;
            B += kSymSamples;
            if (state == 0) {
                if (s == 0) { if (++zeros > 12) return; continue; }
                if (s != 7) return;
                state = 1; continue;
            }
            if (state == 1) { if (s != 0xA) return; state = 2; continue; }
            if (state == 2) { phrLo = s; state = 3; continue; }
            if (state == 3) {
                len = phrLo | (s & 7) << 4;
                if (len < 3) return;
                psdu.assign((size_t)len, 0);
                state = 4; nibbles = 0; got = 0;
                continue;
            }
            const size_t bi = (size_t)(nibbles >> 1);
            if (nibbles & 1) psdu[bi] |= (uint8_t)(s << 4); else psdu[bi] = (uint8_t)s;
            nibbles++;
            if (nibbles >= 2 * len) { got = 1; break; }
        }
        if (!got) return;
        decodedUntil = B;
        const uint16_t calc = zigbee::crc16(psdu.data(), psdu.size() - 2);
        const uint16_t fcs = (uint16_t)(psdu[psdu.size() - 2] | psdu[psdu.size() - 1] << 8);
        if (calc != fcs) { tel.blocksBad++; return; }
        // the frame is good
        double pw = 0; int np = 0;
        for (int64_t k = std::max(base, B0 - 3 * kSymSamples); k < B0; k++) { pw += pb[(size_t)(k - base)]; np++; }
        const float rssi = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
        const double resid = residN ? residSum / residN : 1;
        const double snr = resid > 0 ? 10 * std::log10((kPi / 2) * (kPi / 2) / resid) : 40;
        snrAvg = tel.blocksOk == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
        tel.cfoHz = w0 * 2e6 / (2 * kPi);
        accept(psdu, rssi, (double)(B0 - 32 * kSymSamples) / outRate);
    }

    // ---------------------------------------------------------------- tables

    ZbPan& panEntry(uint16_t pan, int ch) {
        const uint32_t key = (uint32_t)pan << 8 | (uint32_t)ch;
        auto it = panIdx.find(key);
        if (it != panIdx.end()) return tel.pans[it->second];
        if (tel.pans.size() >= kZbPanMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.pans.size(); i++) if (tel.pans[i].lastSec < tel.pans[old].lastSec) old = i;
            tel.pans.erase(tel.pans.begin() + (ptrdiff_t)old);
            reindex();
        }
        ZbPan p;
        p.pan = pan; p.channel = ch;
        p.firstSec = sigSec();
        tel.pans.push_back(std::move(p));
        panIdx[key] = tel.pans.size() - 1;
        if (log && tel.pans.size() <= 20) { char b[80]; snprintf(b, sizeof b, "Zigbee: new PAN 0x%04X on channel %d", pan, ch); log(b); }
        return tel.pans.back();
    }

    void reindex() {
        panIdx.clear(); shortIdx.clear(); extIdx.clear();
        for (size_t i = 0; i < tel.pans.size(); i++) panIdx[(uint32_t)tel.pans[i].pan << 8 | (uint32_t)tel.pans[i].channel] = i;
        for (size_t i = 0; i < tel.devices.size(); i++) {
            const ZbDevice& d = tel.devices[i];
            if (d.shortNum >= 0) shortIdx[(uint32_t)d.pan << 16 | (uint32_t)d.shortNum] = i;
            if (d.extNum) extIdx[d.extNum] = i;
        }
    }

    ZbDevice& newDevice(uint16_t pan, int ch) {
        if (tel.devices.size() >= kZbDevMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.devices.size(); i++) if (tel.devices[i].lastSec < tel.devices[old].lastSec) old = i;
            tel.devices.erase(tel.devices.begin() + (ptrdiff_t)old);
            reindex();
        }
        ZbDevice d;
        d.pan = pan; d.channel = ch;
        d.firstSec = sigSec();
        tel.devices.push_back(std::move(d));
        return tel.devices.back();
    }

    // the device with this short address in the PAN and / or this extended address; one entry for both when they are known together
    ZbDevice& device(uint16_t pan, int shortAddr, uint64_t ext, int ch) {
        size_t si = (size_t)-1, ei = (size_t)-1;
        if (shortAddr >= 0) { auto it = shortIdx.find((uint32_t)pan << 16 | (uint32_t)shortAddr); if (it != shortIdx.end()) si = it->second; }
        if (ext) { auto it = extIdx.find(ext); if (it != extIdx.end()) ei = it->second; }
        size_t i;
        if (si != (size_t)-1) i = si;
        else if (ei != (size_t)-1) i = ei;
        else { newDevice(pan, ch); i = tel.devices.size() - 1; }
        ZbDevice& d = tel.devices[i];
        if (shortAddr >= 0 && d.shortNum < 0) { d.shortNum = shortAddr; d.shortAddr = zigbee::shortText((uint16_t)shortAddr); shortIdx[(uint32_t)pan << 16 | (uint32_t)shortAddr] = i; }
        if (ext && !d.extNum) {
            d.extNum = ext; d.extAddr = zigbee::addrText(ext); d.vendor = zigbee::vendorOfExt(ext); extIdx[ext] = i;
        }
        d.pan = pan;
        if (ch) d.channel = ch;
        return d;
    }

    void seen(ZbDevice& d, float rssi, int64_t wall, const std::string& info) {
        d.packets++; d.rssi = rssi; d.lastSec = sigSec(); d.lastWall = wall; d.last = info;
    }

    void accept(const std::vector<uint8_t>& psdu, float rssi, double sec) {
        using namespace zigbee;
        Frame f = parse(psdu.data(), psdu.size());
        if (!f.ok && f.info.empty()) { tel.blocksBad++; return; }
        { const wire::Chan wc = thread ? wire::Thread : wire::Zigbee; if (wire::on(wc)) wire::Hub::get().publish(wc, psdu); }   // the PSDU with its FCS, as received (never decrypted)
        dect2::thread::Frame th;
        if (thread) {   // Thread: no Zigbee NWK / APS reading, the 6LoWPAN layer instead
            th = dect2::thread::analyse(f, psdu.data(), psdu.size());
            f.hasNwk = f.hasAps = f.zbBeacon = false;
            if (!th.info.empty()) f.info = th.info;
        }
        if (f.nwkSecured && f.encrypted) {
            voicekeys::CallKeyInfo ki; ki.protocol = voicekeys::Protocol::Zigbee;
            for (const auto& k : voicekeys::KeyStore::instance().candidates(ki))
                if (k.key.size() == 16) { std::vector<uint8_t> plain; if (decryptNwk(psdu.data(), psdu.size(), f, k.key.data(), plain)) break; }
        }
        const int ch = hintChannel();
        tel.blocksOk++;
        if (f.type >= 0 && f.type < 8) tel.perType[f.type]++;
        if (ch >= kFirstChannel && ch <= kLastChannel) tel.perChannel[ch - kFirstChannel]++;   // (sub-GHz channels are not in this histogram)
        if (f.encrypted) tel.encryptedFrames++;
        lastGood = sigSec();
        ppsAcc++;
        const int64_t wall = (int64_t)time(nullptr);
        // which PAN
        uint16_t pan = 0; bool hasPan = false;
        if (f.type == Beacon && f.hasSrcPan) { pan = f.srcPan; hasPan = true; }
        else if (f.hasDestPan && f.destPan != 0xFFFF) { pan = f.destPan; hasPan = true; }
        else if (f.hasSrcPan && f.srcPan != 0xFFFF) { pan = f.srcPan; hasPan = true; }
        topoFeedIeee(topo, f, thread ? &th : nullptr, hasPan ? pan : (uint16_t)0, (float)snrAvg, sec);
        // the log
        ZbFrame lf;
        lf.wallTime = wall; lf.sec = sec; lf.channel = ch; lf.type = frameTypeName(f.type); lf.src = srcText(f); lf.dst = dstText(f);
        lf.pan = hasPan ? pan : 0; lf.len = (int)psdu.size(); lf.fcsOk = true; lf.encrypted = f.encrypted; lf.rssi = rssi; lf.info = f.info;
        lf.seq = f.seq; lf.ackReq = f.ackReq;
        tel.log.insert(tel.log.begin(), std::move(lf));
        if (tel.log.size() > kZbLogMax) tel.log.pop_back();
        if (hasPan) {
            ZbPan& p = panEntry(pan, ch);
            p.packets++; p.rssi = rssi; p.lastSec = sigSec(); p.lastWall = wall;
            if (thread && th.beacon) {
                p.zigbee = true; p.networkName = th.networkName; p.extPan = addrText(th.extPan); p.threadVersion = th.version; p.joinOpen = f.assocPermit;
                if (f.panCoordinator || p.coordinator.empty()) p.coordinator = srcText(f);
            } else if (f.zbBeacon) {
                p.zigbee = true; p.extPan = addrText(f.extPanId); p.stackProfile = f.stackProfile; p.joinOpen = f.assocPermit;
                if (f.panCoordinator || p.coordinator.empty()) p.coordinator = srcText(f);
            } else if (f.type == Beacon) {
                p.joinOpen = f.assocPermit;
                if (f.panCoordinator || p.coordinator.empty()) p.coordinator = srcText(f);
            }
            if (thread && th.lowpan) p.zigbee = true;   // (the flag means "the upper layer was seen": Thread here)
            if (f.hasNwk) { p.zigbee = true; if (f.nwkSrc == 0 && p.coordinator.empty()) p.coordinator = "0x0000"; }
        }
        // the devices: the sender (MAC), and the NWK originator when the frame was relayed
        const uint16_t dpan = hasPan ? pan : (f.hasSrcPan ? f.srcPan : 0);
        if (f.srcMode == 2 || f.srcMode == 3) {
            ZbDevice& d = device(dpan, f.srcMode == 2 ? (int)f.srcShort : -1, f.srcMode == 3 ? f.srcExt : 0, ch);
            if (f.type == Beacon) d.role = (f.panCoordinator || f.srcShort == 0) ? "coordinator" : "router";
            else if (d.role.empty()) d.role = f.srcMode == 2 && f.srcShort == 0 ? "coordinator" : "device";
            if (f.hasNwk && f.nwkType == 1 && d.role == "device") d.role = "router";
            seen(d, rssi, wall, f.info);
        }
        if (f.hasNwk && !(f.srcMode == 2 && f.srcShort == f.nwkSrc)) {
            ZbDevice& d = device(dpan, f.nwkSrc, f.nwkHasSrcExt ? f.nwkSrcExt : 0, ch);
            if (d.role.empty()) d.role = f.nwkSrc == 0 ? "coordinator" : "device";
            d.packets++; d.lastSec = sigSec(); d.lastWall = wall; d.last = f.info;
        } else if (f.hasNwk && f.nwkHasSrcExt) {
            device(dpan, f.nwkSrc, f.nwkSrcExt, ch);
        }
        // an association response gives a short address to an extended one
        if (f.type == Command && f.command == 2 && f.destMode == 3 && f.payloadLen >= 4) {
            const uint8_t* p = psdu.data() + f.payloadAt;
            const int sa = p[1] | p[2] << 8;
            if (p[3] == 0 && sa != 0xFFFF) device(dpan, sa, f.destExt, ch);
        }
        // a ZDO device announcement (clear) carries the short and the extended address
        if (f.hasAps && f.apsType == 0 && f.apsProfile == 0 && f.apsCluster == 0x0013) {
            const uint8_t* p = psdu.data() + f.payloadAt;
            // the ZDO payload starts after the NWK header and the APS header: found from the end (sequence, short address, IEEE address, capability)
            const size_t n = f.payloadLen;
            if (n >= 12) {
                const uint8_t* z = p + n - 12;
                uint64_t ext = 0;
                for (int i = 7; i >= 0; i--) ext = ext << 8 | z[3 + i];
                device(dpan, z[1] | z[2] << 8, ext, ch);
            }
        }
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.channel = hintChannel();
        tel.phy = phy;
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        while ((double)nIn >= nextPps && rate > 0) {   // one value a second
            tel.pps = (float)ppsAcc;
            tel.ppsHist.push_back((float)ppsAcc);
            if (tel.ppsHist.size() > kZbPpsHist) tel.ppsHist.erase(tel.ppsHist.begin());
            ppsAcc = 0; nextPps += rate;
        }
        if (tel.timeSec - topoSnapAt >= 1.0 || topoSnapAt > tel.timeSec) { tel.topo = topo.snapshot(); topoSnapAt = tel.timeSec; }
        ZigbeeTelemetry out = tel;
        std::stable_sort(out.pans.begin(), out.pans.end(), [](const ZbPan& x, const ZbPan& y) { return x.lastSec > y.lastSec; });
        std::stable_sort(out.devices.begin(), out.devices.end(), [](const ZbDevice& x, const ZbDevice& y) { return x.lastSec > y.lastSec; });
        for (ZbPan& p : out.pans) {
            int n = 0;
            for (const ZbDevice& d : out.devices) if (d.pan == p.pan && (d.channel == p.channel || !d.channel || !p.channel)) n++;
            p.devices = n;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

ZigbeeReceiver::ZigbeeReceiver() : p_(std::make_unique<Impl>()) {}
ZigbeeReceiver::~ZigbeeReceiver() = default;

void ZigbeeReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void ZigbeeReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void ZigbeeReceiver::setThreadMode(bool on) { p_->thread = on; }
void ZigbeeReceiver::setCenterHz(double hz) {
    // a move between the 2.4 GHz band and the sub-GHz bands changes the PHY and the front end: start the demodulator again
    const bool sub = hz > 0 && hz < 1e9, was = p_->centerHz.load() > 0 && p_->centerHz.load() < 1e9;
    const bool band868 = hz > 0 && std::fabs(hz / 1e6 - 868.3) < 5.0, was868 = p_->centerHz.load() > 0 && std::fabs(p_->centerHz.load() / 1e6 - 868.3) < 5.0;
    p_->centerHz = hz;
    if (sub != was || band868 != was868) p_->resetReq = true;
}
bool ZigbeeReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= zigbeeTuning().minSampleRate - 1; }
void ZigbeeReceiver::reset() { p_->resetReq = true; }
void ZigbeeReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);   // the report at once; the feed thread's lists (and counters) on its next block
        ZigbeeTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void ZigbeeReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    // 2.4 GHz needs 4 Msps (2 MHz channel, 4 samples per chip); the sub-GHz BPSK channels need 2 Msps (an RTL-SDR does)
    if (m.rate < (m.phy == 1 ? 2000000.0 : m.phy == 2 ? 2400000.0 : 4000000.0) - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.input(re, im);
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
    m.process();
}

std::string ZigbeeReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.pans, p_->pub.devices, p_->pub.log);
}
bool ZigbeeReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    ZigbeeTelemetry l;
    if (!results::unpack(blob, t0, l.pans, l.devices, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    ZigbeeTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool ZigbeeReceiver::telemetry(ZigbeeTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void ZigbeeReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning threadTuning() {
    ModeTuning t = zigbeeTuning();
    t.stdMode = 246; t.id = "thread"; t.name = "Thread";
    t.minMhz = 2405; t.maxMhz = 2480; t.defMhz = 2405;   // channel 11
    return t;
}

ModeTuning zigbeeTuning() {
    ModeTuning t;
    t.stdMode = 67; t.id = "zigbee"; t.name = "Zigbee / 802.15.4";
    t.minMhz = 863; t.maxMhz = 2480; t.defMhz = 2405;   // channel 11; 868.3 and 902 - 928 MHz are the sub-GHz BPSK channels
    t.sampleRate = 4000000;                  // one 2 MHz channel at a time (channels 11 - 26 span 77 MHz)
    t.basebandHz = 0;
    t.bandwidthMhz = 2;
    t.minSampleRate = 4000000;               // the engine asks for 4 Msps in both bands (the receiver itself decodes the sub-GHz channels from 2 / 2.4 Msps)
    t.tuneOffsetHz = 1000000;                // the radio sits 1 MHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2
