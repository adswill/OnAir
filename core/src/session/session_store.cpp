#include "dect2/session/session_store.h"
#include "dect2/common/thread_name.h"
#include "dect2/session/retention.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dect2::sess {

namespace fs = std::filesystem;
namespace au = automation;

namespace {

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int64_t steadyNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double steadySec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

fs::path pathOf(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string strOf(const fs::path& p) { const auto u = p.u8string(); return std::string(reinterpret_cast<const char*>(u.c_str()), u.size()); }

// the writer and reader threads run below the decoders
void lowPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

constexpr int kGens = 256;
constexpr uint16_t kStarFlag = FlagStarred;

struct Op {
    enum T : uint8_t { Begin, Update, End, Link, Star, Note, DeleteMode, DeleteAll } t = Update;
    Session s;                 // Begin / Update / End
    uint64_t id = 0;           // Link / Star / Note
    SessionLink link;
    std::string a, b;          // Note: note, tags
    bool on = false;           // Star
    uint16_t mode = 0;         // DeleteMode
};

bool isSnapshot(const Op& o) { return o.t == Op::Begin || o.t == Op::Update || o.t == Op::End; }

std::string isoUtc(int64_t ms) {
    if (ms <= 0) return {};
    const time_t t = (time_t)(ms / 1000);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[48];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, (int)(ms % 1000));
    return b;
}

std::string numStr(double v, int dec) {
    if (!std::isfinite(v)) return {};
    char b[48];
    snprintf(b, sizeof b, "%.*f", dec, v);
    return b;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}

}  // namespace

struct SessionStore::Impl {
    SessionStore* self = nullptr;
    StoreConfig cfg;
    SessionDb db;
    OpenInfo info;
    std::string dbPathStr;
    std::atomic<bool> open{false};
    int64_t startedMs = 0;

    // queue
    std::mutex qmu;
    std::condition_variable qcv, doneCv;
    std::deque<Op> q;
    bool stopReq = false, flushReq = false, paused = false;
    uint64_t flushDone = 0, flushIssued = 0;
    std::atomic<bool> pruneReq{false};
    std::thread writerTh, readerTh, cleanTh;

    // counters
    std::atomic<uint64_t> refusedLinks{0}, enqueued{0}, dropped{0}, applied{0}, commits{0}, dbErrors{0}, eventsEmitted{0}, eventsThrottled{0}, queriesRun{0};
    std::atomic<int64_t> maxEnqueueNs{0};

    IdAllocator ids;

    // settings
    mutable std::mutex setMu;
    std::map<uint16_t, bool> logging;
    bool loggingDefault = true;
    std::map<uint16_t, std::string> names;
    automation::Engine* eng = nullptr;
    int eventsPerSec = 50;
    DbLimits limits;
    RetentionPolicy retention;
    std::vector<std::string> audioRoots, iqRoots;

    // hot window
    mutable std::mutex hotMu;
    std::map<std::pair<uint16_t, uint16_t>, std::deque<Session>> hot;
    std::array<std::atomic<uint64_t>, kGens> gens;
    std::atomic<uint64_t> genAll{1};

    // reader thread
    std::mutex rqMu;
    std::condition_variable rqCv;
    std::deque<std::pair<Query, std::function<void(Page&&)>>> rq;
    bool rqStop = false;

    // cleaner thread
    std::mutex clMu;
    std::condition_variable clCv;
    bool clStop = false;

    // settings file
    std::string settingsPath;
    std::atomic<bool> settingsDirty{false};
    bool noticeAck = false;
    double nextSettingsSave = 0;

    // writer-thread state
    int txOps = 0;
    double txDeadline = 0;
    std::set<uint16_t> touchedSinceCommit;
    bool recovered = false;
    double nextRecoverTry = 0, nextHeartbeat = 0, nextPrune = 0;
    int64_t opsSincePrune = 0;
    struct Bucket { double tokens = 0, last = 0; };
    std::map<uint16_t, Bucket> buckets;

    Impl() { for (auto& g : gens) g.store(1); }

    // ------------------------------------------------------------ settings helpers
    bool loggingFor(uint16_t mode) {
        std::lock_guard<std::mutex> lk(setMu);
        auto it = logging.find(mode);
        return it == logging.end() ? loggingDefault : it->second;
    }
    std::string nameOf(uint16_t mode) {
        std::lock_guard<std::mutex> lk(setMu);
        auto it = names.find(mode);
        return it == names.end() ? "mode " + std::to_string(mode) : it->second;
    }
    void bump(uint16_t mode) {
        gens[mode % kGens].fetch_add(1, std::memory_order_relaxed);
        genAll.fetch_add(1, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------ the queue
    bool push(Op&& op) {
        std::unique_lock<std::mutex> lk(qmu);
        if (q.size() >= cfg.queueMax) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        const bool wasEmpty = q.empty();
        q.push_back(std::move(op));
        enqueued.fetch_add(1, std::memory_order_relaxed);
        lk.unlock();
        if (wasEmpty) qcv.notify_one();
        return true;
    }

    void timed(int64_t t0) {
        const int64_t d = steadyNs() - t0;
        int64_t m = maxEnqueueNs.load(std::memory_order_relaxed);
        while (d > m && !maxEnqueueNs.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
    }

    // ------------------------------------------------------------ privacy: raw ids to short tags (writer thread)
    void mask(Session& s) {
        Privacy& pv = privacy();
        if (!pv.masks(s.mode, s.hasPosition())) return;
        const std::string raw[3] = {s.who[0], s.who[1], s.key};
        const char* label = "id";
        std::string tag[3];
        for (int i = 0; i < 3; i++) tag[i] = raw[i].empty() ? raw[i] : pv.shortTag(s.mode, raw[i], label);
        for (int i = 0; i < 3; i++) {
            if (raw[i].size() >= 3) s.title = replaceAll(s.title, raw[i], tag[i]);   // shorter ones would hit unrelated text
        }
        s.who[0] = tag[0]; s.who[1] = tag[1]; s.key = tag[2];
        for (const std::string& a : {s.alias[0], s.alias[1]}) if (a.size() >= 3) s.title = replaceAll(s.title, a, "");
        s.alias[0].clear(); s.alias[1].clear();   // aliases are names tied to the ids
    }

    // ------------------------------------------------------------ hot window
    // true when the session was already ended in the window (a repeated end)
    bool hotApply(Session& s) {
        std::lock_guard<std::mutex> lk(hotMu);
        auto& d = hot[{s.mode, s.receiver}];
        bool wasEnded = false;
        for (auto it = d.begin(); it != d.end(); ++it) {
            if (it->id != s.id) continue;
            wasEnded = it->endWall != 0;
            if (it->endWall != 0 && s.endWall == 0) { s.endWall = it->endWall; s.durationMs = it->durationMs; }   // an update arriving after the end does not reopen it
            s.flags = (uint16_t)((s.flags & ~kStarFlag) | (it->flags & kStarFlag));
            *it = s;
            return wasEnded;
        }
        // newest first by start time (an update of a trimmed session comes back in its place)
        auto pos = std::find_if(d.begin(), d.end(), [&](const Session& x) { return x.startWall < s.startWall || (x.startWall == s.startWall && x.id < s.id); });
        d.insert(pos, s);
        while (d.size() > cfg.hotWindow) d.pop_back();
        return false;
    }

    void hotStar(uint64_t id, bool on) {
        std::lock_guard<std::mutex> lk(hotMu);
        const uint16_t mode = idMode(id);
        for (auto& kv : hot) {
            if (kv.first.first != mode) continue;
            for (Session& x : kv.second) if (x.id == id) x.flags = (uint16_t)(on ? (x.flags | kStarFlag) : (x.flags & ~kStarFlag));
        }
    }

    // ------------------------------------------------------------ events
    void emitEnded(const Session& s) {
        automation::Engine* e;
        int perSec;
        {
            std::lock_guard<std::mutex> lk(setMu);
            e = eng ? eng : &automation::global();
            perSec = eventsPerSec;
        }
        const Kind k = (Kind)s.kind;
        const int fam = au::FSession;
        if (!e->wants(fam)) return;
        // at most perSec per second per mode: a token bucket
        Bucket& b = buckets[s.mode];
        const double now = steadySec();
        if (b.last == 0) { b.tokens = perSec; b.last = now; }
        b.tokens = std::min<double>(perSec, b.tokens + (now - b.last) * perSec);
        b.last = now;
        if (b.tokens < 1.0) { eventsThrottled.fetch_add(1, std::memory_order_relaxed); return; }
        b.tokens -= 1.0;
        // ids and counts, the key and the title (the title carries no more than the adapter put in it; personal ids are tags already). Never an alias, tag, note or text.
        au::Event ev(kindEventType(k));
        ev.at(s.endWall / 1000.0, nameOf(s.mode), (double)s.freqHz);
        ev.str("session_id", std::to_string(s.id)).str("key", s.key).str("title", s.title).str("kind", kindName(k)).str("verdict", verdictName((Verdict)s.verdict));
        ev.num("mode_num", s.mode).num("receiver", s.receiver).num("duration_s", s.durationMs / 1000.0).num("events", s.eventCount);
        ev.flag("emergency", (s.flags & FlagEmergency) != 0);
        e->submit(std::move(ev));
        eventsEmitted.fetch_add(1, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------ database
    void ensureTx() {
        if (!db.isOpen()) return;
        if (!db.inTx()) { if (db.beginTx()) txDeadline = steadySec() + cfg.commitMs / 1000.0; else dbErrors++; }
    }

    void commit() {
        if (db.inTx()) { if (!db.commitTx()) dbErrors++; commits.fetch_add(1, std::memory_order_relaxed); }
        txOps = 0;
        for (uint16_t m : touchedSinceCommit) bump(m);
        touchedSinceCommit.clear();
    }

    void removeFiles(const std::vector<std::string>& refs, const std::vector<std::string>& roots) {
        for (const std::string& r : refs) {
            if (!insideRoots(r, roots)) continue;
            std::error_code ec;
            const fs::path p = pathOf(r);
            if (fs::is_regular_file(p, ec)) fs::remove(p, ec);
            fs::path j = p; j.replace_extension(".json");
            if (j != p) fs::remove(j, ec);
            if (strOf(p.extension()) == ".sigmf-data") { fs::path m = p; m.replace_extension(".sigmf-meta"); fs::remove(m, ec); }
        }
    }

    // true when the session is known to be encrypted or unreadable (in this batch, else in the window)
    bool contentRefused(uint64_t id, const std::unordered_map<uint64_t, Session>& last) {
        auto it = last.find(id);
        if (it != last.end()) return it->second.verdict == (uint8_t)Verdict::Encrypted || it->second.verdict == (uint8_t)Verdict::Unreadable;
        std::lock_guard<std::mutex> lk(hotMu);
        const uint16_t mode = idMode(id);
        for (const auto& kv : hot) {
            if (kv.first.first != mode) continue;
            for (const Session& x : kv.second) if (x.id == id) return x.verdict == (uint8_t)Verdict::Encrypted || x.verdict == (uint8_t)Verdict::Unreadable;
        }
        return false;
    }

    void applySegment(std::deque<Op>& batch, size_t from, size_t to) {
        const int64_t now = wallMs();
        // 1. every snapshot: protect, window, event; the database gets the last one of each session
        std::vector<uint64_t> order;
        std::unordered_map<uint64_t, Session> last;
        for (size_t i = from; i < to; i++) {
            Op& o = batch[i];
            if (!isSnapshot(o)) continue;
            Session s = std::move(o.s);
            if (o.t == Op::End && s.endWall == 0) s.endWall = now;
            mask(s);
            const bool wasEnded = hotApply(s);
            if (o.t == Op::End && !wasEnded) emitEnded(s);
            bump(s.mode);
            auto prev = last.find(s.id);
            if (prev == last.end()) order.push_back(s.id);
            else if (prev->second.endWall != 0 && s.endWall == 0) { s.endWall = prev->second.endWall; s.durationMs = prev->second.durationMs; }
            last[s.id] = std::move(s);
        }
        const bool haveDb = db.isOpen();
        if (haveDb) {
            for (uint64_t id : order) {
                const Session& s = last[id];
                if (!loggingFor(s.mode)) continue;
                ensureTx();
                // a session that already ended is not reopened by a late update
                if (!db.upsert(s, now)) dbErrors++;
                if (s.verdict == (uint8_t)Verdict::Encrypted || s.verdict == (uint8_t)Verdict::Unreadable) db.dropContentLinks(s.id);   // decided late: what was linked goes
                txOps++;
                touchedSinceCommit.insert(s.mode);
            }
        }
        // 2. the rest, in order
        for (size_t i = from; i < to; i++) {
            Op& o = batch[i];
            if (isSnapshot(o)) continue;
            const uint16_t mode = idMode(o.id);
            switch (o.t) {
            case Op::Link: {
                // an encrypted or unreadable session keeps metadata only: never audio, text, raw frames or IQ (docs/MODE-EXPERIENCE.md, 2.1 and 3.6)
                const LinkType lt = (LinkType)o.link.type;
                const bool content = lt == LinkType::Audio || lt == LinkType::Iq || lt == LinkType::Transcript || lt == LinkType::Message;
                if (content && contentRefused(o.id, last)) { refusedLinks.fetch_add(1, std::memory_order_relaxed); break; }
                if (haveDb && loggingFor(mode)) { ensureTx(); if (!db.addLink(o.link)) dbErrors++; txOps++; touchedSinceCommit.insert(mode); }
                break;
            }
            case Op::Star:
                hotStar(o.id, o.on);
                bump(mode);
                if (haveDb) { ensureTx(); if (!db.setStarred(o.id, o.on)) dbErrors++; txOps++; touchedSinceCommit.insert(mode); }
                break;
            case Op::Note:
                if (haveDb) { ensureTx(); if (!db.setNote(o.id, o.a, o.b)) dbErrors++; txOps++; touchedSinceCommit.insert(mode); }
                break;
            default: break;
            }
        }
        opsSincePrune += (int64_t)(to - from);
    }

    void applyBarrier(Op& o) {
        commit();
        std::vector<std::string> refs;
        std::vector<std::string> roots;
        { std::lock_guard<std::mutex> lk(setMu); roots = audioRoots; roots.insert(roots.end(), iqRoots.begin(), iqRoots.end()); }
        if (o.t == Op::DeleteMode) {
            if (db.isOpen()) db.deleteMode(o.mode, &refs);
            { std::lock_guard<std::mutex> lk(hotMu); for (auto it = hot.begin(); it != hot.end();) it = it->first.first == o.mode ? hot.erase(it) : std::next(it); }
            bump(o.mode);
        } else {
            if (db.isOpen()) db.deleteAll(&refs);
            { std::lock_guard<std::mutex> lk(hotMu); hot.clear(); }
            for (int i = 0; i < kGens; i++) gens[i].fetch_add(1);
            genAll.fetch_add(1);
        }
        removeFiles(refs, roots);
    }

    void applyBatch(std::deque<Op>& batch) {
        size_t i = 0;
        while (i < batch.size()) {
            size_t j = i;
            while (j < batch.size() && batch[j].t != Op::DeleteMode && batch[j].t != Op::DeleteAll) j++;
            if (j > i) applySegment(batch, i, j);
            if (j < batch.size()) { applyBarrier(batch[j]); j++; }
            i = j;
        }
        applied.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    void doPrune() {
        if (!db.isOpen()) return;
        DbLimits l;
        { std::lock_guard<std::mutex> lk(setMu); l = limits; }
        db.prune(l, wallMs());
        opsSincePrune = 0;
        nextPrune = steadySec() + cfg.pruneEverySec;
        for (int i = 0; i < kGens; i++) gens[i].fetch_add(1);
        genAll.fetch_add(1);
    }

    // ------------------------------------------------------------ threads
    void writerMain() {
        lowPriority();
        startedMs = wallMs();
        nextRecoverTry = 0;
        for (;;) {
            std::deque<Op> batch;
            bool doFlush = false, doStop = false;
            uint64_t flushTarget = 0;
            {
                std::unique_lock<std::mutex> lk(qmu);
                double wait = txOps > 0 ? std::max(0.001, txDeadline - steadySec()) : 0.25;
                qcv.wait_for(lk, std::chrono::duration<double>(wait), [&] { return stopReq || (!paused && (!q.empty() || flushReq || pruneReq.load())); });
                if (stopReq) paused = false;
                if (!paused) batch.swap(q);
                doFlush = flushReq && !paused;
                doStop = stopReq && batch.empty();
                if (doFlush) { flushReq = false; flushTarget = flushIssued; }
            }
            if (!batch.empty()) applyBatch(batch);
            const double now = steadySec();
            if (db.isOpen()) {
                if (!recovered && now >= nextRecoverTry) {
                    const int n = db.recoverOrphans(wallMs(), startedMs, cfg.orphanStaleMs);
                    if (n >= 0) { recovered = true; if (n > 0) { for (int i = 0; i < kGens; i++) gens[i].fetch_add(1); genAll.fetch_add(1); } }
                    else nextRecoverTry = now + 1.0;
                }
                if (recovered && now >= nextHeartbeat) {
                    db.setMeta("heartbeat", std::to_string(wallMs()));
                    nextHeartbeat = now + 1.0;
                }
            }
            if (settingsDirty.load() && !settingsPath.empty() && (now >= nextSettingsSave || doStop)) {
                settingsDirty = false;
                nextSettingsSave = now + 1.0;
                if (!writeOwnerOnlyFile(settingsPath, self->saveSettings())) dbErrors++;
            }
            if (txOps >= cfg.commitItems || (txOps > 0 && now >= txDeadline) || doFlush || doStop) commit();
            if (pruneReq.exchange(false) || (db.isOpen() && (opsSincePrune >= 2000 || now >= nextPrune))) doPrune();
            if (doFlush) {
                { std::lock_guard<std::mutex> lk(qmu); flushDone = std::max(flushDone, flushTarget); }
                doneCv.notify_all();
            }
            if (doStop) break;
        }
        if (db.isOpen()) {
            commit();
            db.closeActive(wallMs(), startedMs);
            db.setMeta("heartbeat", "0");   // a clean shutdown: the next start need not wait for a stale heartbeat
        }
    }

    void readerMain() {
        lowPriority();
        for (;;) {
            std::pair<Query, std::function<void(Page&&)>> job;
            {
                std::unique_lock<std::mutex> lk(rqMu);
                rqCv.wait(lk, [&] { return rqStop || !rq.empty(); });
                if (rq.empty()) return;
                job = std::move(rq.front());
                rq.pop_front();
            }
            Page pg = self->query(job.first);
            if (job.second) job.second(std::move(pg));
        }
    }

    void cleanMain() {
        lowPriority();
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(clMu);
                if (clCv.wait_for(lk, std::chrono::seconds(std::max(1, cfg.retentionEverySec)), [&] { return clStop; })) return;
            }
            self->runRetention();
        }
    }
};

// ------------------------------------------------------------------ public

SessionStore::SessionStore() : p_(new Impl) { p_->self = this; }
SessionStore::~SessionStore() { close(); }

bool SessionStore::open(const std::string& dataDir, const StoreConfig& cfg, std::string* err) {
    const std::string dir = dataDir + "/sessions";
    if (cfg.loadSalt) privacy().loadSalt(dataDir + "/keys");
    return openAt(dir + "/sessions.db", cfg, err);
}

bool SessionStore::openAt(const std::string& dbPath, const StoreConfig& cfg, std::string* err) {
    close();
    Impl& I = *p_;
    I.cfg = cfg;
    if (I.cfg.queueMax < 16) I.cfg.queueMax = 16;
    {
        std::lock_guard<std::mutex> lk(I.setMu);
        I.loggingDefault = cfg.defaultLogging;
        I.limits = cfg.limits;
        I.limits.logDays = cfg.retention.logDays;
        I.limits.positionDays = cfg.retention.positionDays;
        I.retention = cfg.retention;
        I.audioRoots = cfg.audioRoots;
        I.iqRoots = cfg.iqRoots;
        I.eventsPerSec = cfg.eventsPerSecPerMode;
    }
    if (!I.db.open(dbPath, err, &I.info)) return false;
    I.dbPathStr = dbPath;
    I.settingsPath.clear();
    if (dbPath != ":memory:") {
        I.settingsPath = strOf(pathOf(dbPath).parent_path() / "settings.txt");
        std::error_code ec;
        if (fs::is_regular_file(pathOf(I.settingsPath), ec)) {
            std::ifstream in(pathOf(I.settingsPath), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            loadSettings(ss.str());
        }
    }
    I.settingsDirty = false;
    I.ids.clear();
    for (const auto& m : I.db.maxSequences()) I.ids.raise(m.first.first, m.first.second, m.second);
    I.stopReq = I.flushReq = I.paused = false;
    I.flushDone = I.flushIssued = 0;
    I.txOps = 0;
    I.recovered = false;
    I.nextHeartbeat = I.nextPrune = 0;
    I.nextPrune = steadySec() + I.cfg.pruneEverySec;
    I.opsSincePrune = 0;
    I.buckets.clear();
    I.rqStop = false;
    I.clStop = false;
    I.startedMs = wallMs();
    I.writerTh = namedThread("onair-session", [this] { p_->writerMain(); });
    I.readerTh = namedThread("onair-sess-read", [this] { p_->readerMain(); });
    if (I.cfg.retentionEverySec > 0) I.cleanTh = namedThread("onair-sess-clean", [this] { p_->cleanMain(); });
    I.open = true;
    return true;
}

void SessionStore::close() {
    Impl& I = *p_;
    if (!I.open) return;
    I.open = false;
    { std::lock_guard<std::mutex> lk(I.clMu); I.clStop = true; }
    I.clCv.notify_all();
    { std::lock_guard<std::mutex> lk(I.rqMu); I.rqStop = true; }
    I.rqCv.notify_all();
    { std::lock_guard<std::mutex> lk(I.qmu); I.stopReq = true; }
    I.qcv.notify_all();
    if (I.writerTh.joinable()) I.writerTh.join();
    if (I.readerTh.joinable()) I.readerTh.join();
    if (I.cleanTh.joinable()) I.cleanTh.join();
    I.db.close();
    std::lock_guard<std::mutex> lk(I.qmu);
    I.q.clear();
}

bool SessionStore::isOpen() const { return p_->open; }
const OpenInfo& SessionStore::openInfo() const { return p_->info; }
std::string SessionStore::dbPath() const { return p_->dbPathStr; }

uint64_t SessionStore::begin(Session s) {
    const int64_t t0 = steadyNs();
    if (s.startWall <= 0) s.startWall = wallMs();
    s.id = p_->ids.next(s.startWall, s.mode);
    const uint64_t id = s.id;
    if (p_->open) {
        Op o;
        o.t = Op::Begin;
        o.s = std::move(s);
        p_->push(std::move(o));
    }
    p_->timed(t0);
    return id;
}

void SessionStore::update(const Session& s) {
    if (!p_->open || s.id == 0) return;
    const int64_t t0 = steadyNs();
    Op o;
    o.t = Op::Update;
    o.s = s;
    p_->push(std::move(o));
    p_->timed(t0);
}

void SessionStore::end(Session s) {
    if (!p_->open || s.id == 0) return;
    const int64_t t0 = steadyNs();
    if (s.endWall <= 0) s.endWall = wallMs();
    if (s.durationMs == 0 && s.endWall > s.startWall) s.durationMs = (uint32_t)std::min<int64_t>(s.endWall - s.startWall, 0xFFFFFFFFll);
    Op o;
    o.t = Op::End;
    o.s = std::move(s);
    p_->push(std::move(o));
    p_->timed(t0);
}

void SessionStore::link(uint64_t id, LinkType type, const std::string& ref, int64_t wall) {
    if (!p_->open || id == 0) return;
    const int64_t t0 = steadyNs();
    Op o;
    o.t = Op::Link;
    o.id = id;
    o.link.sessionId = id;
    o.link.type = (uint8_t)type;
    o.link.ref = ref;
    o.link.wall = wall > 0 ? wall : wallMs();
    p_->push(std::move(o));
    p_->timed(t0);
}

void SessionStore::setStarred(uint64_t id, bool on) {
    if (!p_->open || id == 0) return;
    Op o; o.t = Op::Star; o.id = id; o.on = on;
    p_->push(std::move(o));
}

void SessionStore::setNote(uint64_t id, const std::string& note, const std::string& tags) {
    if (!p_->open || id == 0) return;
    Op o; o.t = Op::Note; o.id = id; o.a = note; o.b = tags;
    p_->push(std::move(o));
}

void SessionStore::setLogging(uint16_t mode, bool on) { { std::lock_guard<std::mutex> lk(p_->setMu); p_->logging[mode] = on; } p_->settingsDirty = true; }
bool SessionStore::logging(uint16_t mode) const { return p_->loggingFor(mode); }
void SessionStore::setLoggingDefault(bool on) { { std::lock_guard<std::mutex> lk(p_->setMu); p_->loggingDefault = on; } p_->settingsDirty = true; }
bool SessionStore::loggingDefault() const { std::lock_guard<std::mutex> lk(p_->setMu); return p_->loggingDefault; }
void SessionStore::setModeName(uint16_t mode, const std::string& name) { std::lock_guard<std::mutex> lk(p_->setMu); p_->names[mode] = name; }
std::string SessionStore::modeName(uint16_t mode) const { return p_->nameOf(mode); }
void SessionStore::setLimits(const DbLimits& l) { { std::lock_guard<std::mutex> lk(p_->setMu); p_->limits = l; } p_->settingsDirty = true; }
void SessionStore::setRetention(const RetentionPolicy& r) {
    {
        std::lock_guard<std::mutex> lk(p_->setMu);
        p_->retention = r;
        p_->limits.logDays = r.logDays;
        p_->limits.positionDays = r.positionDays;
    }
    p_->settingsDirty = true;
}

std::string SessionStore::loggingLabel() const { return loggingDefault() ? "Logging: ON" : "Logging: OFF"; }
bool SessionStore::firstRunNoticeNeeded() const { std::lock_guard<std::mutex> lk(p_->setMu); return !p_->noticeAck; }
void SessionStore::acknowledgeFirstRunNotice() { { std::lock_guard<std::mutex> lk(p_->setMu); p_->noticeAck = true; } p_->settingsDirty = true; }

std::string SessionStore::saveSettings() const {
    std::lock_guard<std::mutex> lk(p_->setMu);
    std::string o;
    auto kv = [&](const std::string& k, long long v) { o += k + "=" + std::to_string(v) + "\n"; };
    kv("logging_default", p_->loggingDefault);
    for (const auto& m : p_->logging) kv("logging." + std::to_string(m.first), m.second);
    kv("notice_ack", p_->noticeAck);
    kv("events_per_sec", p_->eventsPerSec);
    kv("per_mode_rows", p_->limits.perModeRows);
    kv("max_bytes", p_->limits.maxBytes);
    kv("audio_days", p_->retention.audioDays);
    kv("audio_max_bytes", p_->retention.audioMaxBytes);
    kv("iq_days", p_->retention.iqDays);
    kv("iq_max_bytes", p_->retention.iqMaxBytes);
    kv("log_days", p_->retention.logDays);
    kv("position_days", p_->retention.positionDays);
    kv("trash_days", p_->retention.trashDays);
    return o;
}

void SessionStore::loadSettings(const std::string& text) {
    std::lock_guard<std::mutex> lk(p_->setMu);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const long long v = strtoll(line.c_str() + eq + 1, nullptr, 10);
        if (k == "logging_default") p_->loggingDefault = v != 0;
        else if (k.rfind("logging.", 0) == 0) { const long m = strtol(k.c_str() + 8, nullptr, 10); if (m >= 0 && m <= 65535) p_->logging[(uint16_t)m] = v != 0; }
        else if (k == "notice_ack") p_->noticeAck = v != 0;
        else if (k == "events_per_sec") p_->eventsPerSec = (int)std::max<long long>(1, v);
        else if (k == "per_mode_rows") p_->limits.perModeRows = v;
        else if (k == "max_bytes") p_->limits.maxBytes = v;
        else if (k == "audio_days") p_->retention.audioDays = (int)v;
        else if (k == "audio_max_bytes") p_->retention.audioMaxBytes = v;
        else if (k == "iq_days") p_->retention.iqDays = (int)v;
        else if (k == "iq_max_bytes") p_->retention.iqMaxBytes = v;
        else if (k == "log_days") { p_->retention.logDays = (int)v; p_->limits.logDays = (int)v; }
        else if (k == "position_days") { p_->retention.positionDays = (int)v; p_->limits.positionDays = (int)v; }
        else if (k == "trash_days") p_->retention.trashDays = (int)v;
    }
}
RetentionPolicy SessionStore::retention() const { std::lock_guard<std::mutex> lk(p_->setMu); return p_->retention; }
void SessionStore::setEngine(automation::Engine* eng) { std::lock_guard<std::mutex> lk(p_->setMu); p_->eng = eng; }
void SessionStore::setEventsPerSecond(int n) { std::lock_guard<std::mutex> lk(p_->setMu); p_->eventsPerSec = std::max(1, n); }
void SessionStore::setLibraryRoots(const std::vector<std::string>& audio, const std::vector<std::string>& iq) {
    std::lock_guard<std::mutex> lk(p_->setMu);
    p_->audioRoots = audio;
    p_->iqRoots = iq;
}

std::vector<Session> SessionStore::window(uint16_t mode, int receiver, size_t max) const {
    std::vector<Session> out;
    std::lock_guard<std::mutex> lk(p_->hotMu);
    for (const auto& kv : p_->hot) {
        if (kv.first.first != mode) continue;
        if (receiver >= 0 && kv.first.second != (uint16_t)receiver) continue;
        for (const Session& s : kv.second) out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Session& a, const Session& b) { return a.startWall != b.startWall ? a.startWall > b.startWall : a.id > b.id; });
    if (out.size() > max) out.resize(max);
    return out;
}

uint64_t SessionStore::gen(uint16_t mode) const { return p_->gens[mode % kGens].load(std::memory_order_relaxed); }
uint64_t SessionStore::genAll() const { return p_->genAll.load(std::memory_order_relaxed); }

Page SessionStore::query(const Query& q) const {
    const uint64_t g = q.mode >= 0 ? gen((uint16_t)q.mode) : genAll();   // read first: a change during the query makes the next poll see a newer one
    p_->queriesRun.fetch_add(1, std::memory_order_relaxed);
    Page pg = p_->db.query(q);
    pg.gen = g;
    return pg;
}

void SessionStore::queryAsync(const Query& q, std::function<void(Page&&)> done) {
    {
        std::lock_guard<std::mutex> lk(p_->rqMu);
        if (!p_->open || p_->rqStop) { if (done) done(Page()); return; }
        if (p_->rq.size() >= 64) p_->rq.pop_front();   // the UI asked again: the oldest request is stale
        p_->rq.emplace_back(q, std::move(done));
    }
    p_->rqCv.notify_one();
}

bool SessionStore::get(uint64_t id, Session& out) const { return p_->db.get(id, out); }
std::vector<SessionLink> SessionStore::links(uint64_t id) const { return p_->db.links(id); }
bool SessionStore::note(uint64_t id, Note& out) const { return p_->db.note(id, out); }
DbStats SessionStore::dbStats() const { return p_->db.stats(); }

StoreStats SessionStore::stats() const {
    StoreStats s;
    s.enqueued = p_->enqueued.load();
    s.dropped = p_->dropped.load();
    s.applied = p_->applied.load();
    s.commits = p_->commits.load();
    s.dbErrors = p_->dbErrors.load();
    s.eventsEmitted = p_->eventsEmitted.load();
    s.eventsThrottled = p_->eventsThrottled.load();
    s.queriesRun = p_->queriesRun.load();
    s.refusedLinks = p_->refusedLinks.load();
    s.maxEnqueueNs = p_->maxEnqueueNs.load();
    { std::lock_guard<std::mutex> lk(p_->qmu); s.queued = p_->q.size(); }
    return s;
}

uint64_t SessionStore::dropped() const { return p_->dropped.load(); }

void SessionStore::flush() {
    if (!p_->open) return;
    std::unique_lock<std::mutex> lk(p_->qmu);
    const uint64_t want = ++p_->flushIssued;
    p_->flushReq = true;
    lk.unlock();
    p_->qcv.notify_one();
    lk.lock();
    p_->doneCv.wait(lk, [&] { return p_->flushDone >= want || !p_->open; });
}

void SessionStore::pauseWriter(bool on) {
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->paused = on; }
    p_->qcv.notify_all();
}

// ------------------------------------------------------------------ export

std::string SessionStore::exportAs(const Query& q0, const std::string& format, bool anonymise) const {
    std::string fmt = format;
    for (char& c : fmt) c = (char)tolower((unsigned char)c);
    Query q = q0;
    q.limit = 1000;
    q.offset = 0;
    std::vector<Session> rows;
    for (;;) {
        Page pg = p_->db.query(q);
        rows.insert(rows.end(), pg.rows.begin(), pg.rows.end());
        if (pg.rows.size() < q.limit || rows.size() >= 1000000) break;
        q.offset += pg.rows.size();
    }
    auto modeNm = [&](const Session& s) { return p_->nameOf(s.mode); };
    auto titleOf = [&](const Session& s) { return anonymise ? modeNm(s) + " " + kindName((Kind)s.kind) : s.title; };
    auto field = [&](const Session& s, int i) { return anonymise ? std::string() : s.who[i]; };
    auto alias = [&](const Session& s, int i) { return anonymise ? std::string() : s.alias[i]; };
    auto keyOf = [&](const Session& s) { return anonymise ? std::string() : s.key; };
    auto pos = [&](const Session& s) { return !anonymise && s.hasPosition(); };
    std::string o;
    if (fmt == "csv") {
        o = "id,mode,mode_name,receiver,kind,key,title,source,target,source_alias,target_alias,start_utc,end_utc,duration_s,level_db,peak_db,snr_db,error_rate,freq_hz,carrier,slot,lat,lon,alt_m,verdict,flags,events\r\n";
        for (const Session& s : rows) {
            o += std::to_string(s.id) + "," + std::to_string(s.mode) + "," + csvCell(modeNm(s)) + "," + std::to_string(s.receiver) + "," + kindName((Kind)s.kind) + "," +
                 csvCell(keyOf(s)) + "," + csvCell(titleOf(s)) + "," + csvCell(field(s, 0)) + "," + csvCell(field(s, 1)) + "," + csvCell(alias(s, 0)) + "," + csvCell(alias(s, 1)) + "," +
                 isoUtc(s.startWall) + "," + isoUtc(s.endWall) + "," + numStr(s.durationMs / 1000.0, 3) + "," + numStr(s.levelDb, 1) + "," + numStr(s.levelPeakDb, 1) + "," +
                 numStr(s.snrDb, 1) + "," + numStr(s.errRate, 4) + "," + std::to_string(s.freqHz) + "," + std::to_string(s.carrierId) + "," + std::to_string(s.slot) + "," +
                 (pos(s) ? numStr(s.lat, 6) : "") + "," + (pos(s) ? numStr(s.lon, 6) : "") + "," + (pos(s) ? numStr(s.altM, 1) : "") + "," +
                 verdictName((Verdict)s.verdict) + "," + std::to_string(s.flags) + "," + std::to_string(s.eventCount) + "\r\n";
        }
        return o;
    }
    auto jsonRow = [&](const Session& s) {
        std::string r = "{\"id\":\"" + std::to_string(s.id) + "\",\"mode\":" + std::to_string(s.mode) + ",\"mode_name\":\"" + jsonEscape(modeNm(s)) + "\",\"receiver\":" + std::to_string(s.receiver) +
                        ",\"kind\":\"" + kindName((Kind)s.kind) + "\",\"key\":\"" + jsonEscape(keyOf(s)) + "\",\"title\":\"" + jsonEscape(titleOf(s)) + "\"" +
                        ",\"source\":\"" + jsonEscape(field(s, 0)) + "\",\"target\":\"" + jsonEscape(field(s, 1)) + "\",\"source_alias\":\"" + jsonEscape(alias(s, 0)) + "\",\"target_alias\":\"" + jsonEscape(alias(s, 1)) + "\"" +
                        ",\"start_utc\":\"" + isoUtc(s.startWall) + "\",\"end_utc\":" + (s.endWall ? "\"" + isoUtc(s.endWall) + "\"" : "null") + ",\"duration_s\":" + numStr(s.durationMs / 1000.0, 3) +
                        ",\"level_db\":" + numStr(s.levelDb, 1) + ",\"peak_db\":" + numStr(s.levelPeakDb, 1) + ",\"snr_db\":" + numStr(s.snrDb, 1) + ",\"error_rate\":" + numStr(s.errRate, 4) +
                        ",\"freq_hz\":" + std::to_string(s.freqHz) + ",\"carrier\":" + std::to_string(s.carrierId) + ",\"slot\":" + std::to_string(s.slot);
        if (pos(s)) r += ",\"lat\":" + numStr(s.lat, 6) + ",\"lon\":" + numStr(s.lon, 6);
        r += std::string(",\"verdict\":\"") + verdictName((Verdict)s.verdict) + "\",\"flags\":" + std::to_string(s.flags) + ",\"events\":" + std::to_string(s.eventCount) + "}";
        return r;
    };
    if (fmt == "jsonl") {
        for (const Session& s : rows) o += jsonRow(s) + "\n";
        return o;
    }
    o = "[";
    bool first = true;
    for (const Session& s : rows) { o += first ? "\n" : ",\n"; first = false; o += jsonRow(s); }
    o += rows.empty() ? "]\n" : "\n]\n";
    return o;
}

// ------------------------------------------------------------------ retention and deletion

int SessionStore::runRetention() {
    Impl& I = *p_;
    if (!I.open) return 0;
    RetentionPolicy r;
    std::vector<std::string> ar, ir;
    {
        std::lock_guard<std::mutex> lk(I.setMu);
        r = I.retention;
        ar = I.audioRoots;
        ir = I.iqRoots;
    }
    flush();
    const int64_t now = wallMs();
    int n = 0;
    {
        LibraryRule a; a.roots = ar; a.maxAgeDays = r.audioDays; a.maxBytes = r.audioMaxBytes;
        n += cleanLibrary(I.db, LinkType::Audio, a, now).filesDeleted;
        LibraryRule i; i.roots = ir; i.maxAgeDays = r.iqDays; i.maxBytes = r.iqMaxBytes;
        n += cleanLibrary(I.db, LinkType::Iq, i, now).filesDeleted;
    }
    std::string trash = I.cfg.trashDir;
    if (trash.empty() && !I.dbPathStr.empty() && I.dbPathStr != ":memory:") trash = strOf(pathOf(I.dbPathStr).parent_path() / "trash");
    if (!trash.empty()) purgeTrash(trash, r.trashDays, now);
    I.pruneReq = true;
    I.qcv.notify_one();
    flush();
    return n;
}

int64_t SessionStore::deleteMode(uint16_t mode) {
    Impl& I = *p_;
    if (!I.open) return 0;
    flush();
    const int64_t n = (int64_t)I.db.query([&] { Query q; q.mode = mode; q.limit = 1; return q; }()).total;
    Op o; o.t = Op::DeleteMode; o.mode = mode;
    // a barrier must not be lost to a full queue: wait for room
    for (int i = 0; i < 1000 && !I.push(Op(o)); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    flush();
    return n;
}

int64_t SessionStore::deleteEverything() {
    Impl& I = *p_;
    if (!I.open) return 0;
    flush();
    const int64_t n = I.db.stats().rows;
    Op o; o.t = Op::DeleteAll;
    for (int i = 0; i < 1000 && !I.push(Op(o)); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    flush();
    privacy().regenerateSalt();
    return n;
}

SessionStore& global() {
    static SessionStore* s = new SessionStore;   // never destroyed: threads may still use it at exit
    return *s;
}

// ------------------------------------------------------------------ CachedList

struct CachedList::Impl {
    SessionStore* st = nullptr;
    std::mutex mu;
    Query q;
    std::shared_ptr<const Page> page = std::make_shared<Page>();
    bool have = false;
    bool inflight = false;
    bool stale = true;          // the query changed since the page was read
    uint64_t qver = 0;
    uint64_t refreshes = 0;
};

namespace {
bool sameQuery(const Query& a, const Query& b) {
    return a.mode == b.mode && a.receiver == b.receiver && a.kind == b.kind && a.verdict == b.verdict && a.text == b.text && a.activeOnly == b.activeOnly &&
           a.starredOnly == b.starredOnly && a.withAudio == b.withAudio && a.since == b.since && a.until == b.until && a.sort == b.sort && a.offset == b.offset && a.limit == b.limit;
}
}  // namespace

CachedList::CachedList(SessionStore& store) : p_(std::make_shared<Impl>()) { p_->st = &store; }
CachedList::~CachedList() { std::lock_guard<std::mutex> lk(p_->mu); p_->qver++; }   // a refresh still on its way is dropped when it lands

void CachedList::setQuery(const Query& q) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (sameQuery(p_->q, q)) return;
    p_->q = q;
    p_->stale = true;
    p_->qver++;
}

const Query& CachedList::query() const { return p_->q; }

std::shared_ptr<const Page> CachedList::page() {
    Query ask;
    uint64_t ver;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        const uint64_t cur = p_->q.mode >= 0 ? p_->st->gen((uint16_t)p_->q.mode) : p_->st->genAll();
        if (p_->inflight || (p_->have && !p_->stale && p_->page->gen == cur)) return p_->page;
        p_->inflight = true;
        p_->refreshes++;
        ask = p_->q;
        ver = p_->qver;
    }
    std::shared_ptr<Impl> impl = p_;
    p_->st->queryAsync(ask, [impl, ver](Page&& pg) {
        std::lock_guard<std::mutex> lk(impl->mu);
        impl->inflight = false;
        if (ver == impl->qver) {
            impl->page = std::make_shared<const Page>(std::move(pg));
            impl->have = true;
            impl->stale = false;
        }
    });
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->page;
}

bool CachedList::loading() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->inflight || !p_->have; }
uint64_t CachedList::refreshes() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->refreshes; }

}  // namespace dect2::sess
