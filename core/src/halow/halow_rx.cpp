// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) network monitor (see halow_rx.h): the mixer, the receive chains (halow_chain.h), the network table and the frame log.
#include "dect2/halow/halow_rx.h"
#include "dect2/wirecap/wire.h"
#include "dect2/halow/halow_chain.h"
#include "dect2/halow/halow_proto.h"
#include "dect2/halow/halow_tel.h"
#include "dect2/results/results_io.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace halow;

struct HalowReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};
    std::atomic<int> widthMask{31};

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    bool mixOff = false;                 // the signal is not at the centre: mix it there (no mixing at offset 0)
    std::vector<std::unique_ptr<HalowChain>> chains;
    // Sub-channel scan: 1 MHz channels sit anywhere in a wider capture (the primary channel of a Morse Micro or other 2 / 4 / 8 MHz BSS, a duplicated beacon),
    // so extra 1 MHz chains look at the 1 MHz grid around the centre (+-0.5, +-1.5 MHz from 4 Msps, +-2.5 and +-3.5 from 8 Msps), each behind its own mixer
    struct Sub { double hz = 0; std::complex<double> lo{1, 0}, step{1, 0}; int cnt = 0; std::vector<cf32> buf; std::unique_ptr<HalowChain> chain; };
    std::vector<Sub> subs;
    double curSubHz = 0;                 // the sub-channel offset of the chain whose event is being handled
    std::atomic<bool> subScan{true};
    std::vector<cf32> mixed;
    // counters kept across chain restarts (a retune or a new rate rebuilds the chains)
    uint64_t accPre = 0, accSigOk = 0, accSigBad = 0, accDataBad = 0;
    uint64_t accWidth[kNumWidths] = {};
    // results
    HalowTelemetry tel, pub;
    std::unordered_map<std::string, size_t> netIdx;
    std::unordered_map<uint32_t, std::string> ssidByComp;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;
    double nextPps = 1; int ppsAcc = 0;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }
    double hintMhz() const { const double c = centerHz.load(); return c > 0 ? (c + offset + curSubHz) / 1e6 : 0; }

    // The chains for this input rate: 1 / 2 / 4 MHz on the 4 Msps grid, 8 MHz from 8 Msps, 16 MHz from 16 Msps (halow_phy.h: minRateHz)
    void buildChains() {
        for (auto& c : chains) {
            const HalowChain::Stats& s = c->stats();
            accPre += s.preambles; accSigOk += s.sigOk; accSigBad += s.sigBad; accDataBad += s.dataBad;
            for (int w = 0; w < kNumWidths; w++) accWidth[w] += s.perWidth[w];
        }
        for (auto& sb : subs) {   // the scan chains see parts of wider signals: only what they read correctly is counted
            const HalowChain::Stats& s = sb.chain->stats();
            accSigOk += s.sigOk;
            for (int w = 0; w < kNumWidths; w++) accWidth[w] += s.perWidth[w];
        }
        chains.clear();
        subs.clear();
        if (rate < halowTuning().minSampleRate - 1) return;
        auto add = [&](double chainRate, std::vector<int> widths) {
            auto c = std::make_unique<HalowChain>(rate, chainRate, widths);
            c->setWidthMask(widthMask.load());
            c->onEvent = [this](const HalowEvent& e) { onEvent(e); };
            chains.push_back(std::move(c));
        };
        add(4e6, {W2, W1, W4});
        if (rate >= minRateHz(W8) - 1) add(8e6, {W8});
        if (rate >= minRateHz(W16) - 1) add(16e6, {W16});
        // sub-channel scan (0.42 of the rate: the outer 1 / 2 MHz channels of an 8 MHz BSS at +-3.5 / +-3 MHz are inside a 10 Msps capture, #192): only with the dial on the centre, all widths tried (or 1 MHz chosen) and a radio rate that holds a wider band
        if (subScan.load() && offset == 0 && (widthMask.load() & 1) && rate >= 2.9e6) {
            std::vector<double> offs;   // the 1 MHz grid around the centre, as far as the passband of the radio's rate holds a whole channel
            for (double hz = 0.5e6; hz + 0.6e6 <= 0.42 * rate && offs.size() < 12; hz += 1e6) { offs.push_back(-hz); offs.push_back(hz); }
            for (double hz : offs) {
                Sub sb;
                sb.hz = hz;
                sb.step = std::polar(1.0, -2 * math::kPi * hz / rate);
                sb.chain = std::make_unique<HalowChain>(rate, 4e6, std::vector<int>{W1});
                sb.chain->setWidthMask(1);
                sb.chain->onEvent = [this, hz](const HalowEvent& e) { curSubHz = hz; onEvent(e); curSubHz = 0; };
                subs.push_back(std::move(sb));
            }
        }
        // the same for 2 MHz channels (a 2 MHz primary of a 4 / 8 / 16 MHz BSS, or a 2 MHz BSS off the centre): 2 MHz grid positions every 0.5 MHz, the whole 2 MHz channel inside the passband
        if (subScan.load() && offset == 0 && (widthMask.load() & 2) && rate >= 4e6 - 1) {
            int n = 0;
            for (double hz = 0.5e6; hz + 1.1e6 <= 0.42 * rate && n < 16; hz += 0.5e6, n += 2) for (double sgn : {-1.0, 1.0}) {
                Sub sb;
                sb.hz = sgn * hz;
                sb.step = std::polar(1.0, -2 * math::kPi * sb.hz / rate);
                sb.chain = std::make_unique<HalowChain>(rate, 4e6, std::vector<int>{W2});
                sb.chain->setWidthMask(2);
                const double h = sb.hz;
                sb.chain->onEvent = [this, h](const HalowEvent& e) { curSubHz = h; onEvent(e); curSubHz = 0; };
                subs.push_back(std::move(sb));
            }
        }
    }

    // The tables and the frame log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu)
    bool resClear = false, resLoad = false;
    HalowTelemetry loaded;
    double loadedAt = 0;
    // Moves the network table and frame log from one telemetry to another.
    static void moveLists(HalowTelemetry& to, HalowTelemetry& from) {
        to.nets = std::move(from.nets); to.log = std::move(from.log);
        from.nets.clear(); from.log.clear();
    }
    // Shifts the signal times of the lists (a loaded file, a restarted clock).
    static void shiftTimes(HalowTelemetry& t, double dt) {
        for (auto& n : t.nets) { n.firstSec += dt; n.lastSec += dt; }
        for (auto& f : t.log) f.sec += dt;
    }
    // Applies a Clear or a loaded file the interface thread asked for.
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { HalowTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    // Restarts the demodulator for a new rate or offset; the tables stay.
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        lo = 1; loCount = 0;
        mixOff = offset != 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * math::kPi * offset / rate) : std::complex<double>(1, 0);
        nIn = 0;
        buildChains();
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        shiftTimes(tel, -tel.timeSec);
        tel.timeSec = 0;
        nextPps = rate > 0 ? rate : 1; ppsAcc = 0;
        tel.ppsHist.clear(); tel.pps = 0;
    }
    // Empties the tables and counters (the chains are rebuilt so their counters restart too).
    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = HalowTelemetry(); tel.seq = seq; tel.inputRate = rate;
        buildChains();   // folds the chains' counters into the accumulators, which are then zeroed
        accPre = accSigOk = accSigBad = accDataBad = 0;
        for (auto& w : accWidth) w = 0;
        netIdx.clear(); ssidByComp.clear();
        snrAvg = 0;
        nextPps = rate > 0 ? (double)nIn + rate : 1; ppsAcc = 0;
    }
    // Rebuilds the lookup tables after the lists changed.
    void reindex() {
        netIdx.clear(); ssidByComp.clear();
        for (size_t i = 0; i < tel.nets.size(); i++) {
            netIdx[tel.nets[i].bssid] = i;
            if (tel.nets[i].hasSsid && !tel.nets[i].ssid.empty()) ssidByComp[wifi::crc32(reinterpret_cast<const uint8_t*>(tel.nets[i].ssid.data()), tel.nets[i].ssid.size())] = tel.nets[i].ssid;
        }
    }

    // ---------------------------------------------------------------- tables

    void trimLog() { while (tel.log.size() > kHlLogMax) tel.log.pop_back(); }
    void pushLog(HlFrame f) { tel.log.insert(tel.log.begin(), std::move(f)); trimLog(); }

    // The table entry of a BSSID, made (and the oldest dropped at the limit) when new.
    HlNet& netEntry(const std::string& key) {
        auto it = netIdx.find(key);
        if (it != netIdx.end()) return tel.nets[it->second];
        if (tel.nets.size() >= kHlNetMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.nets.size(); i++) if (tel.nets[i].lastSec < tel.nets[old].lastSec) old = i;
            tel.nets.erase(tel.nets.begin() + (ptrdiff_t)old);
            reindex();
        }
        HlNet n;
        n.bssid = key;
        n.firstSec = sigSec();
        tel.nets.push_back(std::move(n));
        netIdx[key] = tel.nets.size() - 1;
        if (log && tel.nets.size() <= 20) { char b[96]; snprintf(b, sizeof b, "HaLow: new network %s", key.c_str()); log(b); }
        return tel.nets.back();
    }

    // A frame log line from the SIG field of an event
    HlFrame frameOf(const HalowEvent& e, bool decoded, int len) {
        HlFrame f;
        f.wallTime = (int64_t)std::time(nullptr); f.sec = sigSec();
        f.widthMhz = widthMhz(e.sig.w); f.mcs = e.sig.mcs; f.nss = e.sig.nss(); f.pre = e.sig.pre; f.sgi = e.sig.sgi; f.snrDb = e.snrDb;
        f.rateMbps = (float)dataRateMbps(e.sig.w, e.sig.mcs, e.sig.nss(), e.sig.sgi);
        f.len = len; f.decoded = decoded; f.rssi = e.rssi;
        return f;
    }

    // The SIG field was read, the data field not decoded (several streams, STBC, LDPC, A-MPDU, NDP): counted and logged
    void undecoded(const HalowEvent& e) {
        tel.undecoded++;
        lastGood = sigSec();
        ppsAcc++;
        tel.cfoHz = e.cfoHz;
        snrAvg = snrAvg == 0 ? e.snrDb : 0.8 * snrAvg + 0.2 * e.snrDb;
        const std::string why = e.why;
        if (why == "NDP") tel.ndpFrames++;
        else if (why == "spatial streams") tel.mimoFrames++;
        else if (why == "STBC") tel.stbcFrames++;
        else if (why == "LDPC") tel.ldpcFrames++;
        else if (why == "A-MPDU") tel.aggFrames++;
        HlFrame f = frameOf(e, false, e.sig.len);
        f.type = e.sig.ndp ? "NDP" : "PPDU";
        char b[160];
        if (e.sig.ndp) snprintf(b, sizeof b, "%d MHz null data packet", widthMhz(e.sig.w));
        else snprintf(b, sizeof b, "%d MHz MCS %d, %d stream%s, %d %s: SIG field read, %s not decoded", widthMhz(e.sig.w), e.sig.mcs, e.sig.nss(), e.sig.nss() > 1 ? "s" : "", e.sig.len,
                      e.sig.agg ? "symbols" : "bytes", e.why);
        f.info = b;
        pushLog(std::move(f));
    }

    // One decoded PSDU: FCS, frame parse, network table
    void accept(const HalowEvent& e) {
        const std::vector<uint8_t>& psdu = e.psdu;
        if (!fcsOk(psdu.data(), psdu.size())) { tel.blocksBad++; return; }
        if (wire::on(wire::Halow) && psdu.size() > 4) wire::Hub::get().publish(wire::Halow, wire::radiotap80211(psdu.data(), psdu.size() - 4, 0, 0));   // as received, without the FCS
        tel.blocksOk++;
        ppsAcc++;
        lastGood = sigSec();
        tel.cfoHz = e.cfoHz;
        snrAvg = snrAvg == 0 ? e.snrDb : 0.8 * snrAvg + 0.2 * e.snrDb;
        const int64_t wall = (int64_t)std::time(nullptr);
        halow::Frame f;
        parseFrame(psdu.data(), psdu.size() - 4, f);
        HlFrame fr = frameOf(e, true, (int)psdu.size());
        const bool net = f.s1gBeacon || f.probeResp || f.mgmtBeacon;
        fr.type = f.s1gBeacon ? (f.shortBeacon ? "S1G short beacon" : "S1G beacon") : f.probeResp ? "probe response" : f.mgmtBeacon ? "beacon" : f.summary;
        if (net || f.hasAddrs) fr.bssid = macText(f.bssid);
        fr.info = f.summary;
        if (net) {
            tel.beaconsSeen++;
            HlNet& n = netEntry(macText(f.bssid));
            n.vendor = wifi::vendorOf(f.bssid);
            n.seenWidthMhz = widthMhz(e.sig.w); n.mcs = e.sig.mcs; n.nss = e.sig.nss(); n.pre = e.sig.pre; n.snrDb = e.snrDb;
            n.rateMbps = fr.rateMbps;
            n.rssi = e.rssi; n.rssiMax = std::max(n.rssiMax, e.rssi);
            n.packets++; if (f.s1gBeacon || f.mgmtBeacon) n.beacons++;
            n.lastSec = sigSec(); n.lastWall = wall; n.last = f.summary;
            if (f.hasSsid) {
                n.hasSsid = !f.ssid.empty(); n.hidden = f.ssid.empty(); n.ssid = f.ssid;
                if (!f.ssid.empty()) {
                    const uint32_t cr = wifi::crc32(reinterpret_cast<const uint8_t*>(f.ssid.data()), f.ssid.size());
                    ssidByComp[cr] = f.ssid;
                    for (HlNet& o : tel.nets) if (!o.hasSsid && o.comp == cr) { o.ssid = f.ssid; o.hasSsid = true; }
                }
            }
            if (f.hasComp) n.comp = f.compSsid;
            if (!n.hasSsid && n.comp) { auto it = ssidByComp.find(n.comp); if (it != ssidByComp.end()) { n.ssid = it->second; n.hasSsid = true; n.hidden = false; } }
            if (f.hasOp) {
                n.ccf = f.ccf; n.opClass = f.opClass; n.opWidthMhz = f.opWidthMhz; n.primaryWidthMhz = f.primaryWidthMhz; n.basicMcs = f.basicMcsOneSs;
                const double m = chanMhz(f.opClass, f.ccf);
                if (m > 0) n.freqMhz = m;
            }
            if (f.hasOp) { const double pm = chanMhz(f.opClass, f.primary); if (pm > 0) n.primaryMhz = pm; }
            if (!f.vendorIes.empty()) {
                std::string s;
                for (const std::string& o : f.vendorIes) {
                    uint8_t a[6] = {};
                    unsigned x[3] = {};
                    if (sscanf(o.c_str(), "%2x:%2x:%2x", &x[0], &x[1], &x[2]) == 3) for (int k = 0; k < 3; k++) a[k] = (uint8_t)x[k];
                    const char* v = wifi::vendorOf(a);
                    const bool wfa = x[0] == 0x50 && x[1] == 0x6F && x[2] == 0x9A;
                    if (!s.empty()) s += ", ";
                    s += o;
                    if (wfa) s += " Wi-Fi Alliance"; else if (v && *v && strcmp(v, "local")) { s += " "; s += v; }
                }
                n.vendorIe = s;
                if (n.vendor.empty() || n.vendor == "local") for (const std::string& o : f.vendorIes) {
                    uint8_t a[6] = {}; unsigned x[3] = {};
                    if (sscanf(o.c_str(), "%2x:%2x:%2x", &x[0], &x[1], &x[2]) == 3) { for (int k = 0; k < 3; k++) a[k] = (uint8_t)x[k]; const char* v = wifi::vendorOf(a); if (v && *v && strcmp(v, "local")) { n.vendor = v; break; } }
                }
            }
            if (f.hasCap) n.longPreamble = f.longPreamble ? 1 : 0;
            if (f.hasCountry) n.country = f.country;
            if (f.hasTim) n.dtimPeriod = f.dtimPeriod;
            if (f.hasRaw) n.hasRaw = true;
            if (f.hasTwt) n.hasTwt = true;
            if (f.txPowerDbm >= 0) n.txPowerDbm = f.txPowerDbm;
            if (n.freqMhz <= 0) n.freqMhz = hintMhz();
            n.security = securityName(f.security);
            if (f.intervalTu > 0) n.intervalTu = f.intervalTu;
        }
        pushLog(std::move(fr));
    }

    // A PPDU from a chain: counted by its SIG field, then decoded or only reported
    void onEvent(const HalowEvent& e) {
        tel.perMcs[std::min(10, std::max(0, e.sig.mcs))]++;
        tel.perNss[std::min(4, std::max(1, e.sig.nss()))]++;
        tel.perPre[std::min(2, std::max(0, e.sig.pre))]++;
        if (e.psduValid) accept(e); else undecoded(e);
    }

    // ---------------------------------------------------------------- feed side

    void process() { for (auto& c : chains) c->process(); for (auto& sb : subs) sb.chain->process(); }

    // Publishes the telemetry for the interface thread.
    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.centerMhz = hintMhz();
        tel.region = tel.centerMhz > 0 ? regionOf(tel.centerMhz) : -1;
        tel.chanHint = tel.region >= 0 ? nearestChannel(tel.region, tel.centerMhz, 1) : 0;
        tel.maxWidthMhz = maxWidthMhz(rate);
        tel.widthMask = widthMask.load();
        tel.preambles = accPre; tel.sigOk = accSigOk; tel.sigBad = accSigBad; tel.dataBad = accDataBad;
        for (int w = 0; w < kNumWidths; w++) tel.perWidth[w] = accWidth[w];
        for (auto& c : chains) {
            const HalowChain::Stats& s = c->stats();
            tel.preambles += s.preambles; tel.sigOk += s.sigOk; tel.sigBad += s.sigBad; tel.dataBad += s.dataBad;
            for (int w = 0; w < kNumWidths; w++) tel.perWidth[w] += s.perWidth[w];
        }
        for (auto& sb : subs) {
            const HalowChain::Stats& s = sb.chain->stats();
            tel.sigOk += s.sigOk;
            for (int w = 0; w < kNumWidths; w++) tel.perWidth[w] += s.perWidth[w];
        }
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        while ((double)nIn >= nextPps && rate > 0) {
            tel.pps = (float)ppsAcc;
            tel.ppsHist.push_back((float)ppsAcc);
            if (tel.ppsHist.size() > kHlPpsHist) tel.ppsHist.erase(tel.ppsHist.begin());
            ppsAcc = 0; nextPps += rate;
        }
        HalowTelemetry out = tel;
        std::stable_sort(out.nets.begin(), out.nets.end(), [](const HlNet& x, const HlNet& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

HalowReceiver::HalowReceiver() : p_(std::make_unique<Impl>()) {}
HalowReceiver::~HalowReceiver() = default;

void HalowReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void HalowReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void HalowReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
// Chooses the widths tried (bit 0 = 1 MHz .. bit 4 = 16 MHz); a change restarts the chains.
void HalowReceiver::setWidthMask(int mask) {
    mask &= 31;
    if (mask == 0) mask = 31;
    if (p_->widthMask.exchange(mask) != mask) p_->resetReq = true;
}
int HalowReceiver::widthMask() const { return p_->widthMask.load(); }
bool HalowReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= halowTuning().minSampleRate - 1; }
void HalowReceiver::reset() { p_->resetReq = true; }
// Asks for the tables and the frame log to be emptied (the feed thread does it).
void HalowReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        HalowTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

// Takes input samples (never blocks): mixes them to the centre, feeds every chain, publishes every 0.25 s of signal.
void HalowReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.rate < halowTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    size_t pos = 0;
    while (pos < n) {
        // up to the next report time, so the tables are published every 0.25 s of signal
        size_t take = n - pos;
        const double toRep = m.nextReport - (double)m.nIn;
        if (toRep < (double)take) take = (size_t)std::max(1.0, std::ceil(toRep));
        m.mixed.resize(take);
        // the signal at the centre of the capture (mixed there when the dial is off), its power, and nothing but finite numbers in it
        float pw = 0;
        for (size_t i = 0; i < take; i++) {
            const float re = x[pos + i].real(), im = x[pos + i].imag();
            pw += re * re + im * im;
            m.mixed[i] = x[pos + i];
        }
        if (!std::isfinite(pw)) {   // a NaN or infinity in the block: those samples become zero
            pw = 0;
            for (size_t i = 0; i < take; i++) {
                if (!std::isfinite(m.mixed[i].real()) || !std::isfinite(m.mixed[i].imag())) m.mixed[i] = cf32(0, 0);
                pw += std::norm(m.mixed[i]);
            }
        }
        m.power += pw;
        if (m.mixOff)
            for (size_t i = 0; i < take; i++) {
                const std::complex<double> v = std::complex<double>(m.mixed[i].real(), m.mixed[i].imag()) * m.lo;
                m.lo *= m.loStep;
                if (++m.loCount >= 4096) { m.lo /= std::abs(m.lo); m.loCount = 0; }
                m.mixed[i] = cf32((float)v.real(), (float)v.imag());
            }
        for (auto& c : m.chains) c->feed(m.mixed.data(), take);
        for (auto& sb : m.subs) {
            sb.buf.resize(take);
            for (size_t i = 0; i < take; i++) {
                const std::complex<double> v = std::complex<double>(m.mixed[i].real(), m.mixed[i].imag()) * sb.lo;
                sb.lo *= sb.step;
                if (++sb.cnt >= 4096) { sb.lo /= std::abs(sb.lo); sb.cnt = 0; }
                sb.buf[i] = cf32((float)v.real(), (float)v.imag());
            }
            sb.chain->feed(sb.buf.data(), take);
        }
        m.nIn += (int64_t)take;
        m.nPower += (int64_t)take;
        pos += take;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.process();
}

// Works on what was fed and publishes the telemetry now (tests).
void HalowReceiver::flush() {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    m.process();
    m.report();
}

// The network table and frame log as a results file blob.
std::string HalowReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.nets, p_->pub.log);
}
// Loads such a blob; false when it is not this build's format.
bool HalowReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    HalowTelemetry l;
    if (!results::unpack(blob, t0, l.nets, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    HalowTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
// Copies the latest published telemetry when it is newer than lastSeq.
bool HalowReceiver::telemetry(HalowTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void HalowReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

// The engine's tuning entry for Wi-Fi HaLow (std 72).
ModeTuning halowTuning() {
    ModeTuning t;
    t.stdMode = 72; t.id = "halow"; t.name = "Wi-Fi HaLow (802.11ah)";
    t.minMhz = 755; t.maxMhz = 928; t.defMhz = 863.5;   // the first EU 1 MHz channel; the band list reaches down to the Chinese 755 MHz plan
    t.sampleRate = 6000000;                  // Auto 1 - 4 MHz: 6 Msps holds a 4 MHz BSS with its 1 MHz primaries at +-1.5 MHz and 2 MHz primaries at +-1 MHz (#192); 1 and 2 MHz channels also work from 2.4 Msps (an RTL-SDR); 8 / 16 MHz need 10 / 20 Msps
    t.basebandHz = 0;
    t.bandwidthMhz = 4;
    t.minSampleRate = 2000000;
    t.tuneOffsetHz = 0;                      // OFDM leaves the centre tone empty: no DC spike to avoid
    return t;
}

} // namespace dect2
