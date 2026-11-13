// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Extra links receiver: resamples the capture to each PHY's rate, hands frames to the parsers (xlink_proto.cpp), keeps the frame and link lists.
#include "dect2/xlink/xlink_rx.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/resampler.h"
#include "dect2/xlink/ht_phy.h"
#include "dect2/xlink/npr.h"
#include "dect2/xlink/winmor.h"
#include "dect2/xlink/mioty.h"
#include "dect2/xlink/nrplus.h"
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

namespace dect2 {

ModeTuning xlinkTuning() {
    ModeTuning t;
    t.stdMode = 602;
    t.id = "xlink";
    t.name = "Extra links";
    t.minMhz = 400; t.maxMhz = 6000; t.defMhz = 5805;
    t.sampleRate = 20e6;
    t.bandwidthMhz = 20;
    t.minSampleRate = 20e6;
    return t;
}

ModeTuning xlinkNarrowTuning() {
    ModeTuning t;
    t.stdMode = 603;
    t.id = "xlinknb";
    t.name = "Extra links, narrow";
    t.minMhz = 1; t.maxMhz = 6000; t.defMhz = 437;
    t.sampleRate = 4e6;
    t.bandwidthMhz = 2;
    t.minSampleRate = 1e6;
    return t;
}

namespace {
constexpr size_t kFramesMax = 400, kLinksMax = 64;
int64_t nowUtc() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string hexOf(const uint8_t* p, size_t n, size_t maxN) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < maxN; i++) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    if (n > maxN) s += "..";
    return s;
}
} // namespace

struct XlinkReceiver::Impl {
    std::mutex m;
    double fs = 0, offset = 0, centre = 0;
    bool dirty = true;
    int protocols = 1;
    std::function<void(const std::string&)> log;
    XlinkTelemetry tel;
    uint64_t seq = 0;
    int64_t nIn = 0;

    bool canHt = false, resample = false, rateTold = false;
    RationalResampler rsm;
    xlink::ht::HtRx ht;
    struct NprChain {
        double sr = 0;
        RationalResampler rs;
        std::unique_ptr<xlink::npr::SyncFinder> f;
        std::vector<cf32> out;
        std::vector<float> phase;
        cf32 prev{1, 0};
    };
    std::vector<std::unique_ptr<NprChain>> npr;
    RationalResampler wrs;
    bool wrsOn = false, wrsActive = false;
    xlink::winmor::PilotFinder winmor;
    RationalResampler mrs;
    bool mrsOn = false, mActive = false;
    xlink::mioty::Receiver mioty;
    RationalResampler nrs;
    bool nrsOn = false, nActive = false;
    xlink::nrplus::Receiver nrp;
    std::vector<cf32> nout;
    std::vector<cf32> mout;
    std::vector<cf32> wout;
    double phase = 0, dphi = 0;
    std::vector<cf32> buf, out;
    struct Seq { uint64_t lastBlock = 0; int lastFrag = -1; int maxFrag = -1; bool have = false; };
    std::map<std::string, Seq> sq;
    int64_t lastFrameUtc = 0;

    Impl() {
        ht.setCallback([this](const uint8_t* mpdu, size_t n, const xlink::ht::HtInfo& in) { onHt(mpdu, n, in); });
    }

    void buildNpr() {
        npr.clear();
        if (!(protocols & xlink::Npr)) return;
        for (int i = 0; i < 5; i++) {
            const double sr = xlink::npr::kSymbolRates[i];
            if (fs < 4 * sr - 1) continue;
            auto c = std::make_unique<NprChain>();
            c->sr = sr;
            double err = 0;
            if (std::fabs(fs - 4 * sr) > 1 && !c->rs.configure(fs, 4 * sr, &err, 1.3 * sr)) continue;
            c->f = std::make_unique<xlink::npr::SyncFinder>(i);
            c->f->setCallback([this, sr](const xlink::npr::Hit& h) {
                char id[32], det[96];
                snprintf(id, sizeof id, "network %d", h.network);
                snprintf(det, sizeof det, "%.0f ksym/s 4GFSK, sync word 45 D2 %02X, payload whitened", sr / 1e3, (unsigned)(xlink::npr::syncWord(h.network) & 0xFF));
                addFrame(xlink::Npr, id, "sync", det, "", h.offsetHz, 0.f);
            });
            npr.push_back(std::move(c));
        }
    }

    void buildWinmor() {
        wrsActive = false;
        if (!(protocols & xlink::Winmor)) return;
        wrsOn = std::fabs(fs - xlink::winmor::kRate) > 1;
        double err = 0;
        if (wrsOn && !wrs.configure(fs, xlink::winmor::kRate, &err, 8000)) return;
        winmor.reset();
        winmor.setCallback([this](const xlink::winmor::Pilot& p) {
            char det[96];
            snprintf(det, sizeof det, "pilot %d symbols, tones %d %d %d %d (raw)", p.symbols, p.tones[0], p.tones[1], p.tones[2], p.tones[3]);
            addFrame(xlink::Winmor, "WINMOR", "pilot", det, "", p.offsetHz, 0.f);
        });
        wrsActive = true;
    }

    void buildMioty() {
        mActive = false;
        if (!(protocols & xlink::Mioty)) return;
        mrsOn = std::fabs(fs - xlink::mioty::kRate) > 1;
        double err = 0;
        if (mrsOn && !mrs.configure(fs, xlink::mioty::kRate, &err, 110e3)) return;
        mioty.reset();
        mioty.setCallback([this](const xlink::mioty::Frame& f) {
            char id[48], typ[24];
            const bool full = !f.mpdu.empty() && (f.mpdu[0] >> 5 & 1);
            std::string a;
            for (size_t k = 1; k < f.mpdu.size() && k < (full ? 9u : 3u); k++) { char b[4]; snprintf(b, sizeof b, "%02X", f.mpdu[k]); a += b; }
            snprintf(id, sizeof id, "%s %s", full ? "EUI-64" : "short", a.c_str());
            snprintf(typ, sizeof typ, "uplink %s", xlink::mioty::patternName(f.pattern));
            const std::string det = xlink::mioty::describeMpdu(f.mpdu);
            addFrame(xlink::Mioty, id, typ, det, hexOf(f.mpdu.data(), f.mpdu.size(), 32), f.freqHz + 12 * xlink::mioty::kCarrierStep, (float)f.snrDb);
        });
        mActive = true;
    }

    void buildNrPlus() {
        nActive = false;
        if (!(protocols & xlink::NrPlus)) return;
        if (fs < xlink::nrplus::kRate - 1) { if (log) log("DECT NR+: a 1.728 MHz channel needs a sample rate of at least 1.728 Msps"); return; }
        nrsOn = std::fabs(fs - xlink::nrplus::kRate) > 1;
        double err = 0;
        if (nrsOn && !nrs.configure(fs, xlink::nrplus::kRate, &err, 1.6e6)) return;
        nrp.reset();
        nrp.setCallback([this](const xlink::nrplus::Header& h) {
            char id[48], typ[32];
            snprintf(id, sizeof id, "network %02X, RD %04X", (unsigned)h.networkId, (unsigned)h.txId);
            snprintf(typ, sizeof typ, "control field type %d", h.type);
            addFrame(xlink::NrPlus, id, typ, xlink::nrplus::describe(h), "", h.cfoHz, h.snrDb);
        });
        nActive = true;
    }

    void rebuild() {
        dirty = false;
        buildNrPlus();
        buildNpr();
        buildWinmor();
        buildMioty();
        canHt = false;
        resample = false;
        phase = 0;
        dphi = -2 * math::kPi * offset / fs;
        ht.reset();
        if (fs < 20e6 - 1) {
            if (!rateTold && log) { log("wfb-ng / OpenHD: a 20 MHz channel needs a sample rate of at least 20 Msps"); rateTold = true; }
            tel.rateTooLow = true;
            return;
        }
        tel.rateTooLow = false;
        if (std::fabs(fs - 20e6) > 1) {
            double err = 0;
            if (!rsm.configure(fs, 20e6, &err)) return;
            resample = true;
        }
        canHt = true;
        sq.clear();
    }

    void addFrame(int proto, const std::string& id, const std::string& type, const std::string& detail, const std::string& hex, double cfo, float snr) {
        XlinkFrame f;
        f.utc = nowUtc();
        f.timeSec = (double)nIn / std::max(1.0, fs);
        f.proto = proto;
        f.id = id; f.type = type; f.detail = detail; f.hex = hex;
        f.freqHz = centre != 0 ? centre + offset + cfo : cfo;
        f.snrDb = snr;
        std::lock_guard<std::mutex> g(m);
        tel.frames.insert(tel.frames.begin(), std::move(f));
        if (tel.frames.size() > kFramesMax) tel.frames.pop_back();
        XlinkLink* l = nullptr;
        for (auto& x : tel.links) if (x.proto == proto && x.id == id) { l = &x; break; }
        if (!l) {
            tel.links.insert(tel.links.begin(), XlinkLink{});
            l = &tel.links.front();
            l->proto = proto; l->id = id; l->firstSeen = tel.frames.front().utc;
            if (tel.links.size() > kLinksMax) tel.links.pop_back();
        }
        l->count++;
        l->lastType = type; l->lastDetail = detail;
        l->lastSeen = tel.frames.front().utc;
        l->lastSnr = snr;
        tel.blocksOk++;
        tel.state = 2;
        tel.dataValid = true;
        tel.cfoHz = cfo;
        tel.snrDb = snr;
        tel.seq = ++seq;
    }

    void addLost(int proto, const std::string& id, uint64_t n) {
        std::lock_guard<std::mutex> g(m);
        for (auto& x : tel.links) if (x.proto == proto && x.id == id) { x.lost += n; break; }
    }

    void onHt(const uint8_t* mpdu, size_t n, const xlink::ht::HtInfo& in) {
        { std::lock_guard<std::mutex> g(m); tel.phyFrames++; if (tel.state < 1) tel.state = 1; tel.seq = ++seq; }
        const xlink::WfbFrame f = xlink::parseWfb(mpdu, n);
        if (!f.ok || !(protocols & 1)) return;
        char id[64], type[48], det[128];
        const std::string hex = hexOf(mpdu + 24, n >= 24 ? n - 24 : 0, 32);
        const char* gi = in.shortGi ? "short GI" : "long GI";
        if (f.proto == xlink::WfbNg) {
            snprintf(id, sizeof id, "link %06X port %d", (unsigned)f.linkId, f.port);
            if (f.session) {
                snprintf(type, sizeof type, "session");
                snprintf(det, sizeof det, "MCS %d, %s, %zu bytes, encrypted session key", in.mcs, gi, f.payload);
                addFrame(xlink::WfbNg, id, type, det, hex, in.cfoHz, in.snrDb);
                return;
            }
            Seq& s = sq[id];
            uint64_t lost = 0;
            if (s.have) {
                if (f.block == s.lastBlock && f.fragment > s.lastFrag) lost = (uint64_t)(f.fragment - s.lastFrag - 1);
                else if (f.block > s.lastBlock && f.block - s.lastBlock < 1000000 && s.maxFrag >= 0) {
                    const int width = s.maxFrag + 1;
                    lost = (uint64_t)(width - 1 - s.lastFrag) + (f.block - s.lastBlock - 1) * (uint64_t)width + (uint64_t)f.fragment;
                }
            }
            s.have = true; s.lastBlock = f.block; s.lastFrag = f.fragment; s.maxFrag = std::max(s.maxFrag, f.fragment);
            snprintf(type, sizeof type, "data");
            snprintf(det, sizeof det, "MCS %d, %s, block %llu fragment %d, %zu bytes", in.mcs, gi, (unsigned long long)f.block, f.fragment, f.payload);
            addFrame(xlink::WfbNg, id, type, det, hex, in.cfoHz, in.snrDb);
            if (lost) addLost(xlink::WfbNg, id, lost);
        } else {
            snprintf(id, sizeof id, "%s port %d", f.openHdPeer == 1 ? "air" : "ground", f.port);
            snprintf(type, sizeof type, "frame");
            snprintf(det, sizeof det, "MCS %d, %s, nonce %llu, %zu bytes", in.mcs, gi, (unsigned long long)f.nonce, f.payload);
            addFrame(xlink::OpenHd, id, type, det, hex, in.cfoHz, in.snrDb);
        }
    }
};

XlinkReceiver::XlinkReceiver() : p_(std::make_unique<Impl>()) {}
XlinkReceiver::~XlinkReceiver() = default;

void XlinkReceiver::configure(double rate) { std::lock_guard<std::mutex> g(p_->m); p_->fs = rate; p_->dirty = true; p_->tel.inputRate = rate; }
bool XlinkReceiver::ready() const { std::lock_guard<std::mutex> g(p_->m); return p_->fs >= 20e6 - 1; }
void XlinkReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> g(p_->m); if (p_->offset != hz) { p_->offset = hz; p_->dirty = true; } }
void XlinkReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> g(p_->m); p_->centre = hz; }
void XlinkReceiver::reset() { std::lock_guard<std::mutex> g(p_->m); p_->dirty = true; }
void XlinkReceiver::clearResults() {
    std::lock_guard<std::mutex> g(p_->m);
    p_->tel.frames.clear(); p_->tel.links.clear(); p_->tel.blocksOk = p_->tel.blocksBad = p_->tel.phyFrames = p_->tel.phyUnsupported = 0;
    p_->tel.state = 0; p_->tel.dataValid = false;
    p_->tel.seq = ++p_->seq;
}
void XlinkReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> g(p_->m); p_->log = std::move(cb); }
void XlinkReceiver::setProtocols(int mask) { std::lock_guard<std::mutex> g(p_->m); if (p_->protocols != mask) { p_->protocols = mask; p_->dirty = true; } p_->tel.protocols = mask; }

void XlinkReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    bool rebuild;
    { std::lock_guard<std::mutex> g(s.m); rebuild = s.dirty && s.fs > 0; if (rebuild) s.rebuild(); }
    if (s.fs <= 0) return;
    s.nIn += (int64_t)n;
    const cf32* in = x;
    if (s.offset != 0) {
        s.buf.resize(n);
        double ph = s.phase;
        for (size_t i = 0; i < n; i++) { s.buf[i] = x[i] * cf32((float)std::cos(ph), (float)std::sin(ph)); ph += s.dphi; }
        s.phase = std::remainder(ph, 2 * math::kPi);
        in = s.buf.data();
    }
    for (auto& cp : s.npr) {
        Impl::NprChain& c = *cp;
        const cf32* y = in;
        size_t m = n;
        if (std::fabs(s.fs - 4 * c.sr) > 1) { c.out.clear(); c.rs.process(in, n, c.out); y = c.out.data(); m = c.out.size(); }
        c.phase.resize(m);
        for (size_t i = 0; i < m; i++) { c.phase[i] = (float)std::arg(y[i] * std::conj(c.prev)); c.prev = y[i]; }
        c.f->feed(c.phase.data(), m);
    }
    if (s.nActive) {
        const cf32* y = in; size_t m = n;
        if (s.nrsOn) { s.nout.clear(); s.nrs.process(in, n, s.nout); y = s.nout.data(); m = s.nout.size(); }
        s.nrp.feed(y, m);
    }
    if (s.mActive) {
        const cf32* y = in; size_t m = n;
        if (s.mrsOn) { s.mout.clear(); s.mrs.process(in, n, s.mout); y = s.mout.data(); m = s.mout.size(); }
        s.mioty.feed(y, m);
    }
    if (s.wrsActive) {
        const cf32* y = in; size_t m = n;
        if (s.wrsOn) { s.wout.clear(); s.wrs.process(in, n, s.wout); y = s.wout.data(); m = s.wout.size(); }
        s.winmor.feed(y, m);
    }
    if (!(s.protocols & 1) || !s.canHt) { std::lock_guard<std::mutex> g(s.m); s.tel.timeSec = (double)s.nIn / s.fs; s.tel.seq = ++s.seq; return; }
    if (s.resample) { s.out.clear(); s.rsm.process(in, n, s.out); s.ht.feed(s.out.data(), s.out.size()); }
    else s.ht.feed(in, n);
    const auto st = s.ht.stats();
    std::lock_guard<std::mutex> g(s.m);
    s.tel.phyFrames = st.framesOk;
    s.tel.phyUnsupported = st.unsupported;
    s.tel.blocksBad = st.framesBad;
    if (st.triggers && s.tel.state < 1) s.tel.state = 1;
    s.tel.timeSec = (double)s.nIn / s.fs;
    s.tel.seq = ++s.seq;
}

void XlinkReceiver::flush() { p_->mioty.flush(); }

bool XlinkReceiver::telemetry(XlinkTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> g(p_->m);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}

} // namespace dect2
