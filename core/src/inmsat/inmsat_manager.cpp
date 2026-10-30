// Manager of the Inmarsat all-carriers mode (inmsat_manager.h).
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/inmsat/inmsat_planes.h"
#include "dect2/aero/aero_demod.h"
#include "dect2/aero/aero_phy.h"
#include "dect2/inmc/inmc_rx.h"
#include "dect2/mesh/mesh_lora.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <mutex>

namespace dect2::inmsat {

const char* carrierTypeName(CarrierType t) {
    switch (t) {
        case CarrierType::AeroP: return "Aero P";
        case CarrierType::InmarsatC: return "Inmarsat-C";
        case CarrierType::AeroC: return "Aero C (voice)";
        default: return "?";
    }
}

namespace {
constexpr double kMargin = 20000;       // a carrier must sit this far inside a window's edge
constexpr double kAeroCost = 1.0, kInmcCost = 0.6;
constexpr double kCCost = 0.25;         // one C-channel demodulator (8400 bit/s, brought down to about 8 samples per bit)
constexpr double kCMaxWidth = 12000;    // a wide carrier up to this width gets a C-channel demodulator (a C channel is about 7 kHz)
constexpr double kWindowHold = 20;      // seconds a window stays without any carrier
}

struct Manager::Impl {
    std::mutex mu, snapMu;
    ManagerConfig cfg;
    CarrierDetector det;
    uint64_t lastSearches = 0;
    int focusId = 0;
    bool haveLoc = false;
    double locLat = 0, locLon = 0;
    std::string satManual;
    std::function<void(const std::string&)> log;
    Manager::VoiceSink voiceSink;

    // an Aero C channel (voice) decoder on one wide carrier of a window: the carrier is mixed to 0 Hz and handed to AeroDemod(8400)
    struct CChan {
        int carrierId = 0;
        double offsetHz = 0;          // against the capture centre
        double mixInc = 0, mixPhase = 0;
        std::unique_ptr<AeroDemod> demod;
        uint64_t frames = 0, susOk = 0, susBad = 0, voiceFrames = 0;
        float ebn0Db = 0;
        double lastGood = -1;         // stream seconds of the last frame whose SUs checked
        std::vector<cf32> tmp;
    };
    struct Window {
        double centerHz = 0, outRate = 0;
        std::unique_ptr<AeroReceiver> aero;
        std::unique_ptr<InmcReceiver> inmc;
        std::vector<std::unique_ptr<CChan>> cchans;
        lora::ChanBuf buf;
        AeroTelemetry at;
        InmcTelemetry it;
        uint64_t lastA = 0, lastI = 0;
        double lastCarrier = 0;       // stream time a carrier was last inside
        double inmcOffset = 0;        // where the Inmarsat-C carrier sits in the window
    };
    std::vector<std::unique_ptr<Window>> wins;
    std::unique_ptr<lora::FdChannelizer> chz;
    std::vector<AeroTelemetry> retiredA;
    std::vector<double> retiredAc;      // the window centre each retired Aero table belonged to (its carrier offsets are against it)
    std::vector<InmcTelemetry> retiredI;
    Snapshot snap;
    double lastPublish = -1;
    uint64_t seq = 0;

    void say(const std::string& s) { if (log) log(s); }

    void configure(const ManagerConfig& c) {
        cfg = c;
        cfg.detector.rate = c.rate;
        det.configure(cfg.detector);
        wins.clear(); chz.reset(); retiredA.clear(); retiredAc.clear(); retiredI.clear();
        lastSearches = 0; lastPublish = -1;
        std::lock_guard<std::mutex> lk(snapMu);
        snap = Snapshot();
        snap.rate = c.rate; snap.centerHz = c.centerHz; snap.cpuBudget = c.budget;
    }
    bool ready() const { return cfg.rate >= 600000; }

    bool covers(const Window& w, double off) const { return std::fabs(off - w.centerHz) <= cfg.windowBwHz / 2 - kMargin; }
    double winCost(const Window& w) const { return kAeroCost + (w.inmc ? kInmcCost : 0) + kCCost * (double)w.cchans.size(); }
    double totalCost() const { double s = 0; for (const auto& w : wins) s += winCost(*w); return s; }

    void retire(Window& w) {
        if (w.aero && (!w.at.messages.empty() || !w.at.aircraft.empty())) { retiredA.push_back(w.at); retiredAc.push_back(w.centerHz); }
        if (w.inmc && !w.it.messages.empty()) retiredI.push_back(w.it);
        if (retiredA.size() > 16) { retiredA.erase(retiredA.begin()); retiredAc.erase(retiredAc.begin()); }
        if (retiredI.size() > 16) retiredI.erase(retiredI.begin());
    }

    void rebuildChannelizer() {
        std::vector<lora::FdChannelizer::Spec> specs;
        for (const auto& w : wins) specs.push_back({w->centerHz, cfg.windowBwHz});
        if (specs.empty()) { chz.reset(); return; }
        chz = std::make_unique<lora::FdChannelizer>(cfg.rate, specs);
        for (size_t i = 0; i < wins.size(); i++) {
            wins[i]->outRate = chz->outRate(i);
            wins[i]->buf.clear(0);
        }
    }

    void plan() {
        const double t = det.nowSec();
        const auto& cars = det.carriers();
        bool changed = false;
        // windows that hold no carrier any more
        for (auto& w : wins) {
            bool has = false;
            for (const auto& c : cars) has |= c.confirmed && covers(*w, c.offsetHz);
            if (has) w->lastCarrier = t;
        }
        for (size_t i = 0; i < wins.size();) {
            if (t - wins[i]->lastCarrier > kWindowHold) {
                say("Inmarsat: window at " + std::to_string((long)(wins[i]->centerHz / 1000)) + " kHz closed");
                retire(*wins[i]);
                wins.erase(wins.begin() + (long)i);
                changed = true;
            } else {
                i++;
            }
        }
        // carriers outside every window: the focused one first, then the strongest
        std::vector<const DetectedCarrier*> un;
        for (const auto& c : cars) {
            if (!c.confirmed) continue;
            bool in = false;
            for (const auto& w : wins) in |= covers(*w, c.offsetHz);
            if (!in) un.push_back(&c);
        }
        std::stable_sort(un.begin(), un.end(), [&](const DetectedCarrier* a, const DetectedCarrier* b) {
            if ((a->id == focusId) != (b->id == focusId)) return a->id == focusId;
            return a->snrDb > b->snrDb;
        });
        const double limit = 0.5 * 0.9 * cfg.rate;      // windows stay inside the usable part of the capture
        std::vector<char> used(un.size(), 0);
        for (size_t i = 0; i < un.size(); i++) {
            if (used[i]) continue;
            bool medium = un[i]->cls == CarrierClass::Medium;
            double lo = un[i]->offsetHz, hi = lo;
            used[i] = 1;
            for (size_t j = i + 1; j < un.size(); j++) {
                if (used[j]) continue;
                const double nlo = std::min(lo, un[j]->offsetHz), nhi = std::max(hi, un[j]->offsetHz);
                if (nhi - nlo <= cfg.windowBwHz - 2 * kMargin) { lo = nlo; hi = nhi; used[j] = 1; medium |= un[j]->cls == CarrierClass::Medium; }
            }
            double centre = 0.5 * (lo + hi);
            centre = std::max(-limit + cfg.windowBwHz / 2, std::min(limit - cfg.windowBwHz / 2, centre));
            const double cost = kAeroCost + (medium && cfg.inmcOn ? kInmcCost : 0);
            if (totalCost() + cost > cfg.budget + 1e-9) continue;     // parked: the carrier rows say so
            auto w = std::make_unique<Window>();
            w->centerHz = centre;
            w->lastCarrier = t;
            wins.push_back(std::move(w));
            changed = true;
            say("Inmarsat: window at " + std::to_string((long)(centre / 1000)) + " kHz opened");
        }
        // an Inmarsat-C receiver for a window that holds a medium carrier
        if (cfg.inmcOn) {
            for (auto& w : wins) {
                if (w->inmc) continue;
                for (const auto& c : cars) {
                    if (!c.confirmed || c.cls != CarrierClass::Medium || !covers(*w, c.offsetHz)) continue;
                    if (totalCost() + kInmcCost > cfg.budget + 1e-9) break;
                    w->inmcOffset = c.offsetHz - w->centerHz;
                    w->inmc = std::make_unique<InmcReceiver>();
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            rebuildChannelizer();
            for (auto& w : wins) {
                if (!w->aero) w->aero = std::make_unique<AeroReceiver>();
                w->aero->configure(w->outRate);
                w->aero->setSignalOffset(0);
                w->aero->setSearchSpan(cfg.windowBwHz / 2);   // the channelizer output is wider than the window: its neighbours leak in at the edges
                if (w->inmc) {
                    w->inmc->configure(w->outRate);
                    w->inmc->setSignalOffset(w->inmcOffset);
                    w->inmc->setChannelSearch(true);
                    w->inmc->setSearchSpan(cfg.windowBwHz / 2);
                }
                w->cchans.clear();                         // the window's rate may have changed: the C decoders start again below
            }
        }
        planCChannels(cars);
    }

    // a C-channel (voice) decoder on every wide carrier a window covers, within the budget; gone carriers lose theirs
    void planCChannels(const std::vector<DetectedCarrier>& cars) {
        for (auto& w : wins) {
            for (size_t i = 0; i < w->cchans.size();) {
                const DetectedCarrier* c = nullptr;
                for (const auto& d : cars) if (d.id == w->cchans[i]->carrierId && d.confirmed) c = &d;
                if (!c || !covers(*w, c->offsetHz)) { w->cchans.erase(w->cchans.begin() + (long)i); continue; }
                if (std::fabs(c->offsetHz - w->cchans[i]->offsetHz) > 1500) {   // the detector moved it: follow (the demodulator searches +-2 kHz)
                    w->cchans[i]->offsetHz = c->offsetHz;
                    w->cchans[i]->mixInc = -2 * M_PI * (c->offsetHz - w->centerHz) / w->outRate;
                }
                i++;
            }
        }
        for (const auto& c : cars) {
            if (!c.confirmed || c.cls != CarrierClass::Wide || c.widthHz > kCMaxWidth) continue;
            for (auto& w : wins) {
                if (!covers(*w, c.offsetHz) || w->outRate <= 0) continue;
                bool have = false;
                for (const auto& cc : w->cchans) have |= cc->carrierId == c.id;
                if (have) break;
                if (totalCost() + kCCost > cfg.budget + 1e-9) break;
                auto cc = std::make_unique<CChan>();
                cc->carrierId = c.id;
                cc->offsetHz = c.offsetHz;
                cc->mixInc = -2 * M_PI * (c.offsetHz - w->centerHz) / w->outRate;
                cc->demod = std::make_unique<AeroDemod>(8400, w->outRate, 2000);
                CChan* raw = cc.get();
                cc->demod->setCallback([this, raw](AeroFrameEvent& e) { onCFrame(*raw, e); });
                w->cchans.push_back(std::move(cc));
                break;
            }
        }
    }

    void onCFrame(CChan& cc, AeroFrameEvent& e) {
        cc.frames++; cc.susOk += (uint64_t)e.susOk; cc.susBad += (uint64_t)e.susBad; cc.ebn0Db = e.ebn0Db;
        const double t = det.nowSec();
        if (e.susOk) cc.lastGood = t;
        // the voice goes on while the frames check (JAERO DecodeC: Voicesignal once the data carrier is detected)
        if (!(e.susOk || cc.demod->dataLock()) || e.voice.size() != (size_t)kAeroCVoiceBytes) return;
        const double f = cfg.centerHz != 0 ? cfg.centerHz + cc.offsetHz : cc.offsetHz;
        std::vector<uint8_t> fr(12);
        for (int k = 0; k < kAeroCVoiceFrames; k++) {
            std::copy(e.voice.begin() + 12 * k, e.voice.begin() + 12 * (k + 1), fr.begin());
            cc.voiceFrames++;
            if (voiceSink) voiceSink(cc.carrierId, f, fr, t + 0.02 * k);
        }
    }

    void feedWindows(const cf32* x, size_t n) {
        if (!chz || wins.empty()) return;
        std::vector<lora::ChanBuf*> outs;
        for (auto& w : wins) outs.push_back(&w->buf);
        chz->process(x, n, outs);
        for (auto& w : wins) {
            lora::ChanBuf& b = w->buf;
            const int64_t e = b.end();
            int64_t i = b.begin();
            while (i < e) {
                const size_t k = (size_t)std::min<int64_t>(e - i, 32768);
                if (w->aero) w->aero->feed(b.at(i), k);
                if (w->inmc) w->inmc->feed(b.at(i), k);
                for (auto& cc : w->cchans) {              // each C decoder sees its carrier at 0 Hz
                    cc->tmp.resize(k);
                    const cf32* src = b.at(i);
                    for (size_t j = 0; j < k; j++) {
                        cc->tmp[j] = src[j] * cf32((float)std::cos(cc->mixPhase), (float)std::sin(cc->mixPhase));
                        cc->mixPhase += cc->mixInc;
                        if (cc->mixPhase > M_PI) cc->mixPhase -= 2 * M_PI; else if (cc->mixPhase < -M_PI) cc->mixPhase += 2 * M_PI;
                    }
                    cc->demod->feed(cc->tmp.data(), k);
                }
                i += (int64_t)k;
            }
            b.trim(e);
        }
    }

    void feed(const cf32* x, size_t n) {
        constexpr size_t kChunk = 65536;
        for (size_t i = 0; i < n; i += kChunk) {
            const size_t k = std::min(kChunk, n - i);
            det.feed(x + i, k);
            if (det.searches() != lastSearches) { lastSearches = det.searches(); plan(); }
            feedWindows(x + i, k);
        }
        const double t = det.nowSec();
        if (lastPublish < 0 || t - lastPublish >= 0.5) { lastPublish = t; publish(); }
    }

    // ---- merge the windows into one report ----
    void publish() {
        Snapshot s;
        s.rate = cfg.rate; s.centerHz = cfg.centerHz; s.timeSec = det.nowSec();
        s.cpuBudget = cfg.budget; s.cpuCost = totalCost(); s.floorDb = (float)det.floorDb();
        for (auto& w : wins) {
            if (w->aero && w->aero->telemetry(w->at, w->lastA)) w->lastA = w->at.seq;
            if (w->inmc && w->inmc->telemetry(w->it, w->lastI)) w->lastI = w->it.seq;
            WindowInfo wi;
            wi.centerHz = w->centerHz; wi.bwHz = cfg.windowBwHz; wi.outRate = w->outRate;
            wi.aero = w->aero != nullptr; wi.inmc = w->inmc != nullptr; wi.cost = winCost(*w);
            s.windows.push_back(wi);
        }
        std::vector<std::pair<const AeroTelemetry*, double>> as;   // each Aero table with its window's centre
        for (const auto& w : wins) if (w->aero) as.push_back({&w->at, w->centerHz});
        for (size_t i = 0; i < retiredA.size(); i++) as.push_back({&retiredA[i], i < retiredAc.size() ? retiredAc[i] : 0.0});
        std::map<std::string, AeroAircraft> air;
        for (const auto& [a, wc] : as) {
            const double base = cfg.centerHz != 0 ? cfg.centerHz + wc : 0;   // absolute frequency of this window's centre
            for (auto m : a->messages) {
                m.heardHz = base != 0 ? base + m.heardOffsetHz : 0;
                s.messages.push_back(m);
            }
            for (const auto& l : a->logons) s.logons.push_back(l);
            for (auto ac : a->aircraft) {
                ac.heardHz = base != 0 && ac.heardBitRate ? base + ac.heardOffsetHz : 0;
                auto it = air.find(planeKey(ac));
                if (it == air.end()) air[planeKey(ac)] = ac;
                else mergeAircraft(it->second, ac);
            }
            s.messagesTotal += a->messagesTotal; s.positionsTotal += a->positionsTotal; s.susOk += a->blocksOk; s.susBad += a->blocksBad;
        }
        std::stable_sort(s.messages.begin(), s.messages.end(), [](const AeroMessage& a, const AeroMessage& b) { return a.wallTime < b.wallTime; });
        if (s.messages.size() > 300) s.messages.erase(s.messages.begin(), s.messages.end() - 300);
        std::stable_sort(s.logons.begin(), s.logons.end(), [](const AeroLogonEntry& a, const AeroLogonEntry& b) { return a.wallTime < b.wallTime; });
        if (s.logons.size() > 200) s.logons.erase(s.logons.begin(), s.logons.end() - 200);
        for (auto& kv : air) s.aircraft.push_back(kv.second);
        std::sort(s.aircraft.begin(), s.aircraft.end(), [](const AeroAircraft& a, const AeroAircraft& b) { return a.lastWall > b.lastWall; });
        if (s.aircraft.size() > 200) s.aircraft.resize(200);
        for (const auto& w : wins) if (w->inmc) for (const auto& m : w->it.messages) s.egc.push_back(m);
        for (const auto& r : retiredI) for (const auto& m : r.messages) s.egc.push_back(m);
        std::stable_sort(s.egc.begin(), s.egc.end(), [](const InmcMessage& a, const InmcMessage& b) { return a.rxUnix > b.rxUnix; });
        if (s.egc.size() > 150) s.egc.resize(150);
        if (getenv("DECT2_INMSAT_DEBUG")) {                 // what the Aero receivers report per window, against the detector's carriers
            for (size_t wi = 0; wi < wins.size(); wi++) {
                const Window& w = *wins[wi];
                if (!w.aero) continue;
                for (const auto& ch : w.at.channels)
                    fprintf(stderr, "[inmsat dbg] window %zu centre %+.1f kHz out %.0f: aero channel at %+.1f kHz (abs %+.1f kHz) rate %d state %d level %.0f ebn0 %.1f frames %llu\n",
                            wi, w.centerHz / 1e3, w.outRate, ch.offsetHz / 1e3, (w.centerHz + ch.offsetHz) / 1e3, ch.bitRate, ch.state, ch.levelDb, ch.ebn0Db, (unsigned long long)ch.frames);
            }
            size_t nsys = 0; for (const auto& ap : as) nsys += ap.first->sys.size();
            fprintf(stderr, "[inmsat dbg] sys facts %zu, centre %.0f\n", nsys, cfg.centerHz);
        }
        // carrier table: the detector's carriers, with what the receivers made of them
        for (const auto& c : det.carriers()) {
            if (!c.confirmed) continue;
            CarrierRow r;
            r.id = c.id; r.offsetHz = c.offsetHz; r.widthHz = c.widthHz; r.snrDb = c.snrDb; r.cls = c.cls;
            r.freqHz = cfg.centerHz != 0 ? cfg.centerHz + c.offsetHz : 0;
            r.focus = c.id == focusId;
            // the window that owns the carrier is the first one covering it, but the receivers' channels are matched across all windows:
            // the Aero and Inmarsat-C receivers report channels over their whole channelizer output, so a carrier is often decoded by a
            // window whose declared span does not cover it, and the C-channel decoders sit in whichever window had room
            const double reach = std::max(3000.0, c.widthHz / 2 + 1500);
            for (size_t wi = 0; wi < wins.size(); wi++) {
                const Window& w = *wins[wi];
                if (r.window < 0 && covers(w, c.offsetHz)) r.window = (int)wi;
                if (w.aero) {
                    for (const auto& ch : w.at.channels) {
                        if (std::fabs(w.centerHz + ch.offsetHz - c.offsetHz) > reach) continue;
                        if (ch.state > r.state) {
                            if (ch.state >= 2) r.type = CarrierType::AeroP;      // a carrier lock alone says nothing about the kind
                            r.bitRate = ch.bitRate; r.state = ch.state; r.ebn0Db = ch.ebn0Db;
                            r.frames = ch.frames; r.idleSec = ch.lastHeard > 0 ? std::max(0.0, w.at.timeSec - ch.lastHeard) : -1;
                        }
                    }
                }
                if (w.inmc) {
                    for (const auto& ch : w.it.channels) {
                        if (std::fabs(w.centerHz + ch.carrierHz - c.offsetHz) > reach) continue;
                        const int st = ch.state == 2 ? 3 : ch.state;
                        if (st > r.state) {
                            if (st >= 2) r.type = CarrierType::InmarsatC;
                            r.bitRate = 1200; r.state = st; r.ebn0Db = ch.ebn0Db;
                            r.frames = ch.framesOk; r.region = ch.region; r.station = ch.lesName; r.idleSec = -1;
                        }
                    }
                }
                for (const auto& cc : w.cchans) {       // the C-channel (voice) decoder on this carrier
                    if (cc->carrierId != c.id || !cc->demod) continue;
                    const int st = cc->demod->dataLock() ? 3 : cc->demod->synced() ? 2 : cc->demod->acquired() ? 1 : 0;
                    if (st >= 2 && st > r.state) {
                        r.type = CarrierType::AeroC; r.bitRate = 8400; r.state = st; r.ebn0Db = cc->ebn0Db;
                        r.frames = cc->frames; r.idleSec = cc->lastGood >= 0 ? std::max(0.0, det.nowSec() - cc->lastGood) : -1;
                    }
                    r.voiceFrames = cc->voiceFrames;
                }
            }
            r.parked = r.window < 0;
            r.voiceCandidate = r.type != CarrierType::AeroC && c.cls == CarrierClass::Wide && r.state < 2 && det.nowSec() - c.firstSeen > 20;
            s.carriers.push_back(r);
        }
        {   // the common prefix
            int best = 0; float bestSnr = 0;
            for (const auto& r : s.carriers) { best = std::max(best, r.state); if (r.state >= 2) bestSnr = std::max(bestSnr, r.ebn0Db); }
            s.state = best >= 2 ? 2 : (!s.carriers.empty() ? 1 : 0);
            s.snrDb = bestSnr; s.dataValid = s.messagesTotal > 0 || !s.egc.empty();
            s.blocksOk = s.susOk; s.blocksBad = s.susBad;
        }
        // which satellite: the evidence of the decoded content, the carrier frequencies and the geometry (inmsat_satid.h)
        {
            SatInput si;
            si.haveLocation = haveLoc; si.latDeg = locLat; si.lonDeg = locLon; si.manual = satManual;
            for (const auto& r : s.carriers) if (r.type == CarrierType::AeroP && r.freqHz > 0) si.carriers.push_back({r.freqHz, r.bitRate, true});
            for (const auto& asp : as) { const AeroTelemetry* a = asp.first;
                for (const auto& e : a->sys) { SatSysFact f; f.kind = e.kind; f.satId = e.satId; f.lonDeg = e.lonDeg; f.pMHz = e.pMHz; f.spotBeam = e.spotBeam; f.gesId = e.gesId; si.sys.push_back(f); } }
            for (const auto& w : wins) if (w->inmc) {
                if (w->it.ncs.valid) si.inmcRegions.push_back(w->it.ncs.sat);
                for (const auto& ch : w->it.channels) if (ch.sat >= 0) si.inmcRegions.push_back(ch.sat);
            }
            s.sat = identifySatellite(si);
            for (auto& r : s.carriers) r.satellite = satNameForCarrier(s.sat, r.freqHz, r.bitRate);
        }
        std::lock_guard<std::mutex> lk(snapMu);
        s.seq = ++seq;
        snap = std::move(s);
    }
};

Manager::Manager() : p_(std::make_unique<Impl>()) {}
Manager::~Manager() = default;
void Manager::configure(const ManagerConfig& cfg) { std::lock_guard<std::mutex> lk(p_->mu); p_->configure(cfg); }
bool Manager::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->ready(); }
void Manager::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& w : p_->wins) p_->retire(*w);
    p_->wins.clear(); p_->chz.reset();
    p_->det.reset(); p_->lastSearches = 0; p_->lastPublish = -1;
}
void Manager::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& w : p_->wins) {
        if (w->aero) w->aero->clearResults();
        if (w->inmc) w->inmc->clearResults();
        w->at = AeroTelemetry(); w->it = InmcTelemetry(); w->lastA = w->lastI = 0;
        for (auto& cc : w->cchans) { cc->frames = cc->susOk = cc->susBad = cc->voiceFrames = 0; cc->lastGood = -1; }
    }
    p_->retiredA.clear(); p_->retiredI.clear();
}
void Manager::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->ready()) p_->feed(x, n); }
bool Manager::snapshot(Snapshot& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->snapMu);
    if (p_->snap.seq <= lastSeq) return false;
    out = p_->snap;
    return true;
}
void Manager::setCenter(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfg.centerHz = hz; }
void Manager::setBudget(double u) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfg.budget = std::max(1.0, u); }
void Manager::setInmcOn(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfg.inmcOn = on; }
void Manager::setLocation(bool known, double lat, double lon) { std::lock_guard<std::mutex> lk(p_->mu); p_->haveLoc = known; p_->locLat = lat; p_->locLon = lon; }
void Manager::setSatelliteOverride(const std::string& name) { std::lock_guard<std::mutex> lk(p_->mu); p_->satManual = name; }
void Manager::setFocus(int id) { std::lock_guard<std::mutex> lk(p_->mu); p_->focusId = id; }
void Manager::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void Manager::setVoiceSink(VoiceSink sink) { std::lock_guard<std::mutex> lk(p_->mu); p_->voiceSink = std::move(sink); }

} // namespace dect2::inmsat
