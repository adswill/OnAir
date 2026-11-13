// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) monitor (see loraraw_rx.h): channel filters and demodulators of mesh_lora.h, one per bandwidth and per spreading factor / sync word searched.
#include "dect2/net/feeds.h"
#include "dect2/automation/engine.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/loraraw/loraraw_tel.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
#include "dect2/common/str.h"

namespace dect2 {

namespace {
constexpr size_t kBlock = 4096;

LoraRawSettings sanitize(LoraRawSettings s) {
    s.sf = std::max(5, std::min(12, s.sf));
    s.bwKhz = s.bwKhz <= 62 ? 62 : s.bwKhz <= 125 ? 125 : s.bwKhz <= 250 ? 250 : 500;
    s.cr = std::max(5, std::min(8, s.cr));
    s.length = std::max(1, std::min(255, s.length));
    s.sync = s.sync < 0 ? -1 : std::min(255, s.sync);
    s.preamble = std::max(6, std::min(65, s.preamble));
    s.ldro = std::max(-1, std::min(1, s.ldro));
    s.offsetHz = std::max(-1e6, std::min(1e6, s.offsetHz));
    return s;
}

std::string hexOf(const std::vector<uint8_t>& p) { return dect2::str::toHex(p.data(), p.size(), true, ' ', 255); }
std::string textOf(const std::vector<uint8_t>& p) {
    std::string s;
    for (size_t i = 0; i < p.size() && i < 255; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    return s;
}
int bwIndex(double bw) { return bw < 190e3 ? 0 : bw < 400e3 ? 1 : 2; }
} // namespace

struct LoraRawReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0;
    LoraRawSettings cfgSet;
    std::atomic<bool> dirty{true}, resetReq{true}, clearReq{false};
    std::function<void(const std::string&)> log;
    LoraRawTelemetry pub;
    bool resClear = false, resLoad = false;
    LoraRawTelemetry loaded;
    double loadedAt = 0;

    // feed thread
    struct Dec {
        std::unique_ptr<lora::Demod> dm;
        std::vector<size_t> infos;         // telemetry rows, one per sync word (one preamble search serves all of them)
        std::vector<int> syncs;
        lora::DemodStats seen;
    };
    struct Chan {
        std::unique_ptr<lora::Channelizer> ch;     // one channel filter per bandwidth ...
        size_t fdIndex = 0;                         // ... or the slot in the shared frequency-domain channeliser
        double outRate = 0;
        int decim = 1;
        double delay = 0;
        lora::ChanBuf buf;
        std::shared_ptr<lora::ChipStream> cs;     // shared by the spreading factors of the channel
        std::vector<Dec> decs;
        double offHz = 0;                  // from the tuned frequency
        int64_t base = 0;
    };
    std::vector<Chan> chans;
    std::unique_ptr<lora::FdChannelizer> fd;       // from 3 bandwidths on: one FFT of the capture serves them all
    std::vector<lora::ChanBuf*> fdOut;
    std::vector<cf32> pend;
    int64_t nIn = 0, nProc = 0;
    double nextReport = 0, rate = 0, offset = 0;
    LoraRawSettings cfg;
    LoraRawTelemetry tel;
    std::vector<lora::RxFrame> frames;
    double lastGood = -1e9;
    double power = 0; int64_t nPower = 0;
    struct Recent { std::vector<uint8_t> payload; double sec; size_t chan, dec; };
    std::vector<Recent> recent;

    double sigSec() const { return rate > 0 ? (double)nProc / rate : 0; }

    void logf(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    static void shiftTimes(LoraRawTelemetry& t, double dt) { for (auto& f : t.log) f.sec += dt; }
    static void moveLists(LoraRawTelemetry& to, LoraRawTelemetry& from) { to.log = std::move(from.log); from.log.clear(); }

    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { LoraRawTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
    }

    void rebuild() {
        {
            std::lock_guard<std::mutex> lk(mu);
            rate = rateSet; offset = offsetSet; cfg = sanitize(cfgSet);
            dirty = false;
        }
        chans.clear();
        recent.clear();
        tel.decoders.clear();
        tel.inputRate = rate; tel.cfg = cfg;
        if (rate <= 0) return;
        std::vector<double> bws;
        if (cfg.autoBw) bws = {125e3, 250e3, 500e3};
        else bws = {cfg.bwKhz == 62 ? 62500.0 : cfg.bwKhz * 1e3};
        std::vector<int> sfs;
        if (cfg.autoSf) for (int s = 7; s <= 12; s++) sfs.push_back(s);
        else sfs = {cfg.sf};
        std::vector<int> syncs;
        if (cfg.sync < 0) syncs = {0x12, 0x34, 0x2B};
        else syncs = {cfg.sync};
        const double inOff = cfg.offsetHz + offset;       // where the channel sits in the input
        std::string listed;
        fd.reset(); fdOut.clear();
        {
            std::vector<lora::FdChannelizer::Spec> specs;
            for (double bw : bws) if (std::fabs(inOff) + 0.5 * bw + 25e3 < 0.45 * rate && rate >= 2.2 * bw) specs.push_back({inOff, bw});
            if (lora::FdChannelizer::kEnabled && specs.size() >= 3) { fd = std::make_unique<lora::FdChannelizer>(rate, specs); fdOut.assign(specs.size(), nullptr); }
        }
        for (double bw : bws) {
            const bool inBand = std::fabs(inOff) + 0.5 * bw + 25e3 < 0.45 * rate && rate >= 2.2 * bw;
            Chan* c = nullptr;
            for (int sf : sfs) {
                Dec dc;
                for (int sw : syncs) {
                    LrDecoderInfo d;
                    d.sf = sf; d.bwHz = bw; d.sync = sw; d.inBand = inBand;
                    tel.decoders.push_back(d);
                    dc.infos.push_back(tel.decoders.size() - 1);
                    dc.syncs.push_back(sw);
                }
                if (!inBand) continue;
                if (!c) {
                    chans.emplace_back();
                    c = &chans.back();
                    if (fd) { c->fdIndex = chans.size() - 1; c->outRate = fd->outRate(c->fdIndex); c->decim = fd->decim(c->fdIndex); c->delay = 0; }
                    else {
                        c->ch = std::make_unique<lora::Channelizer>(rate, inOff, bw);
                        c->outRate = c->ch->outRate(); c->decim = c->ch->decim(); c->delay = c->ch->delayIn();
                    }
                    c->offHz = cfg.offsetHz;
                    c->base = nProc;
                    c->buf.clear(0);
                    c->cs = std::make_shared<lora::ChipStream>(c->outRate / bw);
                }
                lora::Params p;
                p.sf = sf; p.bwHz = bw; p.syncWord = (uint8_t)syncs[0]; p.preamble = cfg.preamble;
                for (size_t si = 1; si < syncs.size(); si++) p.moreSync.push_back((uint8_t)syncs[si]);
                p.ldro = cfg.ldro < 0 ? lora::autoLdro(sf, bw) : cfg.ldro == 1;
                if (cfg.implicit) { p.implicitHdr = true; p.implicitLen = cfg.length; p.cr = cfg.cr; p.crc = cfg.crc; }
                dc.dm = std::make_unique<lora::Demod>(p, c->outRate, 1, c->cs);
                dc.dm->reset(0);
                c->decs.push_back(std::move(dc));
            }
            char b[64];
            snprintf(b, sizeof b, "%s%.0f kHz%s", listed.empty() ? "" : ", ", bw / 1e3, inBand ? "" : " (outside the captured band)");
            listed += b;
        }
        logf("LoRa (raw): searching " + std::string(cfg.autoSf ? "SF7 - SF12" : "SF" + std::to_string(cfg.sf)) + ", " + listed + (cfg.implicit ? ", implicit header" : ", explicit header"));
    }

    void onFrame(size_t ci, size_t di, Chan& c, Dec& d, const lora::RxFrame& f) {
        const double t = ((double)c.base + f.startSample * c.decim - c.delay) / rate;
        const bool good = f.crcOk || !f.hdr.crc;
        for (const auto& r : recent) if ((r.chan != ci || r.dec != di) && r.payload == f.payload && std::fabs(r.sec - t) < 0.25) return;   // another setting locked the same frame
        recent.push_back({f.payload, t, ci, di});
        if (recent.size() > 12) recent.erase(recent.begin());
        const lora::Params& p = d.dm->params();
        size_t row = 0;
        for (size_t si = 0; si < d.syncs.size(); si++) if (d.syncs[si] == f.syncWord) row = si;
        LrDecoderInfo& info = tel.decoders[d.infos[row]];
        info.frames++;
        if (!good) info.crcBad++;
        LrFrame r;
        r.wallTime = (int64_t)std::time(nullptr);
        r.sec = t; r.sf = p.sf; r.bwHz = p.bwHz; r.cr = f.hdr.cr; r.hasCrc = f.hdr.crc; r.crcOk = f.crcOk; r.implicit = p.implicitHdr;
        r.sync = d.syncs[row]; r.len = (int)f.payload.size(); r.corrected = f.corrected;
        r.snrDb = (float)f.snrDb; r.levelDb = (float)f.levelDb; r.cfoHz = f.cfoHz; r.freqHz = c.offHz + f.cfoHz;
        r.payload = f.payload; r.hex = hexOf(f.payload); r.ascii = textOf(f.payload);
        if (good) {
            tel.blocksOk++; tel.dataValid = true; lastGood = sigSec();
            if (Feeds::get().wantLora()) Feeds::get().lora("lora", r.hex, r.snrDb, r.freqHz, r.sf);
            if (automation::wants(automation::FLora)) {   // the automations (automation/engine.h)
                automation::Event ev("lora.packet");
                ev.at(automation::wallNow(), "LoRa", r.freqHz);
                ev.str("kind", "lora").str("hex", r.hex).num("snr_db", r.snrDb).num("sf", r.sf);
                automation::emit(std::move(ev));
            }
            tel.cfoHz = f.cfoHz; tel.snrDb = (float)f.snrDb;
            if (p.sf >= 5 && p.sf <= 12) tel.perSf[p.sf]++;
            tel.perBw[bwIndex(p.bwHz)]++;
        } else tel.blocksBad++;
        tel.log.insert(tel.log.begin(), std::move(r));
        if (tel.log.size() > kLrLogMax) tel.log.pop_back();
    }

    void processBlock(const cf32* x, size_t n) {
        if (fd) { for (Chan& c : chans) fdOut[c.fdIndex] = &c.buf; fd->process(x, n, fdOut); }
        for (size_t ci = 0; ci < chans.size(); ci++) {
            Chan& c = chans[ci];
            if (!fd) c.ch->process(x, n, c.buf);
            c.cs->extend(c.buf);
            int64_t keep = c.buf.end();
            for (size_t di = 0; di < c.decs.size(); di++) {
                Dec& d = c.decs[di];
                frames.clear();
                d.dm->process(c.buf, frames);
                for (auto& f : frames) onFrame(ci, di, c, d, f);
                const auto& st = d.dm->stats();
                tel.preambles += st.preambles - d.seen.preambles;
                tel.headerBad += st.headerBad - d.seen.headerBad;
                for (size_t ii : d.infos) {
                    LrDecoderInfo& info = tel.decoders[ii];
                    info.preambles = st.preambles; info.headerBad = st.headerBad; info.syncBad = st.syncBad;
                }
                d.seen = st;
                keep = std::min(keep, d.dm->oldestNeeded());
            }
            c.buf.trim(keep);
            c.cs->trimToSample(keep);
        }
        nProc += (int64_t)n;
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        chans.clear(); pend.clear(); recent.clear();
        nIn = 0; nProc = 0; nextReport = 0.25 * rate;
        shiftTimes(tel, -tel.timeSec);
        tel.timeSec = 0; lastGood = -1e9;
        power = 0; nPower = 0;
        dirty = true;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        const auto dec = tel.decoders;
        tel = LoraRawTelemetry(); tel.seq = seq; tel.inputRate = rate; tel.cfg = cfg; tel.decoders = dec;
        for (auto& d : tel.decoders) { d.frames = d.crcBad = d.preambles = d.headerBad = d.syncBad = 0; }
        for (auto& c : chans) for (auto& d : c.decs) d.seen = lora::DemodStats();
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

LoraRawReceiver::LoraRawReceiver() : p_(std::make_unique<Impl>()) {}
LoraRawReceiver::~LoraRawReceiver() = default;

void LoraRawReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void LoraRawReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
bool LoraRawReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= loraRawTuning().minSampleRate - 1; }
void LoraRawReceiver::reset() { p_->resetReq = true; }
void LoraRawReceiver::setSettings(const LoraRawSettings& s) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfgSet = sanitize(s); p_->dirty = true; }
LoraRawSettings LoraRawReceiver::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfgSet; }
void LoraRawReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        LoraRawTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resClear = true; p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void LoraRawReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.dirty.load()) m.rebuild();
    if (m.rate <= 0 || n == 0) return;
    for (size_t i = 0; i < n; i++) { const float re = x[i].real(), im = x[i].imag(); if (std::isfinite(re) && std::isfinite(im)) m.power += (double)re * re + (double)im * im; }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.chans.empty() || m.rate < loraRawTuning().minSampleRate - 1) m.nProc += (int64_t)n;
    else {
        size_t i = 0;
        if (!m.pend.empty()) {
            const size_t take = std::min(n, kBlock - m.pend.size());
            m.pend.insert(m.pend.end(), x, x + take);
            i = take;
            if (m.pend.size() == kBlock) { m.processBlock(m.pend.data(), kBlock); m.pend.clear(); }
        }
        for (; i + kBlock <= n; i += kBlock) m.processBlock(x + i, kBlock);
        if (i < n) m.pend.insert(m.pend.end(), x + i, x + n);
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.rate;
        m.report();
    }
}

bool LoraRawReceiver::telemetry(LoraRawTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

std::string LoraRawReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.log);
}
bool LoraRawReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    LoraRawTelemetry l;
    if (!results::unpack(blob, t0, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    LoraRawTelemetry c;
    c.log = l.log;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
void LoraRawReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning loraRawTuning() {
    ModeTuning t;
    t.stdMode = 73; t.id = "loraraw"; t.name = "LoRa (raw)";
    t.minMhz = 137; t.maxMhz = 2500;
    t.defMhz = 868.1;                // EU868 channel 1
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 300000;         // the radio sits 300 kHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2
