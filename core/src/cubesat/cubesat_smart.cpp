// Smart CubeSat decoder: see cubesat_smart.h.
#include "dect2/cubesat/cubesat_smart.h"
#include "cubesat_blind.h"
#include "dect2/dsp/carrierfind.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

namespace dect2 {
namespace cubesat {

namespace {

constexpr double kProbeSec = 1.5;
constexpr size_t kPacketsMax = 2000;

double wallUnix() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

uint64_t hashBytes(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : d) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

std::string isoTime(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string hexOf(const std::vector<uint8_t>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 32) o += ' ';
        else o += c;
    }
    return o;
}

} // namespace

// ============================================================================ Impl
struct SmartCubesat::Impl {
    struct Track {
        int id = 0;
        bool table = false;
        std::vector<TxEntry> tx;          // the table entries that share this carrier
        int norad = 0;
        std::string sat;
        double freqHz = 0;                // absolute, where the channelizer is
        double predictedHz = 0;           // table: nominal + Doppler
        double elDeg = 0;
        int scanId = 0;
        double snrDb = 0, widthHz = 0;
        double lastSeen = -1e9, born = 0;
        bool chanOn = false;
        Channelizer chan;
        std::vector<cf32> probe;
        bool probed = false;
        double probeStart = 0;
        ProbeResult pr;
        std::vector<std::unique_ptr<Chain>> chains;
        std::vector<std::pair<int, int>> chainKey;      // (mod, baud) of the chains
        std::string state = "searching";
        uint64_t frames = 0;
        double lastFrameSec = -1e9;
        std::string lastFraming;
        int bestMod = ModUnknown, bestBaud = 0;
        double estBaud = 0;
        double chainsSince = 0;
        double reprobeAt = 0;
    };

    // settings and time
    mutable std::mutex mu;
    SmartSettings cfg;
    double rate = 0, center = 0;
    Observer obs;
    std::shared_ptr<PassPlanner> plan = std::make_shared<PassPlanner>();
    std::vector<TxEntry> userTx;
    double simStart = 0;
    bool wall = true;
    std::function<void(const std::string&)> logCb;
    // processing state (analysis thread)
    uint64_t samples = 0;
    double sampleT = 0, lastHouse = -1e9, lastPrior = -1e9;
    CarrierScan scan;
    uint64_t lastScanUpdates = 0;
    std::vector<std::unique_ptr<Track>> tracks;
    int nextTrack = 1;
    std::vector<cf32> chanOut;
    struct Expected { std::vector<TxEntry> tx; double hz; double el; int norad; std::string name; };
    std::vector<Expected> expected;
    std::set<int> loggedNoise;
    // results
    std::vector<SmartPacket> pkts;
    SmartStatus st;
    uint64_t frameCount = 0;
    struct Recent { uint64_t h; double t; };
    std::vector<Recent> recent;
    bool configured = false;

    void say(const std::string& s) { if (logCb) logCb(s); }

    double nowUnix() const {
        if (simStart > 0) return simStart + sampleT;
        return wall ? wallUnix() : 0.0;
    }
    bool timeKnown() const { return simStart > 0 || wall; }

    // ---------------------------------------------------------------- table priors
    void refreshExpected(double now) {
        expected.clear();
        if (!cfg.usePasses || center <= 0 || !timeKnown()) return;
        std::shared_ptr<PassPlanner> pl;
        { std::lock_guard<std::mutex> lk(mu); pl = plan; }
        std::vector<Expected> all;
        std::map<int, Look> looks;
        for (const TxEntry& t : pl->transmitters()) {
            if (!decodable(t)) continue;
            auto it = looks.find(t.norad);
            if (it == looks.end()) {
                Look l;
                if (!pl->look(t.norad, now, l)) l.elDeg = -90;
                it = looks.emplace(t.norad, l).first;
            }
            const Look& l = it->second;
            if (l.elDeg < cfg.minElDeg) continue;
            const double f = t.hz + dopplerHz(t.hz, l.rangeRateKmS);
            if (std::fabs(f - center) > rate * 0.5 - 8000.0) continue;
            Expected e; e.tx.push_back(t); e.hz = f; e.el = l.elDeg; e.norad = t.norad; e.name = pl->nameOf(t.norad);
            all.push_back(e);
        }
        // entries of one satellite on (nearly) the same frequency share a carrier
        std::sort(all.begin(), all.end(), [](const Expected& a, const Expected& b) { return a.norad != b.norad ? a.norad < b.norad : a.hz < b.hz; });
        for (const Expected& e : all) {
            if (!expected.empty() && expected.back().norad == e.norad && std::fabs(expected.back().hz - e.hz) < 3000.0) { expected.back().tx.push_back(e.tx[0]); continue; }
            expected.push_back(e);
        }
        std::sort(expected.begin(), expected.end(), [](const Expected& a, const Expected& b) { return a.el > b.el; });
    }

    // ---------------------------------------------------------------- tracks
    Track* findTrackByNoradFreq(int norad, double hz) {
        for (auto& t : tracks) if (t->table && t->norad == norad && std::fabs(t->predictedHz - hz) < 3500.0) return t.get();
        return nullptr;
    }

    int activeCount() const { int n = 0; for (const auto& t : tracks) if (t->chanOn) n++; return n; }

    void startChannel(Track& t) {
        double width = cfg.channelHz > 0 ? cfg.channelHz : std::max(12000.0, std::min(90000.0, t.widthHz * 1.6));
        if (!t.chan.configure(rate, kChanRate, t.freqHz - center, width)) { t.state = "carrier found"; return; }
        t.chanOn = true;
        t.probe.clear(); t.probed = false; t.probeStart = sampleT;
        t.chains.clear(); t.chainKey.clear();
        t.state = "analysing";
        // the table's hypotheses start at once; the probe adds the measured ones
        for (const TxEntry& x : t.tx) addChainFor(t, x.mod, x.baud);
    }

    void stopChannel(Track& t) {
        t.chanOn = false;
        t.probe.clear(); t.probe.shrink_to_fit();
        t.chains.clear(); t.chainKey.clear();
        t.probed = false;
    }

    void addChainFor(Track& t, int mod, int baud) {
        std::vector<std::pair<int, int>> want;
        if (mod == ModFsk) {
            if (baud > 0) want.push_back({mod, baud});
            else for (int b : {1200, 2400, 4800, 9600, 19200}) want.push_back({mod, b});
        } else if (mod == ModBpsk) {
            if (baud > 0) want.push_back({mod, baud});
            else for (int b : {1200, 9600}) want.push_back({mod, b});
        } else if (mod == ModAfsk) want.push_back({mod, 1200});
        else if (mod == ModCw) want.push_back({mod, 0});
        for (const auto& w : want) {
            if (std::find(t.chainKey.begin(), t.chainKey.end(), w) != t.chainKey.end()) continue;
            if (t.chains.size() >= 8) return;
            std::unique_ptr<Chain> c;
            switch (w.first) {
            case ModFsk: c = makeFskChain(w.second); break;
            case ModBpsk: c = makeBpskChain(w.second); break;
            case ModAfsk: c = makeAfskChain(); break;
            case ModCw: c = makeCwChain(); break;
            default: break;
            }
            if (!c) continue;
            Track* tp = &t;
            c->sink = [this, tp](ChainFrame& f) { onFrame(*tp, f); };
            t.chains.push_back(std::move(c));
            t.chainKey.push_back(w);
        }
    }

    void finishProbe(Track& t) {
        t.probed = true;
        t.pr = classifyCarrier(t.probe, cfg.forceMod, cfg.forceBaud);
        t.estBaud = t.pr.estBaudFsk;
        const size_t before = t.chains.size();
        if (t.pr.carrierOnly) { t.state = "carrier only"; t.reprobeAt = sampleT + 30; }
        else if (cfg.forceMod != ModUnknown) {
            if (cfg.forceBaud > 0 || cfg.forceMod == ModCw || cfg.forceMod == ModAfsk) addChainFor(t, cfg.forceMod, cfg.forceBaud);
            else addChainFor(t, cfg.forceMod, 0);
        } else {
            int n = 0;
            for (const Hyp& h : t.pr.hyps) {
                if (h.score < t.pr.hyps.front().score - 70 || n >= 6) break;
                addChainFor(t, h.mod, h.baud);
                n++;
            }
        }
        if (!t.pr.hyps.empty()) { t.bestMod = t.pr.hyps.front().mod; t.bestBaud = t.pr.hyps.front().baud; }
        else if (!t.tx.empty()) { t.bestMod = t.tx[0].mod; t.bestBaud = t.tx[0].baud; }
        // replay the probe through the chains that were added late
        for (size_t i = before; i < t.chains.size(); i++) t.chains[i]->process(t.probe.data(), t.probe.size());
        t.probe.clear(); t.probe.shrink_to_fit();
        t.chainsSince = sampleT;
        if (t.state != "carrier only") t.state = t.chains.empty() ? "no known signal" : (t.frames ? "decoding" : "analysing");
        say("CubeSat: " + (t.sat.empty() ? std::string("carrier") : t.sat) + " " + std::to_string((long long)std::llround(t.freqHz / 1e3)) + " kHz: " +
            (t.pr.carrierOnly ? "unmodulated carrier" : t.pr.hyps.empty() ? "no recognisable modulation" : "looks like " + modulationText(t.pr.hyps.front().mod, t.pr.hyps.front().baud)) +
            (t.chains.empty() ? "" : ", trying " + std::to_string(t.chains.size()) + " demodulator(s)"));
    }

    // ---------------------------------------------------------------- frames
    void onFrame(Track& t, ChainFrame& f) {
        const double nowT = sampleT;
        // the same frame heard by two demodulators of one carrier (or two tracks) counts once
        uint64_t h = hashBytes(f.data.empty() ? std::vector<uint8_t>(f.text.begin(), f.text.end()) : f.data);
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const Recent& r) { return nowT - r.t > 4.0; }), recent.end());
        for (const Recent& r : recent) if (r.h == h) return;
        recent.push_back({h, nowT});
        SmartPacket p;
        p.timeSec = nowT;
        p.wallTime = wall && simStart <= 0 ? (int64_t)wallUnix() : simStart > 0 ? (int64_t)(simStart + nowT) : 0;
        p.freqHz = t.freqHz;
        p.mod = f.mod; p.baud = f.baud;
        p.framing = f.framingName; p.how = f.how; p.rsErrors = f.rsErrors; p.framingId = f.framing;
        p.from = f.from; p.to = f.to; p.path = f.path; p.type = f.type;
        p.hasCsp = f.hasCsp; p.csp = f.csp;
        p.data = f.data; p.payload = f.payload; p.text = f.text;
        p.snrDb = (float)t.snrDb;
        if (t.table && t.norad) { p.sat = t.sat; p.norad = t.norad; }
        else guessSatellite(p, t);
        parseTelemetry(f.framing, f.data, f.payload, p.tel, p.norad);
        if (f.framing == kFramingCw) { p.tel.kind = "CW beacon"; TelField tf; tf.name = "Text"; tf.text = f.text; p.tel.fields.push_back(tf); }
        pkts.insert(pkts.begin(), p);
        if (pkts.size() > kPacketsMax) pkts.pop_back();
        frameCount++;
        t.frames++;
        t.lastFrameSec = nowT;
        t.lastFraming = f.framingName;
        t.state = "decoding";
        t.bestMod = f.mod; t.bestBaud = f.baud;
        if (frameCount <= 40) say("CubeSat: " + packetSummary(p));
    }

    void guessSatellite(SmartPacket& p, const Track& t) {
        if (!timeKnown()) return;
        std::shared_ptr<PassPlanner> pl;
        { std::lock_guard<std::mutex> lk(mu); pl = plan; }
        const double now = nowUnix();
        double best = 9000.0;
        const TxEntry* bt = nullptr;
        std::map<int, bool> seen;
        for (const TxEntry& x : pl->transmitters()) {
            if (std::fabs(x.hz - t.freqHz) > 20000.0) continue;
            Look l;
            if (!pl->look(x.norad, now, l) || l.elDeg < -2.0) continue;
            const double f = x.hz + dopplerHz(x.hz, l.rangeRateKmS);
            const double d = std::fabs(f - t.freqHz);
            if (d < best) { best = d; bt = &x; }
        }
        if (bt) { p.sat = pl->nameOf(bt->norad); p.norad = bt->norad; p.satGuess = true; }
    }

    // ---------------------------------------------------------------- the periodic work
    void housekeeping() {
        const double now = nowUnix();
        const double span = rate;
        if (sampleT - lastPrior >= 1.0) { lastPrior = sampleT; refreshExpected(now); }
        const std::vector<ScanCarrier> sc = scan.carriers();
        std::vector<bool> claimed(sc.size(), false);
        auto absHz = [&](const ScanCarrier& c) { return center + c.hz; };
        // table priors: link each expected carrier to the scan carrier nearest its predicted place
        for (const Expected& e : expected) {
            Track* t = findTrackByNoradFreq(e.norad, e.hz);
            int bi = -1;
            double bd = cfg.searchHz;
            for (size_t i = 0; i < sc.size(); i++) {
                const double d = std::fabs(absHz(sc[i]) - e.hz);
                if (d < bd && sc[i].snrDb >= cfg.minSnrDb - 3.0 && sc[i].hits >= 3) { bd = d; bi = (int)i; }
            }
            if (!t) {
                tracks.push_back(std::make_unique<Track>());
                t = tracks.back().get();
                t->id = nextTrack++;
                t->table = true; t->norad = e.norad; t->sat = e.name; t->tx = e.tx; t->born = sampleT;
                t->freqHz = e.hz;
                t->bestMod = e.tx[0].mod; t->bestBaud = e.tx[0].baud;
            } else {
                // the entries that joined the carrier since
                for (const TxEntry& x : e.tx) {
                    bool have = false;
                    for (const TxEntry& y : t->tx) if (y.hz == x.hz && y.mod == x.mod && y.baud == x.baud) have = true;
                    if (!have) t->tx.push_back(x);
                }
            }
            t->predictedHz = e.hz; t->elDeg = e.el;
            if (bi >= 0) {
                claimed[(size_t)bi] = true;
                t->scanId = sc[(size_t)bi].id; t->snrDb = sc[(size_t)bi].snrDb; t->widthHz = sc[(size_t)bi].widthHz; t->lastSeen = sampleT;
                const double target = absHz(sc[(size_t)bi]);
                t->freqHz = t->chanOn ? t->freqHz + 0.35 * (target - t->freqHz) : target;
            } else {
                t->freqHz = t->chanOn ? t->freqHz + 0.2 * (e.hz - t->freqHz) : e.hz;     // no carrier in the scan: follow the prediction
            }
            if (bi >= 0 && !t->chanOn && activeCount() < cfg.maxChannels) startChannel(*t);
            else if (bi < 0 && !t->chanOn) t->state = "searching";
        }
        // blind carriers: the scan carriers nobody claimed
        for (size_t i = 0; i < sc.size(); i++) {
            if (claimed[i]) continue;
            // inside an existing track's channel?
            Track* near = nullptr;
            for (auto& t : tracks) if (std::fabs(t->freqHz - absHz(sc[i])) < std::max(2500.0, 0.6 * std::max(t->widthHz, sc[i].widthHz)) && !(t->table && !t->chanOn)) near = t.get();
            if (near) {
                claimed[i] = true;
                if (!near->table) {
                    near->lastSeen = sampleT; near->snrDb = sc[i].snrDb; near->widthHz = sc[i].widthHz; near->scanId = sc[i].id;
                    near->freqHz += 0.35 * (absHz(sc[i]) - near->freqHz);
                } else { near->lastSeen = sampleT; }
                continue;
            }
            if (!cfg.blind || sc[i].hits < 4 || sc[i].snrDb < cfg.minSnrDb) continue;
            if (sc[i].widthHz > 60000.0) continue;                         // wider than the channelizer's 96 kHz: LoRa-like, not handled blind
            if (activeCount() >= cfg.maxChannels) continue;
            tracks.push_back(std::make_unique<Track>());
            Track* t = tracks.back().get();
            t->id = nextTrack++; t->table = false; t->born = sampleT; t->freqHz = absHz(sc[i]); t->predictedHz = t->freqHz;
            t->scanId = sc[i].id; t->snrDb = sc[i].snrDb; t->widthHz = sc[i].widthHz; t->lastSeen = sampleT;
            t->sat = "";
            guessName(*t, now);
            startChannel(*t);
            claimed[i] = true;
        }
        // keep the channelizers on their carriers (the Doppler moves them)
        for (auto& t : tracks) if (t->chanOn) t->chan.setShift(t->freqHz - center);
        // retire
        for (auto& t : tracks) {
            const double quiet = sampleT - t->lastSeen;
            if (t->chanOn && quiet > 12.0) {
                t->state = t->table ? "searching" : "gone";
                stopChannel(*t);
            }
            if (t->chanOn && !t->probed && sampleT - t->probeStart > 6.0) {
                // a very slow probe would only happen with a stalled stream: take what there is
            }
            if (t->chanOn && t->probed && t->state == "carrier only" && sampleT >= t->reprobeAt) { t->probed = false; t->probe.clear(); t->probeStart = sampleT; t->state = "analysing"; }
            // nothing validates after a long time: say so, keep the chains (a pass can start late)
            if (t->chanOn && t->probed && t->frames == 0 && !t->chains.empty() && sampleT - t->chainsSince > 45.0 && t->state == "analysing") t->state = "no known signal";
            if (t->chanOn && t->frames && sampleT - t->lastFrameSec > 30.0 && t->state == "decoding") t->state = "carrier found";
        }
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const std::unique_ptr<Track>& t) {
            if (t->chanOn) return false;
            if (t->table) {
                bool still = false;
                for (const Expected& e : expected) if (e.norad == t->norad && std::fabs(e.hz - t->predictedHz) < 20000.0) still = true;
                return !still && sampleT - t->lastSeen > 10.0;
            }
            return sampleT - t->lastSeen > 20.0;
        }), tracks.end());
        (void)span;
        publish(now);
    }

    void guessName(Track& t, double now) {
        if (!timeKnown()) return;
        std::shared_ptr<PassPlanner> pl;
        { std::lock_guard<std::mutex> lk(mu); pl = plan; }
        double best = 9000.0;
        for (const TxEntry& x : pl->transmitters()) {
            if (std::fabs(x.hz - t.freqHz) > 20000.0) continue;
            Look l;
            if (!pl->look(x.norad, now, l) || l.elDeg < -2.0) continue;
            const double d = std::fabs(x.hz + dopplerHz(x.hz, l.rangeRateKmS) - t.freqHz);
            if (d < best) { best = d; t.sat = pl->nameOf(x.norad) + " (?)"; t.norad = 0; }
        }
    }

    void publish(double now) {
        SmartStatus s;
        s.centerHz = center; s.rateHz = rate; s.timeSec = sampleT; s.noiseDb = scan.noiseDb();
        s.frames = frameCount; s.scans = scan.updates();
        int best = 0;
        for (const auto& t : tracks) {
            SmartCarrier c;
            c.id = t->id; c.freqHz = t->freqHz; c.widthHz = t->widthHz; c.snrDb = t->snrDb; c.fromTable = t->table; c.sat = t->sat; c.norad = t->norad; c.elDeg = t->elDeg;
            c.state = t->state; c.mod = t->bestMod; c.baud = t->bestBaud; c.estBaud = t->estBaud;
            c.modText = modulationText(t->bestMod, t->bestBaud);
            c.framing = t->lastFraming; c.frames = t->frames; c.lastFrameSec = t->lastFrameSec;
            std::vector<TrialStats> ts;
            for (const auto& ch : t->chains) ch->stats(ts);
            for (const TrialStats& x : ts) c.trials.push_back({x.label, x.heard, x.valid});
            std::sort(c.trials.begin(), c.trials.end(), [](const SmartTrial& a, const SmartTrial& b) { return a.valid != b.valid ? a.valid > b.valid : a.heard > b.heard; });
            const int lvl = t->state == "decoding" ? 2 : (t->chanOn ? 1 : 0);
            best = std::max(best, lvl);
            s.carriers.push_back(std::move(c));
        }
        s.state = best;
        if (best == 0 && !expected.empty()) s.note = "waiting for " + expected.front().name;
        {
            std::lock_guard<std::mutex> lk(mu);
            st = std::move(s);
        }
        (void)now;
    }
};

// ============================================================================ API
SmartCubesat::SmartCubesat() : p_(std::make_unique<Impl>()) {}
SmartCubesat::~SmartCubesat() = default;

void SmartCubesat::configure(double rateHz) {
    Impl& I = *p_;
    I.rate = rateHz;
    I.scan.minSnrDb = I.cfg.minSnrDb;
    I.scan.configure(rateHz, I.cfg.scanBinHz);
    I.tracks.clear();
    I.samples = 0; I.sampleT = 0; I.lastHouse = -1e9; I.lastPrior = -1e9;
    I.configured = rateHz > 0;
}
void SmartCubesat::setCenterHz(double hz) { p_->center = hz; }
void SmartCubesat::setObserver(const Observer& o) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->obs = o;
    auto np = std::make_shared<PassPlanner>(*p_->plan);
    np->setObserver(o);
    p_->plan = np;
}
void SmartCubesat::setSettings(const SmartSettings& s) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cfg = s;
    p_->scan.minSnrDb = s.minSnrDb;
}
SmartSettings SmartCubesat::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }
void SmartCubesat::setSimTime(double startUnix) { p_->simStart = startUnix; }
void SmartCubesat::setWallClock(bool on) { p_->wall = on; }
size_t SmartCubesat::addTleText(const std::string& text) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto np = std::make_shared<PassPlanner>(*p_->plan);
    const size_t n = np->addTleText(text);
    p_->plan = np;
    return n;
}
void SmartCubesat::setUserTransmitters(const std::vector<TxEntry>& v) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->userTx = v;
    auto np = std::make_shared<PassPlanner>(*p_->plan);
    np->setExtraTx(v);
    p_->plan = np;
}
std::vector<PlannedPass> SmartCubesat::upcoming(double nowUnix, double hours, double minElDeg, size_t max) const {
    std::shared_ptr<PassPlanner> pl;
    { std::lock_guard<std::mutex> lk(p_->mu); pl = p_->plan; }
    return pl->upcoming(nowUnix, hours, minElDeg, max);
}
std::vector<TxEntry> SmartCubesat::transmitters() const {
    std::shared_ptr<PassPlanner> pl;
    { std::lock_guard<std::mutex> lk(p_->mu); pl = p_->plan; }
    return pl->transmitters();
}
size_t SmartCubesat::satelliteCount() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->plan->satelliteCount(); }
double SmartCubesat::tleNewestEpochUnix() const { std::lock_guard<std::mutex> lk(p_->mu); return jdToUnix(p_->plan->newestEpochJd()); }
bool SmartCubesat::look(int norad, double t, Look& l) const {
    std::shared_ptr<PassPlanner> pl;
    { std::lock_guard<std::mutex> lk(p_->mu); pl = p_->plan; }
    return pl->look(norad, t, l);
}
void SmartCubesat::reset() {
    Impl& I = *p_;
    I.tracks.clear();
    I.scan.reset();
    I.sampleT = 0; I.samples = 0; I.lastHouse = -1e9; I.lastPrior = -1e9;
    I.recent.clear();
}
void SmartCubesat::clearPackets() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pkts.clear();
    p_->frameCount = 0;
}
bool SmartCubesat::ready() const { return p_->configured; }
void SmartCubesat::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }

void SmartCubesat::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.configured || n == 0) return;
    I.scan.feed(x, n);
    I.samples += n;
    I.sampleT = (double)I.samples / I.rate;
    if (I.sampleT - I.lastHouse >= 0.25) { I.lastHouse = I.sampleT; I.housekeeping(); }
    for (auto& tp : I.tracks) {
        Impl::Track& t = *tp;
        if (!t.chanOn) continue;
        I.chanOut.clear();
        t.chan.process(x, n, I.chanOut);
        if (I.chanOut.empty()) continue;
        if (!t.probed) {
            t.probe.insert(t.probe.end(), I.chanOut.begin(), I.chanOut.end());
            for (auto& c : t.chains) c->process(I.chanOut.data(), I.chanOut.size());
            if (t.probe.size() >= (size_t)(kProbeSec * kChanRate)) I.finishProbe(t);
        } else {
            for (auto& c : t.chains) c->process(I.chanOut.data(), I.chanOut.size());
        }
    }
    // SNR of a track for the packets comes from the scan (housekeeping)
}

SmartStatus SmartCubesat::status() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }
std::vector<SmartPacket> SmartCubesat::packets() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->pkts; }

SmartCubesat::TuneAdvice SmartCubesat::adviseTune(double now, double maxSpan) const {
    TuneAdvice a;
    std::shared_ptr<PassPlanner> pl;
    { std::lock_guard<std::mutex> lk(p_->mu); pl = p_->plan; }
    const std::vector<PlannedPass> ps = pl->upcoming(now, 3.0, 5.0, 0);
    if (ps.empty()) return a;
    // the passes that are up now or start within 10 minutes of the first one; their transmitters, weighted by the pass's peak elevation
    const double t0 = ps.front().p.aos > now ? ps.front().p.aos : now;
    struct Item { double hz, w; std::string name; };
    std::vector<Item> items;
    for (const PlannedPass& p : ps) {
        if (p.p.aos > t0 + 600.0) continue;
        for (size_t i : p.tx) items.push_back({pl->transmitters()[i].hz, p.p.maxElDeg, p.name});
    }
    if (items.empty()) return a;
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.hz < y.hz; });
    // best window of width maxSpan * 0.8
    const double win = maxSpan * 0.8;
    size_t bi = 0, bj = 0;
    double bw = -1;
    for (size_t i = 0, j = 0; i < items.size(); i++) {
        double w = 0;
        j = i;
        while (j < items.size() && items[j].hz - items[i].hz <= win) { w += items[j].w; j++; }
        if (w > bw) { bw = w; bi = i; bj = j; }
    }
    a.valid = true;
    a.startUnix = t0;
    a.centerHz = 0.5 * (items[bi].hz + items[bj - 1].hz);
    a.spanHz = std::max(items[bj - 1].hz - items[bi].hz + 60000.0, 100000.0);
    std::set<std::string> names;
    for (size_t k = bi; k < bj; k++) names.insert(items[k].name);
    std::string w;
    for (const std::string& nme : names) { if (!w.empty()) w += ", "; w += nme; if (w.size() > 90) { w += " ..."; break; } }
    a.why = std::to_string(names.size()) + " satellite(s): " + w;
    return a;
}

// ============================================================================ exports
std::string packetSummary(const SmartPacket& p) {
    std::string s = p.sat.empty() ? "unknown satellite" : p.sat + (p.satGuess ? " (?)" : "");
    s += ", " + modulationText(p.mod, p.baud) + ", " + p.framing;
    if (!p.from.empty()) s += ", " + p.from;
    if (p.rsErrors > 0) s += ", " + std::to_string(p.rsErrors) + " byte errors put right";
    s += ", " + std::to_string(p.data.size()) + " bytes";
    if (!p.text.empty() && p.framingId == kFramingCw) s += ": " + p.text;
    return s;
}

std::string exportSatnogsJson(const std::vector<SmartPacket>& v, const std::string& call, double lat, double lon) {
    std::string o;
    char b[64];
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        const SmartPacket& p = *it;
        if (p.data.empty() || p.norad <= 0) continue;
        o += "{\"noradID\":" + std::to_string(p.norad) + ",\"source\":\"" + jsonEsc(call.empty() ? "OnAir" : call) + "\",\"timestamp\":\"" + isoTime(p.wallTime) + "\",\"frame\":\"" + hexOf(p.data) + "\",\"locator\":\"longLat\",";
        snprintf(b, sizeof b, "\"latitude\":%.4f,\"longitude\":%.4f,", lat, lon);
        o += b;
        o += "\"version\":\"OnAir Smart CubeSat\",\"framing\":\"" + jsonEsc(p.framing) + "\"}\n";
    }
    return o;
}

std::string sidsForm(const SmartPacket& p, const std::string& call, double lat, double lon, int64_t wallMs) {
    if (p.data.empty() || p.norad <= 0 || p.wallTime <= 0 || call.empty()) return {};
    auto enc = [](const std::string& s) { std::string o; for (unsigned char c : s) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c; else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; } } return o; };
    const int64_t ms = wallMs > 0 ? wallMs : p.wallTime * 1000;
    const time_t t = (time_t)(ms / 1000);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char ts[40];
    snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, (int)(ms % 1000));
    std::string hexUp = hexOf(p.data);
    for (char& c : hexUp) c = (char)toupper((unsigned char)c);   // upper case, as gr-satellites sends it
    char la[32], lo[32];
    snprintf(la, sizeof la, "%.4f%c", std::fabs(lat), lat >= 0 ? 'N' : 'S');
    snprintf(lo, sizeof lo, "%.4f%c", std::fabs(lon), lon >= 0 ? 'E' : 'W');
    return "noradID=" + std::to_string(p.norad) + "&source=" + enc(call) + "&timestamp=" + enc(ts) + "&frame=" + hexUp + "&locator=longLat&longitude=" + enc(lo) + "&latitude=" + enc(la) + "&version=" + enc("OnAir 3.9");
}

std::string exportCsv(const std::vector<SmartPacket>& v) {
    std::string o = "time_utc,satellite,norad,frequency_hz,modulation,baud,framing,validated_by,rs_errors,snr_db,bytes,hex,text\n";
    char b[64];
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        const SmartPacket& p = *it;
        o += isoTime(p.wallTime) + ",\"" + jsonEsc(p.sat) + "\"," + std::to_string(p.norad) + ",";
        snprintf(b, sizeof b, "%.0f,", p.freqHz);
        o += b;
        o += "\"" + modulationText(p.mod, p.baud) + "\"," + std::to_string(p.baud) + ",\"" + jsonEsc(p.framing) + "\",\"" + jsonEsc(p.how) + "\"," + std::to_string(p.rsErrors) + ",";
        snprintf(b, sizeof b, "%.1f,", p.snrDb);
        o += b;
        o += std::to_string(p.data.size()) + "," + hexOf(p.data) + ",\"" + jsonEsc(p.text) + "\"\n";
    }
    return o;
}

std::string exportKiss(const std::vector<SmartPacket>& v) {
    std::string o;
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        if (it->data.empty()) continue;
        o += (char)0xC0; o += (char)0x00;
        for (uint8_t c : it->data) {
            if (c == 0xC0) { o += (char)0xDB; o += (char)0xDC; }
            else if (c == 0xDB) { o += (char)0xDB; o += (char)0xDD; }
            else o += (char)c;
        }
        o += (char)0xC0;
    }
    return o;
}

} // namespace cubesat
} // namespace dect2
