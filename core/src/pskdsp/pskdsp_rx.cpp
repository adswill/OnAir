// PSK demodulator receiver (pskdsp_rx.h): search span, blind analysis (with a zoom for narrow signals), continuous demodulation with
// Doppler following, bit decoding, saving.
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

PskdSettings sanitize(PskdSettings s) {
    s.order = std::clamp(s.order, 0, 4);
    if (!(s.baud >= 0) || s.baud > 5e6) s.baud = 0;
    if (s.alpha != -2 && s.alpha != -1) s.alpha = std::clamp(s.alpha, 0.0, 1.0);
    if (!(s.spanHz >= 0)) s.spanHz = 0;
    s.spanHz = std::min(s.spanHz, 50e6);
    s.diff = std::clamp(s.diff, 0, 3);
    s.scrambler = std::clamp(s.scrambler, 0, 2);
    if (s.sync.size() > 256) s.sync.resize(256);
    return s;
}
double lockEvmFor(pskd::Order o) { return o == pskd::Order::Psk8 ? 0.28 : o == pskd::Order::Bpsk ? 0.55 : 0.42; }

// a window of the newest samples (amortised trimming)
struct Hist {
    std::vector<pskd::cf> v;
    size_t cap = 0;
    void push(const std::vector<pskd::cf>& x) {
        v.insert(v.end(), x.begin(), x.end());
        if (cap && v.size() > cap + cap / 4) v.erase(v.begin(), v.end() - (long)cap);
    }
    size_t size() const { return std::min(v.size(), cap ? cap : v.size()); }
    const pskd::cf* tail(size_t n) const { return v.data() + (v.size() - n); }
    void clear() { v.clear(); }
};
} // namespace

struct PskdReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    PskdSettings cfgSet;
    std::atomic<bool> dirty{true}, resetReq{true}, clearReq{false}, reanalyseReq{false};
    std::function<void(const std::string&)> log;
    PskdTelemetry pub;

    // processing (analysis thread)
    double rate = 0, offset = 0;
    PskdSettings cfg;
    pskd::Channeliser front;
    double frontRate = 0, span = 0;
    Hist histF;
    // zoom on a narrow signal
    bool zoomOn = false; int zoomMisses = 0;
    pskd::Channeliser zoom;
    double zoomRate = 0, zoomOffset = 0, zoomPass = 0;   // zoom offset: relative to the front
    Hist histZ;
    // lock
    bool locked = false;
    bool viaZoom = false;        // locked on a narrow signal: the demodulation channel is cut from the zoom, which follows it
    pskd::Channeliser chan;
    pskd::Demod demod;
    pskd::BitDecoder bitdec;
    pskd::Order order = pskd::Order::Bpsk;
    double baud = 0;
    int badChecks = 0;
    double evmSmooth = 0;
    pskd::Bits syncBits;
    // timing (input samples)
    int64_t nIn = 0;
    double nextAnalyse = 0, nextReport = 0, nextTrack = 0;
    double power = 0; int64_t nPower = 0;
    // results
    PskdTelemetry tel;
    std::vector<pskd::cf> scratch, scratch2, scratch3, syms;
    std::vector<float> eyeNew;
    pskd::Bits newBits;
    std::vector<uint8_t> tailForSync;
    std::ofstream save; std::string savePathOpen; uint8_t saveAcc = 0; int saveN = 0;

    void logf(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb("PSK: " + s);
    }
    void addLog(const std::string& s) {
        if (!tel.log.empty() && tel.log.back() == s) return;
        tel.log.push_back(s);
        if (tel.log.size() > kPskdLogMax) tel.log.erase(tel.log.begin(), tel.log.begin() + (long)(tel.log.size() - kPskdLogMax));
    }

    void rebuild() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; cfg = sanitize(cfgSet); dirty = false; }
        tel.cfg = cfg; tel.inputRate = rate;
        unlock(false);
        zoomOn = false; histZ.clear();
        if (rate <= 0) return;
        span = cfg.spanHz > 0 ? std::min(cfg.spanHz, rate * 0.9) : std::min(200e3, rate * 0.8);
        front.configure(rate, offset, span, 0);
        frontRate = front.outRate();
        histF.clear();
        histF.cap = (size_t)std::min(2.0 * frontRate, 2.5e6);
        tel.spanHz = span;
        syncBits = pskd::parseBitsOrHex(cfg.sync);
        nextAnalyse = (double)nIn + 0.4 * rate;
        char b[160];
        std::snprintf(b, sizeof b, "searching %.1f kHz around the dial (channel at %.0f samples/s)", span / 1e3, frontRate);
        addLog(b);
        tel.status = "searching for a signal";
    }

    void unlock(bool say) {
        if (locked && say) { addLog("lock lost: searching again"); logf("lock lost"); }
        if (locked && viaZoom) { zoomOn = false; histZ.clear(); }
        locked = false; viaZoom = false; badChecks = 0;
        tel.state = 0;
    }

    pskd::AnalyseOptions options(double maxSec, double nearMax, double band) const {
        pskd::AnalyseOptions o;
        o.bandHz = band;
        o.forceOrder = (pskd::Order)cfg.order;
        o.forceBaud = cfg.baud;
        o.forceAlpha = cfg.alpha;
        o.snapStandard = cfg.snapStandard;
        o.nearCentre = true; o.nearMaxHz = nearMax;
        o.maxSeconds = maxSec;
        return o;
    }

    void copyAnalysis(const pskd::Analysis& a) {
        tel.analyses++;
        tel.signal = a.signal; tel.rateFound = a.rateFound;
        tel.status = a.status;
        tel.bw3Hz = a.carrier.bw3Hz; tel.occHz = a.carrier.occHz; tel.snrDb = (float)a.carrier.snrDb;
        tel.line2 = a.line2; tel.line4 = a.line4; tel.line8 = a.line8; tel.lineEnv = a.lineEnv;
        tel.rates.clear();
        for (const auto& r : a.rates) { if (tel.rates.size() >= 5) break; tel.rates.push_back({r.baud, r.conf, r.how}); }
        tel.envSpecDb = a.envSpecDb; tel.envHzPerBin = a.envHzPerBin; tel.envLineHz = a.envLineHz;
        tel.powSpecDb = a.powSpecDb; tel.powM = a.powM; tel.powHzPerBin = a.powHzPerBin; tel.powLineHz = a.powLineHz;
        if (a.rateFound) {
            tel.baud = a.baud; tel.baudRaw = a.baudRaw; tel.baudConf = a.baudConf; tel.baudSnapped = a.baudSnapped;
            tel.order = (int)a.order; tel.orderConf = a.orderConf;
            tel.orderEvm.clear();
            for (const auto& e : a.orderEvm) tel.orderEvm.push_back({(int)e.first, e.second});
            tel.shape = a.shape; tel.alpha = a.alpha; tel.rect = a.rect;
            tel.dopplerHzPerSec = a.dopplerHzPerSec;
            tel.evm = a.evm; tel.merDb = a.merDb; tel.esn0Db = a.esn0Db; tel.ebn0Db = a.ebn0Db; tel.bitRate = a.bitRate;
            if (!a.symbols.empty()) {
                const size_t k = std::min(a.symbols.size(), kPskdConstMax);
                tel.constellation.assign(a.symbols.end() - (long)k, a.symbols.end());
            }
        }
        for (const auto& l : a.log) addLog(l);
    }

    void analyseNow() {
        if (locked) return;
        const size_t nF = histF.size();
        if (zoomOn) {
            const size_t nZ = histZ.size();
            if ((double)nZ < 0.5 * zoomRate) return;
            pskd::Analysis a = pskd::analyse(histZ.tail(nZ), nZ, zoomRate, options(30, std::max(200.0, 0.25 * zoomPass), zoomPass));
            copyAnalysis(a);
            if (a.signal) tel.cfoHz = zoomOffset + a.carrier.offsetHz;
            if (a.locked) { lockOn(a, a.carrierHz, true); return; }
            if (!a.signal && ++zoomMisses >= 4) { zoomOn = false; histZ.clear(); addLog("the narrow signal went away"); }
            tel.state = a.signal ? 1 : 0;
            return;
        }
        if ((double)nF < 0.25 * frontRate) return;
        pskd::Analysis a = pskd::analyse(histF.tail(nF), nF, frontRate, options(2, span / 2, span));
        copyAnalysis(a);
        if (!a.signal) { tel.state = 0; return; }
        tel.state = 1;
        tel.cfoHz = a.carrier.offsetHz;
        if (a.locked) { lockOn(a, a.carrierHz, false); return; }
        // a narrow signal in a wide span (or one whose rate is not clear yet): look at it on its own, over a longer time
        const double w = std::max({a.carrier.occHz, a.carrier.bw10Hz, a.carrier.bw3Hz * 1.5});
        if (w < span / 6 || !a.rateFound) {
            const double pass = std::max({w * 2.5, a.carrier.bw3Hz * 4, 400.0});
            if (pass < frontRate * 0.5) {
                zoomOffset = a.carrier.offsetHz;
                zoom.configure(frontRate, zoomOffset, pass, 0);
                zoomPass = pass;
                zoomRate = zoom.outRate();
                histZ.clear();
                histZ.cap = (size_t)std::min(30.0 * zoomRate, 4e6);
                // start it from the front history so the analysis does not wait
                scratch2.clear();
                zoom.process(histF.tail(nF), nF, scratch2);
                histZ.push(scratch2);
                zoomOn = true; zoomMisses = 0;
                char b[160];
                std::snprintf(b, sizeof b, "narrow signal at %+.1f Hz: zooming in (%.0f Hz wide channel)", zoomOffset, pass);
                addLog(b);
            }
        }
    }

    // carrier: from the front channel's centre, or from the zoom's when fromZoom
    void lockOn(const pskd::Analysis& a, double carrier, bool fromZoom) {
        order = a.order; baud = a.baud;
        viaZoom = fromZoom;
        const double carrierFront = (fromZoom ? zoomOffset : 0) + carrier;
        const double occ = a.rect ? 2.4 * baud : (1 + a.alpha) * baud * 1.25;
        chan.configure(fromZoom ? zoomRate : frontRate, carrier, std::max(occ, baud * 1.3), 6 * baud);
        pskd::DemodConfig dc;
        dc.order = order; dc.sps = chan.outRate() / baud; dc.alpha = a.rect ? 0 : a.alpha;
        demod.configure(dc);
        pskd::BitOptions bo;
        bo.diff = (pskd::Diff)cfg.diff; bo.scr = (pskd::Scrambler)cfg.scrambler; bo.sync = syncBits; bo.invert = cfg.invert;
        bitdec.configure(order, bo);
        tailForSync.clear();
        locked = true; badChecks = 0; evmSmooth = a.evm;
        tel.state = 2;
        tel.cfoHz = carrierFront;
        char b[200];
        std::snprintf(b, sizeof b, "locked: %s %.2f Bd (%s) at %+.1f Hz from the dial, Eb/N0 about %.1f dB", pskd::orderName(order), baud, a.shape.c_str(), carrierFront, a.ebn0Db);
        addLog(b);
        logf(b);
        // demodulate the history first, so a burst that was being analysed is not lost
        const Hist& h = fromZoom ? histZ : histF;
        const size_t n = h.size();
        scratch3.clear();
        chan.process(h.tail(n), n, scratch3);
        demodulate(scratch3);
        if (fromZoom) histZ.clear();
        else { zoomOn = false; histZ.clear(); }
    }

    void demodulate(const std::vector<pskd::cf>& y) {
        syms.clear(); eyeNew.clear();
        demod.process(y.data(), y.size(), syms, nullptr, nullptr, &eyeNew);
        if (syms.empty()) return;
        newBits.clear();
        bitdec.push(syms.data(), syms.size(), newBits);
        tel.symbols += syms.size();
        // constellation and eye
        auto& c = tel.constellation;
        c.insert(c.end(), syms.begin(), syms.end());
        if (c.size() > kPskdConstMax) c.erase(c.begin(), c.end() - (long)kPskdConstMax);
        auto& e = tel.eye;
        e.insert(e.end(), eyeNew.begin(), eyeNew.end());
        if (e.size() > kPskdEyeSyms * 16) e.erase(e.begin(), e.end() - (long)(kPskdEyeSyms * 16));
        addBits(newBits);
    }

    void addBits(const pskd::Bits& nb) {
        if (nb.empty()) return;
        // sync words in the output (the bit decoder already inverted the output when asked)
        if (!syncBits.empty()) {
            pskd::Bits want = syncBits;
            if (cfg.invert) for (auto& v : want) v ^= 1;
            const size_t L = want.size();
            const uint64_t base = tel.bitsTotal - tailForSync.size();
            pskd::Bits w(tailForSync.begin(), tailForSync.end());
            w.insert(w.end(), nb.begin(), nb.end());
            const int maxErr = (int)L / 12;
            for (size_t at : pskd::findSync(w, want, maxErr)) {
                if (at <= tailForSync.size()) continue;   // found before (it ended in the old tail)
                tel.syncAt.push_back(base + at);
                tel.syncs++; tel.blocksOk++;
            }
            if (tel.syncAt.size() > 256) tel.syncAt.erase(tel.syncAt.begin(), tel.syncAt.end() - 256);
            const size_t keep = std::min(w.size(), L > 0 ? L - 1 : 0);
            tailForSync.assign(w.end() - (long)keep, w.end());
        }
        auto& b = tel.bits;
        b.insert(b.end(), nb.begin(), nb.end());
        tel.bitsTotal += nb.size();
        if (b.size() > kPskdBitsMax) { const size_t d = b.size() - kPskdBitsMax; b.erase(b.begin(), b.begin() + (long)d); }
        tel.bitsBase = tel.bitsTotal - b.size();
        // saving
        if (cfg.saveBits && !cfg.savePath.empty()) {
            if (savePathOpen != cfg.savePath) {
                if (save.is_open()) save.close();
                save.open(cfg.savePath, std::ios::binary | std::ios::app);
                savePathOpen = cfg.savePath; saveN = 0; saveAcc = 0;
                if (!save) { addLog("could not open " + cfg.savePath + " for the bits"); savePathOpen.clear(); }
            }
            if (save.is_open()) {
                for (uint8_t v : nb) {
                    saveAcc = (uint8_t)((saveAcc << 1) | (v & 1));
                    if (++saveN == 8) { save.put((char)saveAcc); saveN = 0; saveAcc = 0; tel.bytesSaved++; }
                }
                save.flush();
                tel.savingTo = cfg.savePath;
            }
        } else if (save.is_open()) { save.close(); savePathOpen.clear(); tel.savingTo.clear(); }
    }

    void track() {
        if (!locked) return;
        const pskd::DemodStats st = demod.stats();
        tel.lockMetric = st.lockMetric;
        evmSmooth = st.evm;
        tel.evm = st.evm;
        tel.merDb = st.evm > 0 ? -20 * std::log10(st.evm) : 60;
        tel.esn0Db = tel.merDb;
        tel.ebn0Db = tel.esn0Db - 10 * std::log10((double)pskd::orderBits(order));
        const double fHz = st.freq * chan.outRate() / (2 * kPi);
        if (st.spsNow > 0) tel.baudRaw = chan.outRate() / st.spsNow;
        tel.cfoHz = (viaZoom ? zoomOffset : 0) + chan.offset() + fHz;
        // follow the carrier: keep it in the middle of the channel filter (and the zoom's)
        if (cfg.followDoppler && std::fabs(fHz) > 0.01 * baud) {
            chan.setOffset(chan.offset() + fHz);
            demod.addFreq(-st.freq);
        }
        if (cfg.followDoppler && viaZoom && std::fabs(chan.offset()) > 0.2 * zoomPass) {
            zoomOffset += chan.offset();
            zoom.setOffset(zoomOffset);
            chan.setOffset(0);
        }
        tel.rotation = bitdec.rotation(); tel.conj = bitdec.conj(); tel.railShift = bitdec.railShift(); tel.ambiguityResolved = bitdec.resolved() && !syncBits.empty();
        // lost: the constellation has been bad for a while (a few seconds, and at least a few hundred symbols)
        if (st.symbols > 200 && evmSmooth > 1.4 * lockEvmFor(order)) badChecks++;
        else badChecks = 0;
        const double checksNeeded = std::max(20.0, 400.0 / std::max(1.0, baud) / 0.1);
        if (badChecks > checksNeeded) {
            unlock(true);
            nextAnalyse = (double)nIn;
            tel.status = "signal lost, searching";
        }
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        nIn = 0; nextReport = 0.25 * std::max(1.0, rate); nextTrack = 0;
        power = 0; nPower = 0;
        tel.timeSec = 0;
        dirty = true;
    }
    void clearAll() {
        tel.bits.clear(); tel.syncAt.clear(); tel.bitsTotal = 0; tel.bitsBase = 0; tel.syncs = 0; tel.blocksOk = 0; tel.blocksBad = 0;
        tel.symbols = 0; tel.log.clear(); tel.bytesSaved = 0; tailForSync.clear();
    }
    void report() {
        tel.seq++;
        tel.timeSec = rate > 0 ? (double)nIn / rate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.chanRate = locked ? chan.outRate() : zoomOn ? zoomRate : frontRate;
        tel.dataValid = tel.state == 2;
        if (locked) {
            char b[160];
            std::snprintf(b, sizeof b, "locked at %.0f baud %s, Eb/N0 %.0f dB", baud, pskd::orderName(order), tel.ebn0Db);
            tel.status = b;
            tel.order = (int)order; tel.baud = baud;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

PskdReceiver::PskdReceiver() : p_(std::make_unique<Impl>()) {}
PskdReceiver::~PskdReceiver() = default;
void PskdReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void PskdReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool PskdReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= pskdTuning().minSampleRate - 1; }
void PskdReceiver::reset() { p_->resetReq = true; }
void PskdReceiver::reanalyse() { p_->reanalyseReq = true; }
void PskdReceiver::setSettings(const PskdSettings& s) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfgSet = sanitize(s); p_->dirty = true; }
PskdSettings PskdReceiver::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfgSet; }
void PskdReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void PskdReceiver::clearResults() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.bits.clear(); p_->pub.syncAt.clear(); p_->pub.log.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}

void PskdReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    if (m.dirty.load()) m.rebuild();
    if (m.reanalyseReq.exchange(false)) { m.unlock(false); m.zoomOn = false; m.histZ.clear(); m.nextAnalyse = (double)m.nIn; m.addLog("analysing again"); }
    if (m.rate <= 0 || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        const float re = x[i].real(), im = x[i].imag();
        if (std::isfinite(re) && std::isfinite(im)) m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.scratch.clear();
    m.front.process(x, n, m.scratch);
    m.histF.push(m.scratch);
    if (m.zoomOn) {
        m.scratch2.clear();
        m.zoom.process(m.scratch.data(), m.scratch.size(), m.scratch2);
        if (!m.locked) m.histZ.push(m.scratch2);
    }
    if (m.locked) {
        const std::vector<pskd::cf>& in = m.viaZoom ? m.scratch2 : m.scratch;
        m.scratch3.clear();
        m.chan.process(in.data(), in.size(), m.scratch3);
        m.demodulate(m.scratch3);
    }
    m.nIn += (int64_t)n;
    if (m.locked && (double)m.nIn >= m.nextTrack) { m.nextTrack = (double)m.nIn + 0.1 * m.rate; m.track(); }
    if (!m.locked && (double)m.nIn >= m.nextAnalyse) { m.nextAnalyse = (double)m.nIn + 0.5 * m.rate; m.analyseNow(); }
    while ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
}

bool PskdReceiver::telemetry(PskdTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning pskdTuning() {
    ModeTuning t;
    t.stdMode = 204; t.id = "pskd"; t.name = "PSK demodulator";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 435.0;   // the amateur satellite band; any frequency works
    t.sampleRate = 2000000;
    t.bandwidthMhz = 0.2;
    t.minSampleRate = 8000;                              // an IQ recording or a sound card at 8 kHz holds a slow signal
    t.tuneOffsetHz = 250000;                             // the radio sits 250 kHz above the dial: the signal stays off the DC spike
    return t;
}

// ------------------------------------------------------------------ test signal
namespace {
class PskdSynth : public ModeSynth {
public:
    PskdSynth(const SynthConfig& c, double fs) : fs_(fs) {
        pskd::GenSpec g;
        const pskd::Order ords[] = {pskd::Order::Bpsk, pskd::Order::Qpsk, pskd::Order::Oqpsk, pskd::Order::Psk8};
        g.order = ords[std::clamp(c.modeOpt[0], 0, 3)];
        g.diff = (pskd::Diff)std::clamp(c.modeOpt[1], 0, 3);
        g.scr = (pskd::Scrambler)std::clamp(c.modeOpt[2], 0, 2);
        g.baud = c.modeVal[0] > 0 ? c.modeVal[0] : 1200;
        g.alpha = c.modeVal[1] == 0 ? 0.35 : c.modeVal[1] < 0 ? 0 : std::min(1.0, c.modeVal[1]);
        dopp_ = c.modeVal[2];
        // the baseband loop at 8 samples per symbol, about 3 s long
        g.fs = g.baud * 8; g.offsetHz = 0; g.esn0Db = 300;
        g.sync = pskd::parseBitsOrHex("1ACFFC1D");
        g.payloadBits = 960;
        g.frames = std::max(4, (int)std::ceil(3.0 * g.baud * pskd::orderBits(g.order) / 992.0));
        g.frames = std::min(g.frames, 4000);
        g.seed = 7;
        loop_ = pskd::generate(g);
        bfs_ = g.fs;
        double p = 0;
        for (const auto& v : loop_) p += std::norm(v);
        p /= (double)std::max<size_t>(1, loop_.size());
        // noise for that SNR (Es/N0) in the symbol rate's bandwidth; the total stays below about 0.3 rms (the source rounds to 8 bits),
        // so a weak setting lowers the signal instead of raising the noise
        double amp = 0.25;
        double sig = amp * std::sqrt(fs / g.baud) / std::pow(10.0, c.snrDb / 20) / std::sqrt(2.0);
        if (sig * std::sqrt(2.0) > 0.3) { const double s = 0.3 / (sig * std::sqrt(2.0)); amp *= s; sig *= s; }
        sigma_ = (float)sig;
        const float k = p > 0 ? (float)(amp / std::sqrt(p)) : 1.f;
        for (auto& v : loop_) v *= k;
        // dial at -tuneOffset in the radio's samples (a file or a slow source keeps it at 0)
        const double off = fs > 2 * pskdTuning().tuneOffsetHz + 4 * g.baud ? -pskdTuning().tuneOffsetHz : 0;
        cfo_ = off + c.cfoHz;
        step_ = bfs_ / fs;
        rng_ = 12345u;
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        const size_t L = loop_.size();
        for (size_t i = 0; i < n; i++) {
            const double t = pos_;
            const size_t k = (size_t)t;
            const double mu = t - (double)k;
            pskd::cf v4[4];
            for (int j = 0; j < 4; j++) v4[j] = loop_[(k + L - 1 + (size_t)j) % L];
            pskd::cf v = pskd::interpCubic(v4, mu);
            const double f = cfo_ + dopp_ * time_;
            v *= pskd::cf((float)std::cos(ph_), (float)std::sin(ph_));
            ph_ += 2 * kPi * f / fs_;
            if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
            out[i] = v + cf32(gauss() * sigma_, gauss() * sigma_);
            pos_ += step_;
            if (pos_ >= (double)L) pos_ -= (double)L;
            time_ += 1.0 / fs_;
            if (dopp_ != 0 && time_ > 60) time_ = -60;   // a ramp from -60 s to +60 s, then again
        }
    }
private:
    float gauss() {
        // Box-Muller from a small LCG
        rng_ = rng_ * 1664525u + 1013904223u; const double u1 = ((rng_ >> 8) + 1.0) / 16777217.0;
        rng_ = rng_ * 1664525u + 1013904223u; const double u2 = (rng_ >> 8) / 16777216.0;
        return (float)(std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2));
    }
    double fs_, bfs_ = 0, step_ = 1, pos_ = 0, cfo_ = 0, dopp_ = 0, ph_ = 0, time_ = 0;
    float sigma_ = 0;
    uint32_t rng_;
    std::vector<pskd::cf> loop_;
};
} // namespace

std::unique_ptr<ModeSynth> makePskdSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<PskdSynth>(cfg, sampleRate);
}

} // namespace dect2
