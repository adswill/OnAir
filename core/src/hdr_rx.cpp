// HD Radio receiver (see hdr_rx.h): resampling to 744187.5 Hz, the FM and AM Layer 1 receivers, Layer 2 and the data services, and the
// telemetry for the interface.
#include "dect2/hdr_rx.h"
#include "dect2/exact_resampler.h"
#include "dect2/hdr_fec.h"
#include "dect2/hdr_l2.h"
#include "dect2/hdr_phy.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace dect2 {

namespace {
struct Sink : hdr::L1Sink {
    hdr::L2Decoder* l2 = nullptr;
    double* now = nullptr;
    double lastP1 = -1e9;
    int badP1 = 0;                         // P1 frames with a bad header (cleared by the receiver)
    void pids(const uint8_t* b) override { l2->pushPids(b); }
    bool transfer(const uint8_t* b, int len, int ch) override {
        const uint64_t before = l2->pciOk(ch);
        l2->pushTransfer(b, len, ch);
        const bool ok = l2->pciOk(ch) > before;
        if (ok && ch == 0) lastP1 = *now;
        if (!ok && ch == 0) badP1++;
        return ok;
    }
    void blockSync() override { l2->resetTransport(); }
};
}

struct HdrReceiver::Impl {
    mutable std::mutex mu;                 // guards the members up to resetReq
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    HdrTelemetry pub;                      // the published report
    std::map<std::pair<int, int>, std::vector<uint8_t>> pubLots;
    std::atomic<bool> resetReq{true};
    // receiver thread
    HdrTelemetry tel;
    double curRate = 0;
    ExactResampler rs;
    std::vector<cf32> bb;
    hdr::L2Decoder l2;
    Sink fmSink, amSink;
    hdr::FmRx fm{&fmSink};
    hdr::AmRx am{&amSink};
    int band = 0;                          // 0 both receivers search, 1 FM, 2 AM
    bool mirror = false;                   // the input is conjugated (I and Q swapped)
    // While no band has been found, a second AM receiver looks at the conjugated input (AM runs at 46.5 kHz: cheap): a mirrored AM station
    // never reaches block sync in the first one. FM reaches block sync either way (its reference subcarriers are symmetric), so a mirrored
    // FM station shows as P1 frames that all fail.
    hdr::L2Decoder l2m;
    Sink amSinkM;
    hdr::AmRx amM{&amSinkM};
    std::vector<cf32> bbm;
    double mirrorSince = 0;                // signal time of the last change of `mirror`
    static constexpr double kMirrorWait = 12;   // seconds without any P1 frame before the other side is tried
    std::vector<cf32> clean;
    double lostFor = 0;
    int64_t nIn = 0;
    double now = 0;                        // signal seconds
    double nextReport = 0;
    double power = 0;
    int64_t nPower = 0;
    double busy = 0;                       // seconds spent in feed() since the last report
    uint64_t lastPids = 0;
    double lastPidsAt = -1e9;
    uint64_t lotVersion = 0;

    Impl() {
        fmSink.l2 = &l2; fmSink.now = &now;
        amSink.l2 = &l2; amSink.now = &now;
        amSinkM.l2 = &l2m; amSinkM.now = &now;
    }

    void resetState() {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; cb = log; pubLots.clear(); }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = HdrTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        if (curRate > 0) rs.configure(curRate, hdr::kRateFm);
        rs.reset();
        l2.reset();
        l2.setLog(cb);
        fm.reset();
        am.reset();
        amM.reset();
        l2m.reset();
        fmSink.lastP1 = amSink.lastP1 = -1e9;
        fmSink.badP1 = amSink.badP1 = 0;
        band = 0; lostFor = 0;
        mirror = false; mirrorSince = 0;
        nIn = 0; now = 0; nextReport = 0; power = 0; nPower = 0; busy = 0;
        lastPids = 0; lastPidsAt = -1e9; lotVersion = 0;
    }

    void report() {
        tel.seq++;
        tel.timeSec = now;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        tel.loadPct = (float)(busy / 0.25 * 100);
        power = 0; nPower = 0; busy = 0;
        const hdr::L1Stats f = fm.stats(), a = am.stats();
        const hdr::L1Stats& s = band == 2 ? a : band == 1 ? f : (a.state > f.state ? a : f);
        const bool isAm = band == 2 || (band == 0 && a.state > f.state);
        const double lastP1 = isAm ? amSink.lastP1 : fmSink.lastP1;
        tel.band = band;
        tel.state = s.state;
        if (s.state == 2 && now - lastP1 < 3.5) tel.state = 3;
        tel.cfoHz = s.cfoHz;
        tel.merLower = s.merLower; tel.merUpper = s.merUpper;
        tel.snrDb = s.state == 2 ? 0.5f * (s.merLower + s.merUpper) : 0.f;
        tel.ber = s.ber;
        tel.blockCount = s.bc;
        tel.constel = s.state == 2 ? s.constel : std::vector<cf32>();
        tel.syncCount = f.syncs + a.syncs;
        tel.serviceMode = s.mode;
        static const char* const fmModes[] = {"none", "MP1", "MP2", "MP3", "MP4", "MP5", "MP6", "MP7", "MP8", "MP9", "MP10", "MP11"};
        if (s.mode < 0) tel.modeName.clear();
        else if (isAm) tel.modeName = s.mode == 1 ? "MA1" : s.mode == 2 ? "MA3" : "MA?";
        else tel.modeName = s.mode < 12 ? fmModes[s.mode] : "MP" + std::to_string(s.mode);
        l2.setTime(now);
        l2.fill(tel);
        tel.blocksOk = l2.pciOk(0);
        tel.blocksBad = l2.pciBad(0);
        tel.p3Ok = l2.pciOk(1) + l2.pciOk(2);
        tel.p3Bad = l2.pciBad(1) + l2.pciBad(2);
        tel.p1Ok = now - lastP1 < 3.5;
        if (tel.pidsOk != lastPids) { lastPids = tel.pidsOk; lastPidsAt = now; }
        tel.pidsRecent = now - lastPidsAt < 1.0;
        std::map<std::pair<int, int>, std::vector<uint8_t>> lots;
        const bool newLots = tel.lotVersion != lotVersion;
        if (newLots) {
            lotVersion = tel.lotVersion;
            for (const HdrLotInfo& li : tel.lots) {
                std::vector<uint8_t> b;
                if (li.complete && l2.lotBytes(li.port, li.lot, b)) lots[std::make_pair(li.port, li.lot)] = std::move(b);
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
        if (newLots) pubLots = std::move(lots);
    }

    void feed(const cf32* x, size_t n) {
        const auto t0 = std::chrono::steady_clock::now();
        // NaN or infinite samples (a broken file or driver) would stay in the resampler and the filters: they become zeros. A mirrored
        // spectrum (I and Q swapped) is conjugated back.
        clean.assign(x, x + n);
        for (auto& v : clean) {
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            else if (mirror) v = std::conj(v);
        }
        x = clean.data();
        for (size_t i = 0; i < n; i++) power += (double)std::norm(x[i]);
        nPower += (int64_t)n;
        bb.clear();
        rs.process(x, n, bb);
        // FM or AM: until one of them has block sync, both search
        if (band != 2) fm.feed(bb.data(), bb.size());
        if (band != 1) am.feed(bb.data(), bb.size());
        const double dt = (double)n / curRate;
        if (band == 0) {
            if (fm.state() == 2) { band = 1; am.reset(); fmSink.badP1 = 0; say("HD Radio: FM hybrid signal found"); }
            else if (am.state() == 2) { band = 2; fm.reset(); say("HD Radio: AM hybrid signal found"); }
        } else {
            const int st = band == 1 ? fm.state() : am.state();
            lostFor = st == 2 ? 0 : lostFor + dt;
            if (lostFor > 8) { band = 0; lostFor = 0; fm.reset(); am.reset(); }
        }
        // the sample clock error the AM receiver measures is taken out in the resampler (FM copes with its pilots)
        double ppm;
        if (band == 2 && am.clockPpm(ppm) && std::fabs(ppm) > 0.5) rs.scaleStep(1 + ppm * 1e-6);
        // the mirrored spectrum: AM block sync only on the conjugated input, three failed FM P1 frames and not one good one, or (the last
        // resort) no P1 frame at all for a long time
        const bool noneGood = std::max(fmSink.lastP1, amSink.lastP1) < mirrorSince;
        if (band == 0 && noneGood) {
            bbm.resize(bb.size());
            for (size_t i = 0; i < bb.size(); i++) bbm[i] = std::conj(bb[i]);
            amM.feed(bbm.data(), bbm.size());
        }
        if (noneGood && ((band == 0 && amM.state() == 2 && am.state() != 2) || (band == 1 && fmSink.badP1 >= 3) || now - mirrorSince > kMirrorWait)) {
            mirror = !mirror;
            mirrorSince = now;
            band = 0; lostFor = 0; fm.reset(); am.reset(); amM.reset();
            fmSink.badP1 = amSink.badP1 = 0;
            say(mirror ? "HD Radio: the spectrum is mirrored (I and Q swapped), turning it round" : "HD Radio: trying the spectrum the right way round");
        }
        nIn += (int64_t)n;
        now = (double)nIn / curRate;
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        while (now >= nextReport) {      // about four reports a second of signal
            nextReport += 0.25;
            report();
        }
    }
    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }
};

HdrReceiver::HdrReceiver() : p_(std::make_unique<Impl>()) {}
HdrReceiver::~HdrReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void HdrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
bool HdrReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= hdrTuning().minSampleRate - 1;
}
void HdrReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = HdrTelemetry();
    p_->pub.seq = s;
    p_->pubLots.clear();
}

void HdrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.curRate <= 0 || !n) return;
    // in pieces of about 10 ms, so that the receivers' work and the reports stay spread out
    const size_t step = std::max<size_t>(1024, (size_t)(m.curRate * 0.01));
    for (size_t i = 0; i < n; i += step) m.feed(x + i, std::min(step, n - i));
}

bool HdrReceiver::telemetry(HdrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void HdrReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);        // the decoder takes it over at the next reset (configure() asks for one)
}
bool HdrReceiver::lotBytes(int port, int lot, std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->pubLots.find(std::make_pair(port, lot));
    if (it == p_->pubLots.end()) return false;
    out = it->second;
    return true;
}

ModeTuning hdrTuning() {
    ModeTuning t;
    t.stdMode = 23; t.id = "hdr"; t.name = "HD Radio";
    t.minMhz = 0.52; t.maxMhz = 108; t.defMhz = 98.5;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 1500000;
    t.tuneOffsetHz = 0;                  // no offset: the digital sidebands sit 129 to 198 kHz either side of an FM station (AM hybrid: within 15 kHz), the DC spike between them
    return t;
}

} // namespace dect2
