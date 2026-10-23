// LoRa satellite receiver (see lorasat_rx.h).
#include "dect2/lorasat/lorasat_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/lorasat/lorasat_presets.h"
#include "dect2/lorawan/lora_bank.h"
#include "dect2/net/feeds.h"
#include "dect2/results/results_io.h"
#include "dect2/common/str.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {
LoraSatSettings sanitize(LoraSatSettings s) {
    s.sf = std::max(5, std::min(12, s.sf));
    s.bwKhz = s.bwKhz <= 41 ? 41 : s.bwKhz <= 62 ? 62 : s.bwKhz <= 125 ? 125 : s.bwKhz <= 250 ? 250 : 500;
    s.sync = s.sync < 0 ? -1 : std::min(255, s.sync);
    s.preamble = std::max(6, std::min(65, s.preamble));
    s.dopplerMaxHz = std::max(0.0, std::min(60e3, s.dopplerMaxHz));
    s.offsetHz = std::max(-1e6, std::min(1e6, s.offsetHz));
    return s;
}
std::string hexOf(const std::vector<uint8_t>& p) { return dect2::str::toHex(p.data(), p.size(), true, ' ', 255); }
std::string textOf(const std::vector<uint8_t>& p) {
    std::string s;
    for (size_t i = 0; i < p.size() && i < 255; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    return s;
}
} // namespace

struct LoraSatReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0, centerSet = 0;
    LoraSatSettings cfgSet;
    int trackedNorad = 0;
    std::string trackedName;
    std::atomic<bool> dirty{true}, resetReq{true}, clearReq{false};
    std::function<void(const std::string&)> log;
    LoraSatTelemetry pub;
    bool resClear = false, resLoad = false;
    LoraSatTelemetry loaded;
    double loadedAt = 0;

    lorabank::Bank bank;
    std::vector<lorabank::Decoded> got;
    double rate = 0, offset = 0, center = 0;
    LoraSatSettings cfg;
    LoraSatTelemetry tel;
    int64_t nIn = 0;
    double nextReport = 0, lastGood = -1e9;
    double power = 0; int64_t nPower = 0;

    void logf(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }
    static void shiftTimes(LoraSatTelemetry& t, double dt) {
        for (auto& f : t.log) f.sec += dt;
        for (auto& d : t.doppler) d.sec += (float)dt;
    }
    static void moveLists(LoraSatTelemetry& to, LoraSatTelemetry& from) {
        to.log = std::move(from.log); to.doppler = std::move(from.doppler);
        from.log.clear(); from.doppler.clear();
    }
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { LoraSatTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
    }

    void rebuild() {
        {
            std::lock_guard<std::mutex> lk(mu);
            rate = rateSet; offset = offsetSet; center = centerSet; cfg = sanitize(cfgSet);
            dirty = false;
        }
        tel.decoders.clear();
        tel.inputRate = rate; tel.cfg = cfg; tel.centerHz = center; tel.centreKnown = center > 0;
        tel.dialHz = center > 0 ? center + offset : 0;
        tel.presetRunning = -1;
        std::vector<lorabank::Channel> chans;
        if (rate <= 0) { bank.configure(0, chans); return; }
        std::vector<double> bws;
        std::vector<int> sfs, syncs;
        int pr = -1;
        if (cfg.followDial && tel.dialHz > 0) pr = lorasat::nearest(tel.dialHz / 1e6);
        if (pr >= 0) {
            const lorasat::Preset& p = lorasat::presets()[(size_t)pr];
            bws = {lorasat::bandwidthHz(p.bwKhz)};
            if (p.sf > 0) sfs = {p.sf}; else if (cfg.autoSf) for (int s = 7; s <= 12; s++) sfs.push_back(s); else sfs = {cfg.sf};
            if (p.sync >= 0) syncs = {p.sync}; else syncs = {0x12, 0x34};
            tel.presetRunning = pr;
        } else {
            if (cfg.autoBw) bws = {125e3, 250e3, 500e3};
            else bws = {lorasat::bandwidthHz(cfg.bwKhz)};
            if (cfg.autoSf) for (int s = 7; s <= 12; s++) sfs.push_back(s); else sfs = {cfg.sf};
            if (cfg.sync < 0) syncs = {0x12, 0x34}; else syncs = {cfg.sync};
        }
        const double mhz = tel.dialHz > 0 ? tel.dialHz / 1e6 : 436.0;
        tel.dopplerMaxHz = cfg.dopplerMaxHz > 0 ? cfg.dopplerMaxHz : 1.15 * lorasat::maxDopplerHz(mhz) + 2e3;
        tel.filterHz = 0;
        for (double bw : bws) {
            lorabank::Channel c;
            c.offsetHz = offset + cfg.offsetHz; c.bwHz = bw; c.filterBwHz = bw + 2 * tel.dopplerMaxHz;
            c.sfs = sfs; c.syncs = syncs; c.cfoTrack = cfg.track ? 1.0 : 0.0; c.preamble = cfg.preamble;
            c.marginHz = 10e3; c.carrierHz = mhz * 1e6;
            chans.push_back(c);
            tel.filterHz = std::max(tel.filterHz, c.filterBwHz);
        }
        bank.configure(rate, chans);
        for (const auto& s : bank.stats()) {
            LsDecoderInfo d;
            d.sf = s.sf; d.bwHz = bws[s.chan]; d.sync = s.sync; d.inBand = s.inBand;
            tel.decoders.push_back(d);
        }
        char b[220];
        snprintf(b, sizeof b, "LoRa satellites: %s%s, channel filter %.0f kHz for a Doppler shift of up to +-%.1f kHz, carrier %s", pr >= 0 ? "preset " : "manual settings",
                 pr >= 0 ? lorasat::presets()[(size_t)pr].name : "", tel.filterHz / 1e3, tel.dopplerMaxHz / 1e3, cfg.track ? "followed through each frame" : "measured once per frame");
        logf(b);
    }

    void onFrame(const lorabank::Decoded& d) {
        const bool good = d.f.crcOk || !d.f.hdr.crc;
        LsFrame r;
        r.wallTime = (int64_t)std::time(nullptr);
        r.sec = d.sec; r.sf = d.sf; r.bwHz = d.bwHz; r.cr = d.f.hdr.cr; r.hasCrc = d.f.hdr.crc; r.crcOk = d.f.crcOk; r.sync = d.sync;
        r.len = (int)d.f.payload.size(); r.corrected = d.f.corrected;
        r.snrDb = (float)d.f.snrDb; r.levelDb = (float)d.f.levelDb;
        r.cfoHz = d.f.cfoHz; r.cfoStartHz = cfg.track ? d.f.cfoStartHz : d.f.cfoHz; r.rateHzS = cfg.track ? d.f.cfoRateHzS : 0;
        r.freqHz = (tel.dialHz > 0 ? tel.dialHz : 0) + cfg.offsetHz + r.cfoStartHz;
        { std::lock_guard<std::mutex> lk(mu); r.sat = trackedName; }
        r.hex = hexOf(d.f.payload); r.ascii = textOf(d.f.payload);
        if (tel.decoders.size() > 0) {
            for (auto& di : tel.decoders) if (di.sf == d.sf && di.sync == d.sync && std::fabs(di.bwHz - d.bwHz) < 1) { di.frames++; if (!good) di.crcBad++; break; }
        }
        if (good) {
            tel.blocksOk++; tel.dataValid = true; lastGood = bank.timeSec();
            tel.cfoHz = r.cfoStartHz; tel.snrDb = r.snrDb;
            if (r.sf >= 5 && r.sf <= 12) tel.perSf[r.sf]++;
            LsDopplerPoint pt; pt.sec = (float)r.sec; pt.hz = (float)r.cfoStartHz; pt.rate = (float)r.rateHzS;
            tel.doppler.push_back(pt);
            if (tel.doppler.size() > kLsTrackMax) tel.doppler.erase(tel.doppler.begin());
            if (Feeds::get().wantLora()) Feeds::get().lora("lorasat", r.hex, r.snrDb, r.freqHz, r.sf);
            if (automation::wants(automation::FLora)) {
                automation::Event ev("lora.packet");
                ev.at(automation::wallNow(), "LoRa satellite", r.freqHz);
                ev.str("kind", "lorasat").str("hex", r.hex).num("snr_db", r.snrDb).num("sf", r.sf);
                if (!r.sat.empty()) ev.str("satellite", r.sat);
                automation::emit(std::move(ev));
            }
        } else tel.blocksBad++;
        tel.log.insert(tel.log.begin(), std::move(r));
        if (tel.log.size() > kLsLogMax) tel.log.pop_back();
    }

    void afterFeed() {
        for (const auto& d : got) onFrame(d);
        got.clear();
        const auto& st = bank.stats();
        uint64_t pre = 0, hb = 0;
        for (size_t i = 0; i < st.size() && i < tel.decoders.size(); i++) {
            tel.decoders[i].preambles = st[i].preambles; tel.decoders[i].headerBad = st[i].headerBad; tel.decoders[i].syncBad = st[i].syncBad;
            pre += st[i].preambles; hb += st[i].headerBad;
        }
        tel.preambles = pre; tel.headerBad = hb;
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; center = centerSet; }
        nIn = 0; nextReport = 0.25 * rate;
        shiftTimes(tel, -tel.timeSec);
        tel.timeSec = 0; lastGood = -1e9;
        power = 0; nPower = 0;
        dirty = true;
    }
    void clearAll() {
        const uint64_t seq = tel.seq;
        const auto dec = tel.decoders;
        const LoraSatSettings c = tel.cfg;
        const LoraSatTelemetry keep = tel;
        tel = LoraSatTelemetry(); tel.seq = seq; tel.inputRate = keep.inputRate; tel.cfg = c; tel.decoders = dec; tel.centerHz = keep.centerHz; tel.dialHz = keep.dialHz;
        tel.centreKnown = keep.centreKnown; tel.presetRunning = keep.presetRunning; tel.dopplerMaxHz = keep.dopplerMaxHz; tel.filterHz = keep.filterHz;
        for (auto& d : tel.decoders) d.frames = d.crcBad = d.preambles = d.headerBad = d.syncBad = 0;
    }
    void report() {
        tel.seq++;
        tel.timeSec = bank.timeSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.state = tel.timeSec - lastGood < 8 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        { std::lock_guard<std::mutex> lk(mu); tel.tracked = trackedName; tel.trackedNorad = trackedNorad; pub = tel; }
    }
};

LoraSatReceiver::LoraSatReceiver() : p_(std::make_unique<Impl>()) {}
LoraSatReceiver::~LoraSatReceiver() = default;
void LoraSatReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void LoraSatReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void LoraSatReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->centerSet) { p_->centerSet = hz; p_->resetReq = true; } }
bool LoraSatReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= loraSatTuning().minSampleRate - 1; }
void LoraSatReceiver::reset() { p_->resetReq = true; }
void LoraSatReceiver::setSettings(const LoraSatSettings& s) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfgSet = sanitize(s); p_->dirty = true; }
LoraSatSettings LoraSatReceiver::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfgSet; }
void LoraSatReceiver::setTracked(int norad, const std::string& name) { std::lock_guard<std::mutex> lk(p_->mu); p_->trackedNorad = norad; p_->trackedName = name; }
void LoraSatReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        LoraSatTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resClear = true; p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void LoraSatReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.dirty.load()) m.rebuild();
    if (m.rate <= 0 || n == 0) return;
    for (size_t i = 0; i < n; i++) { const float re = x[i].real(), im = x[i].imag(); if (std::isfinite(re) && std::isfinite(im)) m.power += (double)re * re + (double)im * im; }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.rate >= loraSatTuning().minSampleRate - 1) { m.bank.feed(x, n, m.got); m.afterFeed(); }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.rate;
        m.report();
    }
}

bool LoraSatReceiver::telemetry(LoraSatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
std::string LoraSatReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.doppler);
}
bool LoraSatReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    LoraSatTelemetry l;
    if (!results::unpack(blob, t0, l.log, l.doppler)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    LoraSatTelemetry c;
    c.log = l.log; c.doppler = l.doppler;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
void LoraSatReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning loraSatTuning() {
    ModeTuning t;
    t.stdMode = 203; t.id = "lorasat"; t.name = "LoRa satellites";
    t.minMhz = 137; t.maxMhz = 1000;
    t.defMhz = 436.8;                // the FOSSA Systems pico-satellites (uncertain: see lorasat_presets.h)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 300000;         // away from the radio's DC spike; the Doppler shift of up to +-20 kHz is followed by the decoder
    return t;
}

} // namespace dect2
