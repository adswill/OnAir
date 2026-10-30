// LoRaWAN monitor (see lorawan_rx.h): a bank of LoRa channels (lora_bank.h) over the channels of a regional plan, and the MAC layer parser
// (lorawan_plan.h) on every frame. Keyless: nothing is decrypted.
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/lorawan/lora_bank.h"
#include "dect2/lorawan/lorawan_fsk.h"
#include "dect2/lorawan/lorawan_plan.h"
#include "dect2/net/feeds.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
LorawanSettings sanitize(LorawanSettings s) {
    s.plan = std::max(0, std::min((int)lorawan::kPlans - 1, s.plan));
    s.subBand = std::max(0, std::min(8, s.subBand));
    s.maxChannels = std::max(1, std::min(72, s.maxChannels));
    return s;
}

struct Plan {
    std::vector<lorawan::PlanChannel> ch;   // what the settings select, ascending
};

std::string mhzText(double mhz) { char b[32]; snprintf(b, sizeof b, "%.3f", mhz); return b; }
} // namespace

double lorawanPlanCentreMhz(int plan) {
    switch (plan) {
    case lorawan::EU868: return 868.3;
    case lorawan::US915: return 904.6;      // sub-band 2 (channels 8 - 15), the one The Things Network and most US networks use
    case lorawan::AU915: return 917.5;      // sub-band 2
    case lorawan::AS923_1: return 923.9;
    case lorawan::AS923_2: return 922.1;
    case lorawan::AS923_3: return 917.3;
    case lorawan::AS923_4: return 918.0;
    case lorawan::EU433: return 433.9;
    }
    return 868.3;
}

struct LorawanReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0, centerSet = 0;
    LorawanSettings cfgSet;
    std::atomic<bool> dirty{true}, resetReq{true}, clearReq{false};
    std::function<void(const std::string&)> log;
    LorawanTelemetry pub;
    bool resClear = false, resLoad = false;
    LorawanTelemetry loaded;
    double loadedAt = 0;

    // analysis thread
    lorabank::Bank bank;
    std::vector<lorabank::Decoded> got;
    double rate = 0, offset = 0, center = 0;
    LorawanSettings cfg;
    LorawanTelemetry tel;
    std::vector<lorawan::PlanChannel> planCh;   // the channels of the bank, by bank index
    std::vector<double> chanFreq;               // absolute (or dial-relative) centre of each bank channel
    std::vector<size_t> planIndex;              // bank channel -> index in the plan's ascending list
    lorawan::fsk::Receiver fskRx;               // the plan's FSK (DR7) channel(s), over the same capture
    std::vector<lorawan::fsk::RxFrame> fskGot;
    std::vector<double> fskFreq;                // absolute (or dial-relative) centre of each FSK channel
    std::vector<size_t> fskTel;                 // FSK channel -> index in tel.channels
    bool fskOn = false;
    int64_t nIn = 0;
    double nextReport = 0, lastGood = -1e9;
    double power = 0; int64_t nPower = 0;
    double dialHz = 0;

    void logf(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    static void shiftTimes(LorawanTelemetry& t, double dt) {
        for (auto& f : t.log) f.sec += dt;
        for (auto& d : t.devices) if (d.lastHeard >= 0) d.lastHeard += dt;
    }
    static void moveLists(LorawanTelemetry& to, LorawanTelemetry& from) {
        to.log = std::move(from.log); to.devices = std::move(from.devices);
        from.log.clear(); from.devices.clear();
    }

    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { LorawanTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
    }

    void rebuild() {
        {
            std::lock_guard<std::mutex> lk(mu);
            rate = rateSet; offset = offsetSet; center = centerSet; cfg = sanitize(cfgSet);
            dirty = false;
        }
        tel.cfg = cfg; tel.inputRate = rate; tel.centerHz = center; tel.centreKnown = center > 0;
        tel.channels.clear(); tel.note.clear();
        planCh.clear(); chanFreq.clear(); planIndex.clear();
        fskFreq.clear(); fskTel.clear(); fskOn = false;
        std::vector<lorabank::Channel> bc;
        if (rate <= 0) { bank.configure(0, bc); fskRx.configure(0, {}); return; }
        // where the dial is: the radio's centre plus the signal offset; unknown centre: the plan's own dial
        dialHz = center > 0 ? center + offset : lorawanPlanCentreMhz(cfg.plan) * 1e6;
        const auto all = lorawan::planChannels(cfg.plan);
        auto sel = lorawan::selectChannels(cfg.plan, cfg.subBand, cfg.downlinks);
        // every channel with its place in the input; those that fit first, nearest the centre first
        struct Cand { lorawan::PlanChannel c; double off; size_t idx; bool fits; };
        std::vector<Cand> cands;
        for (const auto& c : sel) {
            if (cfg.defaultsOnly && !c.dflt) continue;
            if (!cfg.include250_500 && c.bwKhz != 125) continue;
            Cand k;
            k.c = c; k.off = c.mhz * 1e6 - dialHz + offset;
            k.idx = 0;
            for (size_t i = 0; i < all.size(); i++) if (std::fabs(all[i].mhz - c.mhz) < 1e-6 && all[i].bwKhz == c.bwKhz) k.idx = i;
            lorabank::Channel probe; probe.offsetHz = k.off; probe.bwHz = c.bwKhz * 1e3;
            k.fits = lorabank::Bank::fits(rate, probe);
            cands.push_back(k);
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.fits != b.fits ? a.fits : std::fabs(a.off) < std::fabs(b.off); });
        size_t nFit = 0, used = 0;
        for (const Cand& k : cands) if (k.fits) nFit++;
        std::vector<Cand> run;
        for (const Cand& k : cands) {
            if (k.fits && used < (size_t)cfg.maxChannels) { run.push_back(k); used++; }
            else if (k.fits) { Cand z = k; z.fits = false; run.push_back(z); }      // listed, not run: over the channel limit
            else run.push_back(k);
        }
        std::sort(run.begin(), run.end(), [](const Cand& a, const Cand& b) { return a.c.mhz != b.c.mhz ? a.c.mhz < b.c.mhz : a.c.bwKhz < b.c.bwKhz; });
        for (const Cand& k : run) {
            lorabank::Channel c;
            c.offsetHz = k.off; c.bwHz = k.c.bwKhz * 1e3;
            c.syncs = {0x34};
            if (cfg.privateSync) c.syncs.push_back(0x12);
            int upLo = 0, upHi = 0, dnLo = 0, dnHi = 0;
            lorawan::planSfRange(cfg.plan, k.c.bwKhz, false, upLo, upHi);
            lorawan::planSfRange(cfg.plan, k.c.bwKhz, true, dnLo, dnHi);
            const bool wantDn = cfg.downlinks && k.c.downlink;
            c.normalIq = k.c.uplink;
            c.invertedIq = wantDn;
            const int lo = std::min(c.normalIq ? upLo : 99, wantDn ? dnLo : 99), hi = std::max(c.normalIq ? upHi : 0, wantDn ? dnHi : 0);
            for (int sf = lo; sf <= hi; sf++) c.sfs.push_back(sf);
            if (!k.fits) { c.sfs.clear(); }   // still listed by the bank as a channel outside the band (no decoders)
            bc.push_back(c);
            LwChannelInfo ci;
            ci.freqHz = k.c.mhz * 1e6; ci.bwHz = k.c.bwKhz * 1e3; ci.sfMin = lo; ci.sfMax = hi; ci.downlinkOnly = !k.c.uplink; ci.inBand = k.fits;
            tel.channels.push_back(ci);
            planCh.push_back(k.c); chanFreq.push_back(k.c.mhz * 1e6); planIndex.push_back(k.idx);
        }
        // channels that do not fit get no decoder at all: give them a channel with no SF so the bank keeps the indices
        bank.configure(rate, bc);
        tel.channelsListed = (uint32_t)bc.size();
        tel.channelsInBand = 0;
        for (size_t i = 0; i < bc.size(); i++) if (!bc[i].sfs.empty()) tel.channelsInBand++;
        tel.demodulators = (uint32_t)bank.decoders();
        // the FSK channel(s) of the plan: one more receiver over the same samples, listed after the LoRa channels
        std::vector<double> fskOff;
        uint32_t fskIn = 0;
        if (cfg.fsk) {
            for (double mhz : lorawan::planFskChannelsMhz(cfg.plan)) {
                const double off = mhz * 1e6 - dialHz + offset;
                LwChannelInfo ci;
                ci.freqHz = mhz * 1e6; ci.bwHz = 2 * lorawan::fsk::kBitRate; ci.fsk = true; ci.inBand = lorawan::fsk::Receiver::fits(rate, off);
                fskTel.push_back(tel.channels.size());
                tel.channels.push_back(ci);
                fskFreq.push_back(mhz * 1e6); fskOff.push_back(off);
                fskIn += ci.inBand;
            }
        }
        fskRx.configure(rate, fskOff);
        fskOn = fskIn > 0;
        char b[220];
        snprintf(b, sizeof b, "%u of %zu %s channels are decoded (%zu fit the %.2f Msps capture around %s MHz%s)%s", tel.channelsInBand, bc.size(), lorawan::planName(cfg.plan), nFit, rate / 1e6,
                 mhzText(dialHz / 1e6).c_str(), nFit > (size_t)cfg.maxChannels ? ", the rest is over the channel limit" : "",
                 fskIn ? ", and the FSK channel" : (fskOff.empty() ? "" : "; the FSK channel lies outside the capture"));
        tel.note = b;
        logf(std::string("LoRaWAN: ") + b);
    }

    void onFrame(const lorabank::Decoded& d) {
        LwFrame r;
        r.wallTime = (int64_t)std::time(nullptr);
        r.sec = d.sec;
        r.chan = d.chan < planIndex.size() ? (int)planIndex[d.chan] : -1;
        r.freqHz = d.chan < chanFreq.size() ? chanFreq[d.chan] : d.offsetHz;
        r.bwHz = d.bwHz; r.sf = d.sf; r.inverted = d.inverted;
        r.hasCrc = d.f.hdr.crc; r.crcOk = d.f.crcOk;
        r.snrDb = (float)d.f.snrDb; r.levelDb = (float)d.f.levelDb; r.cfoHz = d.f.cfoHz; r.len = (int)d.f.payload.size(); r.corrected = d.f.corrected;
        logFrame(std::move(r), d.f.payload, d.chan);
    }

    // a LoRaWAN FSK frame (only good CRCs come here)
    void onFskFrame(const lorawan::fsk::RxFrame& f) {
        LwFrame r;
        r.wallTime = (int64_t)std::time(nullptr);
        r.sec = f.sec;
        r.chan = -1;
        r.freqHz = f.chan < fskFreq.size() ? fskFreq[f.chan] : 0;
        r.bwHz = lorawan::fsk::kBitRate; r.sf = 0; r.fsk = true;
        r.hasCrc = true; r.crcOk = f.crcOk;
        r.snrDb = f.snrDb; r.levelDb = f.levelDb; r.cfoHz = f.cfoHz; r.len = (int)f.payload.size();
        tel.fskFrames++;
        logFrame(std::move(r), f.payload, f.chan < fskTel.size() ? fskTel[f.chan] : (size_t)-1);
    }

    // the MAC layer, the device table and the counters of a frame (LoRa or FSK); telChan: index in tel.channels
    void logFrame(LwFrame r, const std::vector<uint8_t>& payload, size_t telChan) {
        const bool good = r.crcOk || !r.hasCrc;
        r.hex = lorawan::hex(payload.data(), payload.size());
        if (good) {
            const lorawan::Frame m = lorawan::parse(payload.data(), payload.size());
            if (!m.ok && !r.hasCrc) return;      // a header-only frame (downlink) that is no LoRaWAN is most likely noise
            if (m.ok) {
                r.lorawan = true; r.mtype = m.mtype; r.uplink = m.uplink;
                r.type = lorawan::mtypeName(m.mtype);
                r.flags = m.flags; r.mic = m.mic; r.summary = m.summary;
                r.fport = m.fport; r.frmLen = (int)m.frmLen;
                if (m.mtype >= 2 && m.mtype <= 5) { r.devAddr = m.devAddr; r.hasDevAddr = true; r.addrType = m.addrType; r.nwkId = m.nwkId; r.fcnt = m.fcnt; }
                r.devEui = m.devEui; r.joinEui = m.joinEui; r.devNonce = m.devNonce;
                for (size_t i = 0; i < m.mac.size(); i++) {
                    r.mac += (i ? ", " : "") + m.mac[i].name;
                    if (!m.mac[i].detail.empty()) r.mac += " (" + m.mac[i].detail + ")";
                }
                updateDevice(r, m);
                if (m.mtype >= 0 && m.mtype < 8) tel.perType[m.mtype]++;
            } else { r.type = "not LoRaWAN"; r.summary = m.why; }
        } else r.type = "CRC error";
        if (telChan < tel.channels.size()) { tel.channels[telChan].frames++; if (!good) tel.channels[telChan].crcBad++; }
        if (good) {
            tel.blocksOk++; tel.dataValid = true; lastGood = sigSec();
            tel.cfoHz = r.cfoHz; tel.snrDb = r.snrDb;
            if (r.sf >= 5 && r.sf <= 12) tel.perSf[r.sf]++;
            if (Feeds::get().wantLora()) Feeds::get().lora("lorawan", r.hex, r.snrDb, r.freqHz, r.sf);
            if (automation::wants(automation::FLora)) {
                automation::Event ev("lora.packet");
                ev.at(automation::wallNow(), "LoRaWAN", r.freqHz);
                ev.str("kind", "lorawan").str("hex", r.hex).num("snr_db", r.snrDb).num("sf", r.sf).str("modulation", r.fsk ? "fsk" : "lora");
                if (r.hasDevAddr) { char b[16]; snprintf(b, sizeof b, "%08X", r.devAddr); ev.str("devaddr", b); }
                automation::emit(std::move(ev));
            }
        } else tel.blocksBad++;
        tel.log.insert(tel.log.begin(), std::move(r));
        if (tel.log.size() > kLwLogMax) tel.log.pop_back();
    }

    double sigSec() const { return bank.timeSec(); }

    void updateDevice(const LwFrame& r, const lorawan::Frame& m) {
        if (!r.hasDevAddr && r.devEui == 0) return;
        char idb[40];
        std::string id;
        if (r.hasDevAddr) { snprintf(idb, sizeof idb, "%08X", r.devAddr); id = idb; }
        else id = "DevEUI " + lorawan::eui(r.devEui);
        LwDevice* dv = nullptr;
        for (auto& d : tel.devices) if (d.id == id) { dv = &d; break; }
        if (!dv) {
            if (tel.devices.size() >= kLwDevMax) {
                size_t old = 0;
                for (size_t i = 1; i < tel.devices.size(); i++) if (tel.devices[i].lastHeard < tel.devices[old].lastHeard) old = i;
                tel.devices.erase(tel.devices.begin() + (long)old);
            }
            tel.devices.emplace_back();
            dv = &tel.devices.back();
            dv->id = id; dv->devAddr = r.devAddr; dv->hasDevAddr = r.hasDevAddr;
        }
        dv->lastHeard = r.sec; dv->freqHz = r.freqHz; dv->sf = r.sf; dv->fsk = r.fsk; dv->lastSnrDb = r.snrDb; dv->lastLevelDb = r.levelDb;
        dv->packets++;
        if (r.mtype == 0 || r.mtype == 6) { dv->joins++; dv->devEui = lorawan::eui(r.devEui); if (r.joinEui) dv->joinEui = lorawan::eui(r.joinEui); }
        if (r.hasDevAddr) {
            if (r.uplink) dv->uplinks++; else dv->downlinks++;
            if (m.confirmed) dv->confirmed++;
            if (r.uplink) {   // the counter of uplinks only (downlinks have their own)
                if (dv->fcnt >= 0) {
                    const int gap = (int)(uint16_t)(r.fcnt - (uint16_t)dv->fcnt);
                    if (gap > 1 && gap < 200) dv->missed += (uint32_t)(gap - 1);
                }
                dv->fcnt = r.fcnt;
            }
            if (r.fport >= 0) dv->lastFport = r.fport;
        }
        dv->lastType = r.type; dv->lastMac = r.mac;
    }

    void processBlockFrames() {
        for (const auto& d : got) onFrame(d);
        got.clear();
        const auto& st = bank.stats();
        uint64_t pre = 0, hb = 0;
        std::vector<uint64_t> chPre(tel.channels.size(), 0);
        for (const auto& s : st) { pre += s.preambles; hb += s.headerBad; if (s.chan < chPre.size()) chPre[s.chan] += s.preambles; }
        tel.preambles = pre; tel.headerBad = hb;
        for (size_t i = 0; i < chPre.size(); i++) tel.channels[i].preambles = chPre[i];
        // the FSK channel(s): frames, then the sync words found as "preambles" of the channel
        for (const auto& f : fskGot) onFskFrame(f);
        fskGot.clear();
        const auto& fs = fskRx.stats();
        uint64_t sy = 0, cb = 0;
        for (size_t i = 0; i < fs.size() && i < fskTel.size(); i++) {
            sy += fs[i].syncs; cb += fs[i].crcBad;
            if (fskTel[i] < tel.channels.size()) { tel.channels[fskTel[i]].preambles = fs[i].syncs; tel.channels[fskTel[i]].crcBad = fs[i].crcBad; }
        }
        tel.fskSyncs = sy; tel.fskCrcBad = cb;
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
        const auto chans = tel.channels;
        const LorawanSettings c = tel.cfg;
        const std::string note = tel.note;
        const uint32_t cl = tel.channelsListed, ci = tel.channelsInBand, dm = tel.demodulators;
        tel = LorawanTelemetry(); tel.seq = seq; tel.inputRate = rate; tel.cfg = c; tel.channels = chans; tel.note = note;
        tel.channelsListed = cl; tel.channelsInBand = ci; tel.demodulators = dm; tel.centerHz = center; tel.centreKnown = center > 0;
        for (auto& x : tel.channels) x.frames = x.crcBad = x.preambles = 0;
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.state = tel.timeSec - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

LorawanReceiver::LorawanReceiver() : p_(std::make_unique<Impl>()) {}
LorawanReceiver::~LorawanReceiver() = default;

void LorawanReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void LorawanReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void LorawanReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->centerSet) { p_->centerSet = hz; p_->resetReq = true; } }
bool LorawanReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= lorawanTuning().minSampleRate - 1; }
void LorawanReceiver::reset() { p_->resetReq = true; }
void LorawanReceiver::setSettings(const LorawanSettings& s) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfgSet = sanitize(s); p_->dirty = true; }
LorawanSettings LorawanReceiver::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfgSet; }
void LorawanReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        LorawanTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resClear = true; p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void LorawanReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.dirty.load()) m.rebuild();
    if (m.rate <= 0 || n == 0) return;
    for (size_t i = 0; i < n; i++) { const float re = x[i].real(), im = x[i].imag(); if (std::isfinite(re) && std::isfinite(im)) m.power += (double)re * re + (double)im * im; }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.rate >= lorawanTuning().minSampleRate - 1) {
        m.bank.feed(x, n, m.got);
        if (m.fskOn) m.fskRx.feed(x, n, m.fskGot);
        m.processBlockFrames();
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.rate;
        m.report();
    }
}

bool LorawanReceiver::telemetry(LorawanTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

std::string LorawanReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log, p_->pub.devices);
}
bool LorawanReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    LorawanTelemetry l;
    if (!results::unpack(blob, t0, l.log, l.devices)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    LorawanTelemetry c;
    c.log = l.log; c.devices = l.devices;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
void LorawanReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning lorawanTuning() {
    ModeTuning t;
    t.stdMode = 202; t.id = "lorawanwide"; t.name = "LoRaWAN (all channels)";
    t.minMhz = 137; t.maxMhz = 2500;
    t.defMhz = 868.3;                // EU868: the three join channels and the five extra ones around it
    t.sampleRate = 4000000;          // 4 Msps: all eight EU868 channels, a quarter of the US915 channels
    t.basebandHz = 3500000;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 100000;         // the radio sits 100 kHz above the dial: its DC spike falls between two channels
    return t;
}

} // namespace dect2
