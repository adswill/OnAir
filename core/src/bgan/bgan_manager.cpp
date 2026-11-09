// BGAN mode manager (bgan_manager.h).
#include "dect2/bgan/bgan_manager.h"
#include "dect2/bgan/bgan_demod.h"
#include "dect2/bgan/bgan_phy.h"
#include "dect2/inmsat/inmsat_detect.h"
#include "dect2/mesh/mesh_lora.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace dect2::bgan {

namespace {
const double kPi = 3.141592653589793;

// the symbol rates a carrier of this occupied width can be, widest evidence first; 0 = not a BGAN candidate
std::vector<int> ratesForWidth(double w) {
    if (w > 6000 && w < 16000) return { 8400 };
    if (w >= 24000 && w < 62000) return { 33600 };
    if (w >= 62000 && w < 130000) return { 84000 };
    if (w >= 130000 && w <= 225000) return { 168000, 151200 };
    return {};
}

std::string plmnText(const Plmn& p) { char b[16]; snprintf(b, sizeof b, "%03d-%0*d", p.mcc, p.mncDigits, p.mnc); return b; }

void fft(std::vector<cf32>& a) {
    const int n = int(a.size());
    for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        const cf32 w(float(std::cos(ang)), float(std::sin(ang)));
        for (int i = 0; i < n; i += len) { cf32 c(1, 0); for (int j = 0; j < len / 2; j++) { const cf32 u = a[i + j], v = a[i + j + len / 2] * c; a[i + j] = u + v; a[i + j + len / 2] = u - v; c *= w; } }
    }
}

// the lines of filtered OQPSK in the squared signal at +-Rs/2 around twice the carrier: GMR-2 (135.4167 ksym/s)
bool gmr2Lines(const std::vector<cf32>& x, double fs) {
    int n = 1 << 13;
    while (n > int(x.size()) && n > 256) n >>= 1;
    if (n < 2048 || fs < 2.1 * 135416.7) return false;
    std::vector<cf32> s(n);
    for (int i = 0; i < n; i++) { const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1)); s[i] = x[size_t(x.size()) - n + i] * x[size_t(x.size()) - n + i] * float(w); }
    fft(s);
    std::vector<float> m(n);
    std::vector<float> tmp(n);
    for (int i = 0; i < n; i++) m[i] = std::norm(s[i]);
    tmp = m;
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    const float med = tmp[n / 2] + 1e-20f;
    const int sep = int(std::lround(135416.67 / (fs / n)));
    const int tol = std::max(2, int(600.0 / (fs / n)));
    float best = 0; int bestI = -1;
    for (int i = 0; i < n; i++) if (m[i] > best) { best = m[i]; bestI = i; }
    if (best < 18 * med || bestI < 0) return false;
    for (int d : { sep, -sep }) {
        for (int e = -tol; e <= tol; e++) {
            const int j = (bestI + d + e + n) % n;
            if (m[j] > 8 * med) return true;
        }
    }
    return false;
}

struct Chan {
    int id = 0;
    double offsetHz = 0, widthHz = 0;
    float snrDb = 0;
    std::vector<int> rates;
    int rateIdx = 0;
    double rateSince = 0, lastCarrier = 0;
    std::unique_ptr<BearerDemod> demod;
    lora::ChanBuf buf;
    double outRate = 0;
    std::vector<cf32> tail;                  // for the GMR-2 check
    int gmrCheckAge = 0, gmrVotes = 0;
    bool gmr = false;
    int64_t cursor = 0;
    bool any = false;
    double firstSeen = 0;
    // merged results, by absolute frequency key, survive retunes
    CarrierSig sig;
    BganCarrierRow row;
};

} // namespace

struct Manager::Impl {
    ManagerConfig cfg;
    inmsat::CarrierDetector det;
    bool detReady = false;
    std::vector<std::unique_ptr<Chan>> chans;
    std::unique_ptr<lora::FdChannelizer> chz;
    uint64_t lastSearches = 0;
    double nextRebuild = 0;
    bool needRebuild = false;
    std::function<void(const std::string&)> logCb;
    std::mutex mu;
    BganTelemetry snap;
    std::vector<std::string> log;                  // newest first, up to 200
    std::vector<BganCarrierRow> archive;           // rows from earlier retunes (the scan)
    std::map<int, BganBeam> beams;                 // merged primaries
    uint64_t seq = 0, blocksOk = 0, blocksBad = 0;
    double lastPublish = -1;
    int nextId = 1;
    bool scanning = false; int scanStep = 0, scanSteps = 0;

    void say(const std::string& s) { if (logCb) logCb(s); }
    double t() { return det.nowSec(); }

    void configure(const ManagerConfig& c) {
        cfg = c;
        inmsat::DetectorConfig d;
        d.rate = c.rate;
        d.binHz = 500;
        d.searchSec = 1.0;
        d.slices = 32;
        d.thresholdDb = 3.0;
        d.holdSec = 8.0;
        d.dcExcludeHz = 3000;
        d.edgeFraction = 0.48;
        d.maxWidthHz = 260000;
        det.configure(d);
        chans.clear(); chz.reset();
        lastSearches = 0;
        detReady = c.rate >= 300000;
    }

    void rebuildChannelizer() {
        std::vector<lora::FdChannelizer::Spec> specs;
        for (auto& c : chans) specs.push_back({ c->offsetHz, std::max(c->widthHz * 1.25, 6000.0) });
        if (specs.empty()) { chz.reset(); return; }
        chz = std::make_unique<lora::FdChannelizer>(cfg.rate, specs);
        for (size_t i = 0; i < chans.size(); i++) {
            chans[i]->outRate = chz->outRate(i);
            chans[i]->buf.clear(0);
            chans[i]->cursor = 0;
            // a new channeliser: samples restart, but the demodulator keeps its counts and tables (acquisition restarts)
            if (!chans[i]->demod) chans[i]->demod = std::make_unique<BearerDemod>();
            const int r = chans[i]->rates[chans[i]->rateIdx];
            if (!chans[i]->demod->configure(chans[i]->outRate, r)) { }
        }
    }

    Chan* find(int id) { for (auto& c : chans) if (c->id == id) return c.get(); return nullptr; }

    void plan() {
        const auto& cars = det.carriers();
        const double now = det.nowSec();
        bool changed = false;
        // keep channels whose carrier is still there
        for (auto& c : chans) {
            for (const auto& car : cars) {
                if (car.id == c->id && car.confirmed) { c->offsetHz += 0.02 * (car.offsetHz - c->offsetHz); c->widthHz = car.widthHz; c->snrDb = car.snrDb; c->lastCarrier = now; }
            }
        }
        for (size_t i = 0; i < chans.size();) {
            if (now - chans[i]->lastCarrier > 20) { archiveRow(*chans[i]); chans.erase(chans.begin() + long(i)); changed = true; }
            else i++;
        }
        for (const auto& car : cars) {
            if (!car.confirmed) continue;
            if (int(chans.size()) >= cfg.maxCarriers) break;
            bool have = false;
            for (auto& c : chans) if (std::fabs(c->offsetHz - car.offsetHz) < 0.5 * car.widthHz + 2000) have = true;
            if (have) continue;
            std::vector<int> rates = ratesForWidth(car.widthHz);
            if (rates.empty()) continue;
            auto c = std::make_unique<Chan>();
            c->id = car.id; c->offsetHz = car.offsetHz; c->widthHz = car.widthHz; c->snrDb = car.snrDb; c->rates = rates; c->rateSince = now; c->lastCarrier = now; c->firstSeen = now;
            chans.push_back(std::move(c));
            changed = true;
        }
        if (changed) needRebuild = true;
        if (needRebuild && now >= nextRebuild) { rebuildChannelizer(); nextRebuild = now + 3; needRebuild = false; }
        // a wide carrier that has not locked on one rate: try the other one
        for (auto& c : chans) {
            if (!c->demod || c->rates.size() < 2) continue;
            const DemodStatus& st = c->demod->status();
            if (st.state != 0) continue;
            if (now - c->rateSince > 4) {
                c->rateIdx = (c->rateIdx + 1) % int(c->rates.size());
                c->rateSince = now;
                c->demod->configure(c->outRate, c->rates[c->rateIdx]);
            }
        }
    }

    void archiveRow(Chan& c) {
        BganCarrierRow r = rowFor(c);
        for (auto& a : archive) if (std::fabs(a.freqHz - r.freqHz) < 3000 && r.freqHz != 0) { if (r.state >= a.state) a = r; return; }
        archive.push_back(r);
        if (archive.size() > 300) archive.erase(archive.begin());
    }

    BganCarrierRow rowFor(Chan& c) {
        BganCarrierRow r;
        r.id = c.id;
        r.offsetHz = c.offsetHz; r.widthHz = c.widthHz;
        r.freqHz = cfg.centerHz != 0 ? cfg.centerHz + c.offsetHz : 0;
        r.snrDb = c.snrDb;
        r.lastSeenSec = c.lastCarrier;
        if (c.demod) {
            const DemodStatus& st = c.demod->status();
            r.state = st.state;
            if (st.bearer >= 0) { r.bearer = bearer(st.bearer).name; r.symRate = bearer(st.bearer).symRate; }
            if (st.subtype >= 0) r.subtype = subtypeName(st.subtype);
            r.cfoHz = st.cfoHz; r.cn0Dbhz = float(st.cn0Dbhz); r.frames = st.frames; r.blocksOk = st.blocksOk; r.blocksBad = st.blocksBad; r.load = st.occupancy;
            r.structureOnly = st.structureOnly;
            r.kind = (st.state >= 1 && st.bearer >= 0) ? 1 : 0;
            const CarrierSig& s = st.sig;
            r.spotBeam = s.beamSpot >= 0 ? s.beamSpot : s.spotBeam;
            r.beamType = s.beamType; r.rnc = s.rnc; r.bctId = s.bctId; r.fBearer = s.fBearer;
            if (!s.plmns.empty()) r.plmn = plmnText(s.plmns[0]);
            if (!s.sats.empty()) r.satellite = describeSatellite(s.sats[0].lonDeg);
        }
        if (r.kind == 0 && c.gmr) { r.kind = 2; r.bearer = "GMR-2 class (IsatPhone type)"; r.symRate = 135416.67; }
        return r;
    }

    void feed(const cf32* x, size_t n) {
        det.feed(x, n);
        if (det.searches() != lastSearches) { lastSearches = det.searches(); plan(); }
        if (chz) {
            std::vector<lora::ChanBuf*> out;
            out.reserve(chans.size());
            for (auto& c : chans) out.push_back(&c->buf);
            chz->process(x, n, out);
            for (auto& c : chans) {
                const int64_t e = c->buf.end();
                if (c->cursor < c->buf.begin()) c->cursor = c->buf.begin();
                if (e > c->cursor) {
                    const size_t len = size_t(e - c->cursor);
                    if (c->demod) c->demod->feed(c->buf.at(c->cursor), len);
                    // keep a tail for the GMR-2 check of carriers that do not lock
                    if (c->rates.size() >= 2 && c->demod && c->demod->status().state == 0) {
                        c->tail.insert(c->tail.end(), c->buf.at(c->cursor), c->buf.at(c->cursor) + len);
                        if (c->tail.size() > 40000) c->tail.erase(c->tail.begin(), c->tail.end() - 16384);
                    }
                    c->cursor = e;
                    c->buf.trim(e);
                }
                if (c->demod) {
                    std::vector<std::string> l;
                    c->demod->takeLines(l);
                    if (!l.empty()) {
                        for (auto& s : l) {
                            char b[56]; snprintf(b, sizeof b, "%.4f MHz: ", (cfg.centerHz + c->offsetHz) / 1e6);
                            log.insert(log.begin(), (cfg.centerHz != 0 ? std::string(b) : std::string()) + s);
                        }
                        if (log.size() > 200) log.resize(200);
                    }
                }
                if (c->rates.size() >= 2 && c->demod && c->demod->status().state == 0 && c->tail.size() >= 16384 && ++c->gmrCheckAge >= 8) {
                    c->gmrCheckAge = 0;
                    if (gmr2Lines(c->tail, c->outRate)) c->gmrVotes = std::min(c->gmrVotes + 1, 8); else c->gmrVotes = std::max(c->gmrVotes - 1, 0);
                    c->gmr = c->gmrVotes >= 3;
                    c->tail.clear();
                }
            }
        }
        publish();
    }

    void publish() {
        const double now = det.nowSec();
        if (lastPublish >= 0 && now - lastPublish < 0.4) return;
        lastPublish = now;
        BganTelemetry s;
        s.rate = cfg.rate; s.centerHz = cfg.centerHz;
        s.scanning = scanning; s.scanStep = scanStep; s.scanSteps = scanSteps;
        s.floorDb = float(det.floorDb());
        uint64_t ok = 0, bad = 0;
        std::vector<BganCarrierRow> rows = archive;
        for (auto& c : chans) {
            BganCarrierRow r = rowFor(*c);
            bool merged = false;
            for (auto& a : rows) if (r.freqHz != 0 && std::fabs(a.freqHz - r.freqHz) < 3000) { a = r; merged = true; break; }
            if (!merged) rows.push_back(r);
            if (c->demod) {
                const DemodStatus& st = c->demod->status();
                ok += st.blocksOk; bad += st.blocksBad;
                if (st.state == 2) s.state = 2;
                else if (st.state == 1 && s.state < 2) s.state = 1;
                for (const auto& kv : st.sig.primaries) { BganBeam b; b.spot = kv.first; b.mhz = kv.second.mhz; b.bearer = fbearerTypeName(kv.second.fbType); beams[kv.first] = b; }
            }
        }
        if (s.state == 0 && !det.carriers().empty()) s.state = 1;
        s.rows = std::move(rows);
        std::sort(s.rows.begin(), s.rows.end(), [](const BganCarrierRow& a, const BganCarrierRow& b) { if (a.freqHz != b.freqHz) return a.freqHz < b.freqHz; return a.offsetHz < b.offsetHz; });
        for (const auto& r : s.rows) { if (r.kind == 1) s.bearers++; if (r.kind == 2) s.identifiedGmr++; s.carriers++; if (r.cn0Dbhz > s.snrDb) s.snrDb = r.cn0Dbhz; }
        s.dataValid = false;
        std::vector<std::string> sats, nets;
        for (const auto& r : s.rows) {
            if (r.state == 2) s.dataValid = true;
            if (!r.satellite.empty() && std::find(sats.begin(), sats.end(), r.satellite) == sats.end()) sats.push_back(r.satellite);
            if (!r.plmn.empty() && std::find(nets.begin(), nets.end(), r.plmn) == nets.end()) nets.push_back(r.plmn);
        }
        for (size_t i = 0; i < sats.size(); i++) { s.satellites += (i ? ", " : ""); s.satellites += sats[i]; }
        for (size_t i = 0; i < nets.size(); i++) { s.network += (i ? ", " : ""); s.network += nets[i]; }
        for (const auto& kv : beams) s.beams.push_back(kv.second);
        s.log = log;
        s.blocksOk = ok; s.blocksBad = bad;
        for (auto& c : chans) if (c->demod) { s.cfoHz = c->demod->status().cfoHz; break; }
        std::lock_guard<std::mutex> lk(mu);
        s.seq = ++seq;
        snap = std::move(s);
    }
};

Manager::Manager() : p_(std::make_unique<Impl>()) {}
Manager::~Manager() = default;
void Manager::configure(const ManagerConfig& c) { p_->configure(c); }
bool Manager::ready() const { return p_->detReady; }
void Manager::reset() {
    for (auto& c : p_->chans) p_->archiveRow(*c);
    p_->chans.clear(); p_->chz.reset(); p_->det.reset(); p_->lastSearches = 0; p_->lastPublish = -1;
}
void Manager::clearResults() {
    p_->archive.clear(); p_->beams.clear(); p_->log.clear();
    for (auto& c : p_->chans) if (c->demod) c->demod->reset();
}
void Manager::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool Manager::snapshot(BganTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->snap.seq <= lastSeq) return false;
    out = p_->snap;
    return true;
}
void Manager::setCenter(double hz) { p_->cfg.centerHz = hz; }
void Manager::setScanStatus(bool scanning, int step, int steps) { p_->scanning = scanning; p_->scanStep = step; p_->scanSteps = steps; }
void Manager::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }

} // namespace dect2::bgan
