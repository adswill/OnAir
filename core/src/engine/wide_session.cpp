// Wide multi-signal decode: see wide_session.h.
#include "dect2/engine/wide_session.h"
#include "dect2/autodet/autodet.h"
#include "dect2/channels/freq_db.h"
#include "dect2/channels/tvchan.h"
#include "dect2/dsp/carrierfind.h"
#include "dect2/engine/channelizer.h"
#include "dect2/engine/multitv.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace dect2 {

const char* wsKindName(WsKind k) {
    static const char* const n[] = {"unknown", "DMR", "P25", "NXDN", "dPMR", "D-STAR", "Fusion", "M17", "Pager", "APRS", "AM", "NFM", "DVB-T", "DVB-T2", "no decoder"};
    return (int)k >= 0 && (int)k < (int)WsKind::Count ? n[(int)k] : "unknown";
}
int wsKindStdMode(WsKind k) {
    switch (k) {
    case WsKind::Dmr: return 10;
    case WsKind::P25: return 51;
    case WsKind::Nxdn: return 39;
    case WsKind::Dpmr: return 46;
    case WsKind::Dstar: return 34;
    case WsKind::Fusion: return 129;
    case WsKind::M17: return 32;
    case WsKind::Pager: return 24;
    case WsKind::Packet: return 25;
    default: return 0;
    }
}
const char* wsStateName(WsState s) {
    static const char* const n[] = {"identifying", "running", "off", "dropped (CPU)", "no decoder", "unidentified"};
    return (int)s >= 0 && (int)s <= 5 ? n[(int)s] : "";
}

namespace {

constexpr double kIdRate = 96000.0;
constexpr double kTickSec = 0.25;
constexpr int kTvIdBase = 1000000;
constexpr int kMaxIdentifying = 6;
constexpr double kKnownTolHz = 7000.0;

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool bankKind(WsKind k) { return wsKindStdMode(k) != 0; }
bool vfoKind(WsKind k) { return k == WsKind::Am || k == WsKind::Nfm; }

WsKind kindOfDbMode(const std::string& m) {
    if (m == "dmr") return WsKind::Dmr;
    if (m == "p25") return WsKind::P25;
    if (m == "nxdn") return WsKind::Nxdn;
    if (m == "dpmr") return WsKind::Dpmr;
    if (m == "dstar") return WsKind::Dstar;
    if (m == "m17") return WsKind::M17;
    if (m == "fusion" || m == "ysf") return WsKind::Fusion;
    if (m == "pocsag" || m == "flex") return WsKind::Pager;
    if (m == "packet") return WsKind::Packet;
    return WsKind::Unknown;
}

struct DbCh { double hz; WsKind kind; std::string mode; };
const std::vector<DbCh>& dbChannels() {
    static const std::vector<DbCh> v = [] {
        std::vector<DbCh> r;
        for (const FreqEntry& e : builtinFrequencies()) { const WsKind k = kindOfDbMode(e.mode); if (k != WsKind::Unknown) r.push_back({(double)e.hz, k, e.mode}); }
        std::sort(r.begin(), r.end(), [](const DbCh& a, const DbCh& b) { return a.hz < b.hz; });
        return r;
    }();
    return v;
}

WsKind kindOfVoice(int sys) {
    switch (sys) {
    case autodet::VoiceDmr: return WsKind::Dmr;
    case autodet::VoiceP25: case autodet::VoiceP25p2: return WsKind::P25;
    case autodet::VoiceNxdn48: case autodet::VoiceNxdn96: return WsKind::Nxdn;
    case autodet::VoiceDstar: return WsKind::Dstar;
    case autodet::VoiceDpmr: return WsKind::Dpmr;
    case autodet::VoiceYsf: return WsKind::Fusion;
    case autodet::VoiceM17: return WsKind::M17;
    default: return WsKind::Unknown;
    }
}

// Low-pass with a steep Blackman-windowed sinc (cut in Hz), in place
void sharpen(std::vector<cf32>& x, double rate, double cutHz) {
    const int taps = 241;
    std::vector<float> h((size_t)taps);
    const double fc = std::min(0.499, cutHz / rate);
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double k = i - (taps - 1) / 2.0;
        const double sc = k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(sc * w); sum += sc * w;
    }
    for (float& v : h) v = (float)(v / sum);
    std::vector<cf32> y(x.size());
    const size_t n = x.size();
    for (size_t i = (size_t)taps; i < n; i++) {
        float re = 0, im = 0;
        const cf32* p = &x[i - (size_t)taps + 1];
        for (int k = 0; k < taps; k++) { re += h[(size_t)(taps - 1 - k)] * p[k].real(); im += h[(size_t)(taps - 1 - k)] * p[k].imag(); }
        y[i] = cf32(re, im);
    }
    x.assign(y.begin() + taps, y.end());
}

struct Job {
    int id = 0;
    bool tv = false;
    std::vector<cf32> iq;
    double rate = 0, rfHz = 0;
    double cutHz = 0;   // TV: low-pass the slot before looking (an adjacent channel leaks into the edge of the slot's filter)
};
struct JobResult {
    int id = 0;
    bool tv = false;
    autodet::VoiceSignal voice;
    autodet::TvResult tvr;
};

} // namespace

struct WideSession::Impl {
    // ---- the analysis thread's own state (configure / feed / reset)
    WsConfig cfg;
    double center = 0, clock = 0, nextTick = 0;
    CarrierFinder finder;
    WideModes wm;
    VfoBank vfo;
    SpanScanner scan;
    std::unique_ptr<MultiTv> mtv[2];   // [0] DVB-T2, [1] DVB-T
    bool wmConfigured[200] = {};
    bool vfoReady = false;
    bool tvPossible = false;
    std::set<int> modesFollowed;
    std::map<int, std::vector<double>> followNow;
    std::vector<VfoConfig> vfoNow;
    std::vector<int> allowedNow[2];
    bool allowedSet[2] = {false, false};

    struct N {
        WsEntry e;
        int fid = 0;
        Channelizer idCh;
        bool idOk = false;
        std::vector<cf32> idBuf;
        int attempts = 0;
        double nextTry = 0;
        bool pending = false;
        double vfoOff = 1e18;
    };
    std::vector<std::unique_ptr<N>> nar;
    struct T {
        int index = 0;
        WsEntry e;
        ChannelFilter cf;
        bool cfOk = false;
        std::vector<cf32> buf;
        int attempts = 0;
        double nextTry = 0;
        bool pending = false;
        double lastSeen = 0;
        bool occ = false;
    };
    std::vector<std::unique_ptr<T>> tvs;
    double tvWidthMhz = 0, tvGrid = 0;
    std::vector<ChannelSlotPos> tvSlots;
    double tvEmptySince = -1;

    std::set<int> dropped;
    double lastChange = -100;
    uint64_t dropEvents = 0;
    int listen = 0;

    // load measurement: a window of data time
    double winWork = 0, winData = 0, loadAnalysis = 0;
    double winBank[200] = {};
    double bankLoad[200] = {};
    double winVfo = 0, vfoLoad = 0;
    double tvLoad = 0;
    double loadMeasured = 0;
    uint64_t rowSeq[200] = {};
    std::map<int, std::vector<WideCarrier>> bankRows;
    std::set<int> tvNames;

    // ---- shared with other threads
    mutable std::mutex ctlMu;
    struct Ctl { bool enabled = true, pinned = false; int priority = 0; WsKind forced = WsKind::Unknown; };
    std::map<int, Ctl> ctl;
    double budget = 0.8, loadOverride = -1;
    std::vector<WsKnown> known;
    bool useDb = true;
    mutable std::mutex snapMu;
    std::vector<WsEntry> snap;
    WsStatus snapSt;
    std::atomic<int> identStarted{0};

    // ---- the identifier's thread
    std::mutex jobMu;
    std::condition_variable jobCv, idleCv;
    std::vector<Job> jobs;
    std::vector<JobResult> results;
    bool busy = false, quit = false;
    std::thread worker;

    Impl() {
        worker = std::thread([this] { run(); });
        WideTuning t;
        t.cpuBudget = 0;   // the session has one guard for everything
        wm.setTuning(t);
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(jobMu); quit = true; }
        jobCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(jobMu);
                jobCv.wait(lk, [&] { return quit || !jobs.empty(); });
                if (quit) return;
                j = std::move(jobs.front()); jobs.erase(jobs.begin());
                busy = true;
            }
            JobResult r; r.id = j.id; r.tv = j.tv;
            if (j.tv && j.cutHz > 0) sharpen(j.iq, j.rate, j.cutHz);
            if (j.tv) { autodet::TvOptions o; o.rfHz = j.rfHz; r.tvr = autodet::detectTv(j.iq.data(), j.iq.size(), j.rate, o); }
            else r.voice = autodet::classifyVoice(j.iq.data(), j.iq.size(), j.rate);
            {
                std::lock_guard<std::mutex> lk(jobMu);
                results.push_back(std::move(r));
                busy = false;
            }
            idleCv.notify_all();
        }
    }
    void post(Job&& j) {
        { std::lock_guard<std::mutex> lk(jobMu); jobs.push_back(std::move(j)); }
        jobCv.notify_one();
        identStarted++;
    }
    bool jobsIdle() {
        std::lock_guard<std::mutex> lk(jobMu);
        return jobs.empty() && !busy;
    }

    Ctl ctlOf(int id) const { std::lock_guard<std::mutex> lk(ctlMu); auto it = ctl.find(id); return it == ctl.end() ? Ctl() : it->second; }

    // ------------------------------------------------------------------------------------------------ setup
    void configure(const WsConfig& c) {
        cfg = c; center = c.centerHz;
        { std::lock_guard<std::mutex> lk(ctlMu); budget = c.cpuBudget; }
        clearState();
        if (c.spanRate <= 0) return;
        CarrierCfg fc;
        fc.stepHz = 12500; fc.maxCarriers = std::max(1, c.maxNarrow); fc.rejectTones = !(c.airband && c.centerHz > 0 && c.centerHz + c.spanRate / 2 >= 118e6 && c.centerHz - c.spanRate / 2 <= 137e6);   // an air-band carrier with nobody talking is a line, and it is the squelch break fc.holdSec = 20; fc.debounceSec = 0.3;
        finder.configure(c.spanRate, fc);
        vfo.configure(c.spanRate);
        vfo.setSink([this](const float* s, size_t n) { std::lock_guard<std::mutex> lk(sinkMu); if (sink) sink(s, n); });
        wm.setCenterHz(center);
        tvPossible = c.tv && c.spanRate >= 9e6;
        scan.configure(c.spanRate);
        scan.reset();
    }
    std::mutex sinkMu;
    VfoBank::Sink sink;

    void clearState() {
        nar.clear(); tvs.clear(); tvSlots.clear(); tvWidthMhz = 0; tvGrid = 0; tvEmptySince = -1;
        dropped.clear(); followNow.clear(); vfoNow.clear();
        for (int i = 0; i < 2; i++) { mtv[i].reset(); allowedNow[i].clear(); allowedSet[i] = false; }
        for (auto& m : followNow) (void)m;
        for (int m : modesFollowed) wm.setFollowOnlyFor(m, true, {});
        modesFollowed.clear();
        vfo.setChannels({});
        clock = 0; nextTick = kTickSec; winWork = winData = winVfo = 0; loadAnalysis = vfoLoad = tvLoad = loadMeasured = 0;
        for (int i = 0; i < 200; i++) { winBank[i] = bankLoad[i] = 0; rowSeq[i] = 0; wmConfigured[i] = false; }
        bankRows.clear(); lastChange = -100;
        std::lock_guard<std::mutex> lk(snapMu); snap.clear(); snapSt = WsStatus();
    }

    // ------------------------------------------------------------------------------------------------ identification
    WsKind knownKind(double absHz, std::string& why) const {
        std::vector<WsKnown> k;
        bool db;
        { std::lock_guard<std::mutex> lk(ctlMu); k = known; db = useDb; }
        double best = kKnownTolHz + 1; WsKind bk = WsKind::Unknown;
        for (const WsKnown& w : k) { const double d = std::fabs(w.absHz - absHz); if (d < best) { best = d; bk = w.kind; why = "known channel"; } }
        if (bk != WsKind::Unknown) return bk;
        if (db && center > 0) {
            const auto& v = dbChannels();
            auto it = std::lower_bound(v.begin(), v.end(), absHz - kKnownTolHz, [](const DbCh& a, double h) { return a.hz < h; });
            for (; it != v.end() && it->hz <= absHz + kKnownTolHz; ++it) {
                const double d = std::fabs(it->hz - absHz);
                if (d < best) { best = d; bk = it->kind; why = "known channel: " + it->mode; }
            }
        }
        return bk;
    }
    static bool airBand(double absHz) { return absHz >= 118e6 && absHz <= 137e6; }

    void applyVoice(N& n, const autodet::VoiceSignal& v) {
        const double abs = center + n.e.offsetHz;
        WsKind k = kindOfVoice(v.sys);
        const bool sure = v.verdict >= autodet::VerdictLikely;
        if (k != WsKind::Unknown && sure) { n.e.kind = k; n.e.identity = std::string(autodet::voiceSysName(v.sys)) + " (" + autodet::verdictName(v.verdict) + ")"; return; }
        if (v.sys == autodet::VoiceTetra && sure) { n.e.kind = WsKind::NoDecoder; n.e.identity = "TETRA"; n.e.why = "TETRA is identified; the wide session has no decoder for it (use the TETRA mode on this channel)"; return; }
        std::string why;
        const WsKind kn = knownKind(abs, why);
        if (kn != WsKind::Unknown) { n.e.kind = kn; n.e.identity = why; return; }
        if (v.sys == autodet::VoiceAnalogFm) {
            n.e.kind = cfg.airband && center > 0 && airBand(abs) ? WsKind::Am : WsKind::Nfm;
            n.e.identity = n.e.kind == WsKind::Am ? "analogue voice, air band (AM)" : "analogue FM";
            return;
        }
        if (k != WsKind::Unknown) { n.e.kind = k; n.e.identity = std::string(autodet::voiceSysName(v.sys)) + " (" + autodet::verdictName(v.verdict) + ")"; return; }
        if (cfg.airband && center > 0 && airBand(abs)) { n.e.kind = WsKind::Am; n.e.identity = "air band (AM)"; return; }
        if (++n.attempts >= cfg.identAttempts) { n.e.state = WsState::Unidentified; n.e.why = "could not be identified; give it a kind by hand"; }
        else n.nextTry = clock + 2.0;
    }
    void applyTv(T& t, const autodet::TvResult& r) {
        if (r.std == autodet::TvDvbt2) { t.e.kind = WsKind::DvbT2; t.e.identity = r.headline; }
        else if (r.std == autodet::TvDvbt) { t.e.kind = WsKind::DvbT; t.e.identity = r.headline; }
        else if (r.signal && r.std != autodet::TvUnknown) {
            t.e.kind = WsKind::NoDecoder; t.e.identity = r.headline;
            t.e.why = std::string(autodet::tvStdName(r.std)) + " is identified; the multi-channel TV decodes DVB-T and DVB-T2 only";
        } else if (++t.attempts >= cfg.identAttempts) { t.e.state = WsState::Unidentified; t.e.identity = r.headline; t.e.why = "the TV standard could not be identified" + (r.headline.empty() ? std::string() : ": " + r.headline); }
        else t.nextTry = clock + 1.0;
    }
    void takeResults() {
        std::vector<JobResult> rs;
        { std::lock_guard<std::mutex> lk(jobMu); rs.swap(results); }
        for (JobResult& r : rs) {
            if (r.tv) { for (auto& t : tvs) if (t->e.id == r.id) { t->pending = false; t->buf.clear(); applyTv(*t, r.tvr); } }
            else for (auto& n : nar) if (n->e.id == r.id) { n->pending = false; n->idBuf.clear(); applyVoice(*n, r.voice); }
        }
    }

    // ------------------------------------------------------------------------------------------------ the entries
    double rankOf(const WsEntry& e) const {
        double b = 30;
        if (wsIsTv(e.kind)) b = 60; else if (bankKind(e.kind) && e.kind != WsKind::Pager && e.kind != WsKind::Packet) b = 50; else if (e.kind == WsKind::Pager || e.kind == WsKind::Packet) b = 40;
        b += (e.locked ? 20 : 0) + (e.active ? 10 : 0) + std::max(0.0, std::min(30.0, e.snrDb)) / 3.0 + 10.0 * e.priority;
        if (e.pinned || e.id == listen) b += 1000;
        return b;
    }
    void applyCtl(WsEntry& e, bool& forced, WsKind& fk) {
        const Ctl c = ctlOf(e.id);
        e.enabled = c.enabled; e.pinned = c.pinned; e.priority = c.priority;
        forced = c.forced != WsKind::Unknown; fk = c.forced;
    }
    // the state an entry should have given its kind, switches and the CPU guard
    void settleState(WsEntry& e) {
        if (e.state == WsState::Unidentified && e.kind == WsKind::Unknown) return;
        if (e.kind == WsKind::Unknown) { e.state = WsState::Identifying; return; }
        if (e.kind == WsKind::NoDecoder) { e.state = WsState::NoDecoder; return; }
        if (!e.enabled) { e.state = WsState::Off; e.why = "switched off"; return; }
        if (dropped.count(e.id)) { e.state = WsState::Dropped; e.why = "dropped: the CPU budget was exceeded and this had the lowest rank"; return; }
        e.state = WsState::Running; e.why.clear();
    }

    void syncNarrow(const std::vector<std::pair<double, double>>& zones) {
        for (int id : finder.takeRemoved()) {
            for (size_t i = 0; i < nar.size(); i++) if (nar[i]->fid == id) { dropped.erase(nar[i]->e.id); nar.erase(nar.begin() + (long)i); break; }
        }
        for (const Carrier& c : finder.carriers()) {
            bool inZone = false;
            for (const auto& z : zones) if (c.hz >= z.first && c.hz <= z.second) inZone = true;
            N* n = nullptr;
            for (auto& q : nar) if (q->fid == c.id) n = q.get();
            if (inZone) { if (n) { for (size_t i = 0; i < nar.size(); i++) if (nar[i].get() == n) { dropped.erase(n->e.id); nar.erase(nar.begin() + (long)i); break; } } continue; }
            if (!n) {
                nar.emplace_back(new N());
                n = nar.back().get();
                n->fid = c.id; n->e.id = c.id; n->nextTry = clock;
            }
            n->e.offsetHz = c.hz; n->e.absHz = center + c.hz; n->e.bandwidthHz = c.widthHz; n->e.levelDb = c.levelDb; n->e.snrDb = c.snrDb; n->e.active = c.active;
            if (n->idOk) n->idCh.setShift(c.hz);
        }
    }

    void syncKinds() {
        for (auto& n : nar) {
            bool forced; WsKind fk;
            applyCtl(n->e, forced, fk);
            if (forced) { n->e.kind = fk; n->e.identity = std::string("set by hand: ") + wsKindName(fk); n->e.state = WsState::Identifying; }
            else if (n->e.identity.rfind("set by hand", 0) == 0) { n->e.kind = WsKind::Unknown; n->e.identity.clear(); n->attempts = 0; n->nextTry = clock; }
            if (n->e.state == WsState::Unidentified && !forced && n->e.kind == WsKind::Unknown) {}
            else settleState(n->e);
            n->e.rank = rankOf(n->e);
        }
        for (auto& t : tvs) {
            bool forced; WsKind fk;
            applyCtl(t->e, forced, fk);
            settleState(t->e);
            t->e.rank = rankOf(t->e);
        }
    }

    // ------------------------------------------------------------------------------------------------ the TV side
    std::vector<std::pair<double, double>> zones() const {
        std::vector<std::pair<double, double>> z;
        for (auto& t : tvs) if (t->occ) z.push_back({t->e.offsetHz - tvWidthMhz * 0.5e6, t->e.offsetHz + tvWidthMhz * 0.5e6});
        return z;
    }
    void tvScanTick() {
        if (!tvPossible || scan.frames() < 12) return;
        if (tvSlots.empty()) {
            double best = 0, bw = 0, bg = 0;
            for (double w : cfg.tvWidthsMhz) for (double off : {0.0, 0.5 * w * 1e6}) {
                const auto sl = spanSlots(cfg.spanRate, w * 1e6, off);
                if (sl.empty()) continue;
                const auto m = scan.measure(sl, w * 1e6, 6.0);
                double score = 0;
                for (const auto& p : m) if (p.occupied) score += 1000.0 + std::min(p.aboveDb, 60.0);
                if (score > best) { best = score; bw = w; bg = off; }
            }
            if (best <= 0) return;
            tvWidthMhz = bw; tvGrid = bg; tvSlots = spanSlots(cfg.spanRate, bw * 1e6, bg);
        }
        const auto m = scan.measure(tvSlots, tvWidthMhz * 1e6, 6.0);
        bool any = false;
        for (const auto& p : m) {
            T* t = nullptr;
            for (auto& q : tvs) if (q->index == p.pos.index) t = q.get();
            if (p.occupied) {
                any = true;
                if (!t) {
                    tvs.emplace_back(new T());
                    t = tvs.back().get();
                    t->index = p.pos.index; t->e.id = kTvIdBase + 1000 + p.pos.index; t->nextTry = clock;
                }
                t->lastSeen = clock; t->occ = true;
            } else if (t) t->occ = clock - t->lastSeen < 12.0;
            if (t) { t->e.offsetHz = p.pos.offsetHz; t->e.absHz = center + p.pos.offsetHz; t->e.bandwidthHz = tvWidthMhz * 1e6; t->e.levelDb = p.levelDb; t->e.snrDb = p.aboveDb; t->e.active = p.occupied; }
        }
        for (size_t i = 0; i < tvs.size();) {
            if (!tvs[i]->occ) { dropped.erase(tvs[i]->e.id); tvs.erase(tvs.begin() + (long)i); } else i++;
        }
        if (any) tvEmptySince = -1;
        else {
            if (tvEmptySince < 0) tvEmptySince = clock;
            if (clock - tvEmptySince > 30) { tvSlots.clear(); tvWidthMhz = 0; for (int i = 0; i < 2; i++) { mtv[i].reset(); allowedSet[i] = false; allowedNow[i].clear(); } }
        }
    }
    void tvPlace() {
        for (int g = 0; g < 2; g++) {
            const WsKind want = g == 0 ? WsKind::DvbT2 : WsKind::DvbT;
            std::vector<int> allow;
            bool have = false;
            for (auto& t : tvs) if (t->e.kind == want) { have = true; if (t->e.state == WsState::Running) allow.push_back(t->index); }
            if (!have) continue;
            if (!mtv[g]) {
                mtv[g].reset(new MultiTv());
                MultiTvOptions o; o.dvbt = g == 1; o.spanRate = cfg.spanRate; o.widthMhz = tvWidthMhz; o.gridOffsetHz = tvGrid; o.autoGrid = false;
                o.maxChannels = 32; o.blocking = cfg.blockingTv;
                if (!mtv[g]->configure(o)) { mtv[g].reset(); continue; }
                mtv[g]->setAllowedSlots({});
                allowedSet[g] = true; allowedNow[g].clear();
            }
            std::sort(allow.begin(), allow.end());
            if (!allowedSet[g] || allow != allowedNow[g]) { mtv[g]->setAllowedSlots(allow); allowedNow[g] = allow; allowedSet[g] = true; if (allow.empty()) mtv[g]->reset(); }   // nothing allowed: no feed reaches it, so it is stopped here
        }
    }
    void tvRows() {
        tvLoad = 0;
        for (int g = 0; g < 2; g++) {
            if (!mtv[g]) continue;
            const WsKind want = g == 0 ? WsKind::DvbT2 : WsKind::DvbT;
            std::vector<TvChannelRow> rows = mtv[g]->channels();
            RxTelemetry dump; mtv[g]->poll(dump);
            for (auto& t : tvs) {
                if (t->e.kind != want) continue;
                for (const TvChannelRow& r : rows) if (r.slot == t->index) {
                    t->e.locked = r.locked; t->e.items = (uint64_t)r.services;
                    t->e.cost = r.cpuPct / 100.0;
                    if (r.running) tvLoad += r.cpuPct / 100.0;
                    char b[96]; snprintf(b, sizeof b, "%d services%s%s", r.services, r.network.empty() ? "" : ", ", r.network.c_str());
                    t->e.info = r.running ? b : "not running";
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------------ the decoders of the narrow kinds
    void bankPlace() {
        std::map<int, std::vector<double>> want;
        std::vector<VfoConfig> vc;
        for (auto& n : nar) {
            if (n->e.state != WsState::Running) continue;
            const int m = wsKindStdMode(n->e.kind);
            if (m) want[m].push_back(std::round(n->e.absHz * 0.1) * 10.0);
            else if (vfoKind(n->e.kind)) {
                if (std::fabs(n->e.offsetHz - n->vfoOff) > 1500) n->vfoOff = n->e.offsetHz;
                VfoConfig c; c.id = n->e.id; c.offsetHz = n->vfoOff; c.mode = n->e.kind == WsKind::Am ? VfoMode::Am : VfoMode::Nfm;
                c.squelchDb = n->e.levelDb - 8;
                vc.push_back(c);
            }
        }
        for (auto& [m, v] : want) {
            std::sort(v.begin(), v.end());
            if (!wmConfigured[m]) { wm.configure(m, cfg.spanRate, 0); wmConfigured[m] = true; }
            auto it = followNow.find(m);
            if (it == followNow.end() || it->second != v) { wm.setFollowOnlyFor(m, true, v); followNow[m] = v; }
            modesFollowed.insert(m);
        }
        for (auto it = followNow.begin(); it != followNow.end();) {
            if (!want.count(it->first)) { wm.setFollowOnlyFor(it->first, true, {}); it = followNow.erase(it); }
            else ++it;
        }
        std::sort(vc.begin(), vc.end(), [](const VfoConfig& a, const VfoConfig& b) { return a.id < b.id; });
        bool same = vc.size() == vfoNow.size();
        for (size_t i = 0; same && i < vc.size(); i++) same = vc[i].id == vfoNow[i].id && vc[i].offsetHz == vfoNow[i].offsetHz && vc[i].squelchDb == vfoNow[i].squelchDb && vc[i].mode == vfoNow[i].mode;
        if (!same) { vfo.setChannels(vc); vfoNow = vc; }
    }
    void bankRowsMerge() {
        for (auto& [m, v] : followNow) {
            if (v.empty()) continue;
            RxTelemetry t; uint64_t seq = rowSeq[m];
            if (wm.telemetry(m, t, seq)) { rowSeq[m] = seq; bankRows[m] = t.wide; }
        }
        const auto vst = vfo.status();
        for (auto& n : nar) {
            const int m = wsKindStdMode(n->e.kind);
            n->e.locked = false;
            if (m && n->e.state == WsState::Running) {
                const auto it = bankRows.find(m);
                if (it != bankRows.end()) {
                    const WideCarrier* best = nullptr; double bd = 1e18;
                    for (const WideCarrier& r : it->second) { const double d = std::fabs(r.hz - n->e.offsetHz); if (d < bd) { bd = d; best = &r; } }
                    if (best && bd < 15000) { n->e.locked = best->state >= 2; n->e.info = best->info.empty() ? (best->state >= 2 ? "locked" : "searching") : best->info; }
                    else n->e.info = "starting";
                }
                const int cnt = (int)followNow[m].size();
                n->e.cost = cnt ? bankLoad[m] / cnt : 0;
            } else if (vfoKind(n->e.kind) && n->e.state == WsState::Running) {
                for (const VfoStatus& s : vst) if (s.id == n->e.id) { n->e.locked = s.open; n->e.info = s.open ? "open" : "squelched"; }
                n->e.cost = vfoNow.empty() ? 0 : vfoLoad / (double)vfoNow.size();
            } else if (n->e.state == WsState::Off || n->e.state == WsState::Dropped) n->e.info.clear();
        }
    }

    // ------------------------------------------------------------------------------------------------ the CPU guard
    double loadNow() const {
        double ov;
        { std::lock_guard<std::mutex> lk(ctlMu); ov = loadOverride; }
        return ov >= 0 ? ov : loadMeasured;
    }
    void guard() {
        double b;
        { std::lock_guard<std::mutex> lk(ctlMu); b = budget; }
        if (b <= 0) { dropped.clear(); return; }
        const double load = loadNow();
        std::vector<WsEntry*> all;
        for (auto& n : nar) all.push_back(&n->e);
        for (auto& t : tvs) all.push_back(&t->e);
        if (load > b && clock - lastChange >= 1.5) {
            WsEntry* v = nullptr;
            for (WsEntry* e : all) {
                if (e->state != WsState::Running || e->pinned || e->id == listen) continue;
                if (!v || e->rank < v->rank) v = e;
            }
            if (v) { dropped.insert(v->id); v->state = WsState::Dropped; lastChange = clock; dropEvents++; }
        } else if (load < 0.5 * b && !dropped.empty() && clock - lastChange >= 5.0) {
            WsEntry* best = nullptr;
            for (WsEntry* e : all) if (dropped.count(e->id) && e->enabled && (!best || e->rank > best->rank)) best = e;
            if (best && load + best->cost < 0.8 * b) { dropped.erase(best->id); lastChange = clock; }
            else if (!best) dropped.clear();
        }
    }

    // ------------------------------------------------------------------------------------------------ feeding
    void feedIdent(const cf32* x, size_t n) {
        int identifying = 0;
        for (auto& q : nar) {
            N& a = *q;
            if (a.e.kind != WsKind::Unknown || a.e.state == WsState::Unidentified || a.pending) continue;
            if (!a.e.active || clock < a.nextTry) continue;
            if (identifying >= kMaxIdentifying) break;
            identifying++;
            if (!a.idOk) { a.idOk = a.idCh.configure(cfg.spanRate, kIdRate, a.e.offsetHz, 22000); if (!a.idOk) { a.e.state = WsState::Unidentified; a.e.why = "the capture rate is too low to identify"; continue; } }
            std::vector<cf32> tmp;
            a.idCh.process(x, n, tmp);
            a.idBuf.insert(a.idBuf.end(), tmp.begin(), tmp.end());
            if (a.idBuf.size() >= (size_t)(cfg.identSec * kIdRate)) {
                Job j; j.id = a.e.id; j.iq = std::move(a.idBuf); a.idBuf.clear(); j.rate = kIdRate; j.rfHz = center + a.e.offsetHz;
                a.pending = true;
                post(std::move(j));
            }
        }
        for (auto& q : tvs) {
            T& t = *q;
            if (t.e.kind != WsKind::Unknown || t.e.state == WsState::Unidentified || t.pending || clock < t.nextTry || !t.occ) continue;
            if (!t.cfOk) { t.cfOk = t.cf.configure(cfg.spanRate, t.e.offsetHz, tvChannelRate(tvWidthMhz, cfg.spanRate)); if (!t.cfOk) continue; }
            std::vector<cf32> tmp;
            t.cf.process(x, n, tmp);
            t.buf.insert(t.buf.end(), tmp.begin(), tmp.end());
            const double r = t.cf.outRate();
            if (t.buf.size() >= (size_t)(autodet::tvCaptureSec() * (t.attempts ? 2.0 : 1.15) * r)) {   // a retry looks twice as long
                Job j; j.id = t.e.id; j.tv = true; j.iq = std::move(t.buf); t.buf.clear(); j.rate = r; j.cutHz = std::max(0.5e6, tvWidthMhz * 0.5e6 - 0.05e6); j.rfHz = center + t.e.offsetHz;
                t.pending = true;
                post(std::move(j));
            }
        }
    }

    void tick() {
        takeResults();
        if (tvPossible) tvScanTick();
        const auto z = zones();
        finder.setExclusions(z);
        if (cfg.narrow) syncNarrow(z);
        syncKinds();
        if (clock >= 1.0) guard();   // wait for the first measurement window
        syncKinds();
        bankPlace();
        tvPlace();
        bankRowsMerge();
        tvRows();
        syncKinds();
        publish();
    }

    void publish() {
        std::vector<WsEntry> v;
        for (auto& n : nar) v.push_back(n->e);
        for (auto& t : tvs) v.push_back(t->e);
        std::sort(v.begin(), v.end(), [](const WsEntry& a, const WsEntry& b) { return a.rank > b.rank; });
        WsStatus st;
        st.load = loadNow(); { std::lock_guard<std::mutex> lk(ctlMu); st.budget = budget; }
        st.droppedForCpu = (int)dropped.size(); st.tooSlow = !dropped.empty(); st.dropEvents = dropEvents;
        for (const WsEntry& e : v) st.running += e.state == WsState::Running;
        st.noiseDb = finder.noiseDb(); st.tvPresent = !tvs.empty(); st.tvWidthMhz = tvWidthMhz;
        std::lock_guard<std::mutex> lk(snapMu);
        snap = std::move(v); snapSt = st;
    }

    void feed(const cf32* x, size_t n) {
        if (!n || cfg.spanRate <= 0) return;
        const double t0 = nowSec();
        const double dt = (double)n / cfg.spanRate;
        clock += dt;
        if (tvPossible) scan.feed(x, n);
        if (cfg.narrow) finder.feed(x, n);
        feedIdent(x, n);
        for (auto& [m, v] : followNow) if (!v.empty()) {
            const double b0 = nowSec();
            wm.feed(m, x, n);
            winBank[m] += nowSec() - b0;
        }
        if (!vfoNow.empty()) { const double b0 = nowSec(); vfo.feed(x, n); winVfo += nowSec() - b0; }
        for (int g = 0; g < 2; g++) if (mtv[g] && !allowedNow[g].empty()) mtv[g]->feed(x, n);
        winWork += nowSec() - t0; winData += dt;
        if (winData >= 1.0) {
            loadAnalysis = winWork / winData; vfoLoad = winVfo / winData;
            for (int i = 0; i < 200; i++) { bankLoad[i] = winBank[i] / winData; winBank[i] = 0; }
            loadMeasured = loadAnalysis + tvLoad;
            winWork = winData = winVfo = 0;
        }
        if (clock >= nextTick) { nextTick = clock + kTickSec; tick(); }
    }
};

WideSession::WideSession() : p_(new Impl) {}
WideSession::~WideSession() = default;

void WideSession::configure(const WsConfig& c) { p_->configure(c); }
void WideSession::reset() { WsConfig c = p_->cfg; p_->finder.reset(); p_->wm.reset(); p_->vfo.reset(); p_->clearState(); if (c.spanRate > 0) p_->configure(c); }
void WideSession::setCenterHz(double hz) { p_->center = hz; p_->cfg.centerHz = hz; p_->wm.setCenterHz(hz); }
void WideSession::feed(const cf32* x, size_t n) { p_->feed(x, n); }
void WideSession::wait() {
    for (int i = 0; i < 4000 && !p_->jobsIdle(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int g = 0; g < 2; g++) if (p_->mtv[g]) p_->mtv[g]->wait();
}
std::vector<WsEntry> WideSession::entries() const { std::lock_guard<std::mutex> lk(p_->snapMu); return p_->snap; }
WsStatus WideSession::status() const { std::lock_guard<std::mutex> lk(p_->snapMu); return p_->snapSt; }

void WideSession::setEnabled(int id, bool on) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->ctl[id].enabled = on; }
void WideSession::setPinned(int id, bool on) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->ctl[id].pinned = on; }
void WideSession::setPriority(int id, int adj) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->ctl[id].priority = adj; }
void WideSession::setKind(int id, WsKind k) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->ctl[id].forced = k; }
void WideSession::setListen(int id) { p_->listen = id; }
void WideSession::setBudget(double frac) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->budget = std::max(0.0, frac); }
void WideSession::setLoadOverride(double v) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->loadOverride = v; }
void WideSession::setKnownChannels(const std::vector<WsKnown>& k) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->known = k; }
void WideSession::useFrequencyDatabase(bool on) { std::lock_guard<std::mutex> lk(p_->ctlMu); p_->useDb = on; }

WideModes& WideSession::modes() { return p_->wm; }
MultiTv* WideSession::tv(bool dvbt) { return p_->mtv[dvbt ? 1 : 0].get(); }
void WideSession::setAudioSink(VfoBank::Sink s) { std::lock_guard<std::mutex> lk(p_->sinkMu); p_->sink = std::move(s); }
bool WideSession::vfoStatusOf(int id, VfoStatus& s) const {
    for (const VfoStatus& v : p_->vfo.status()) if (v.id == id) { s = v; return true; }
    return false;
}
int WideSession::identCount() const { return p_->identStarted.load(); }

} // namespace dect2
