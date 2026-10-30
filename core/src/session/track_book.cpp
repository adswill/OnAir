#include "dect2/session/track_book.h"
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace dect2::sess {

int64_t wallNowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct TrackBook::Impl {
    struct Open { Session s; int64_t lastSeen = 0, lastPush = 0; };
    BookConfig cfg;
    SessionStore* store;
    mutable std::mutex mu;
    std::unordered_map<std::string, Open> open;
    int64_t lastSweep = 0;
    SessionStore& st() { return store ? *store : global(); }

    void endLocked(Open& o, int64_t nowMs) {
        Session s = o.s;
        s.endWall = std::max<int64_t>(o.lastSeen, s.startWall + 1);
        (void)nowMs;
        st().end(std::move(s));
    }
    void sweepLocked(int64_t nowMs) {
        lastSweep = nowMs;
        for (auto it = open.begin(); it != open.end();) {
            if (nowMs - it->second.lastSeen >= cfg.gapMs) { endLocked(it->second, nowMs); it = open.erase(it); }
            else ++it;
        }
    }
};

TrackBook::TrackBook(const BookConfig& cfg, SessionStore* store) : p_(new Impl) { p_->cfg = cfg; p_->store = store; }
TrackBook::~TrackBook() = default;

uint64_t TrackBook::observe(Session s, int64_t nowMs, int64_t lastSeenMs) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.mu);
    if (!lastSeenMs) lastSeenMs = nowMs;
    if (nowMs - p.lastSweep >= 1000) p.sweepLocked(nowMs);
    s.mode = p.cfg.mode;
    s.receiver = p.cfg.receiver;
    s.kind = (uint8_t)p.cfg.kind;
    auto it = p.open.find(s.key);
    if (it == p.open.end()) {
        if (nowMs - lastSeenMs >= p.cfg.gapMs) return 0;     // already stale: not a new session
        if (p.open.size() >= p.cfg.maxOpen) return 0;
        if (s.startWall == 0) s.startWall = lastSeenMs;
        s.endWall = 0;
        Impl::Open o;
        o.lastSeen = lastSeenMs;
        o.lastPush = nowMs;
        s.id = 0;
        s.id = p.st().begin(s);
        o.s = std::move(s);
        const uint64_t id = o.s.id;
        p.open.emplace(o.s.key, std::move(o));
        return id;
    }
    Impl::Open& o = it->second;
    o.lastSeen = std::max(o.lastSeen, lastSeenMs);
    const bool flagChange = s.flags != o.s.flags;
    const uint64_t id = o.s.id;
    const int64_t start = o.s.startWall;
    o.s = std::move(s);
    o.s.id = id;
    o.s.startWall = start;
    o.s.endWall = 0;
    o.s.durationMs = (uint32_t)std::min<int64_t>(std::max<int64_t>(0, o.lastSeen - start), 4000000000ll);
    if (flagChange || nowMs - o.lastPush >= p.cfg.updateMs) { o.lastPush = nowMs; p.st().update(o.s); }
    return id;
}

void TrackBook::sweep(int64_t nowMs) { std::lock_guard<std::mutex> lk(p_->mu); p_->sweepLocked(nowMs); }

void TrackBook::endAll(int64_t nowMs) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& kv : p_->open) p_->endLocked(kv.second, nowMs);
    p_->open.clear();
}

void TrackBook::link(const std::string& key, LinkType t, const std::string& ref, int64_t wallMs) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->open.find(key);
    if (it != p_->open.end()) p_->st().link(it->second.s.id, t, ref, wallMs);
}

size_t TrackBook::openCount() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->open.size(); }
void TrackBook::setConfig(const BookConfig& c) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfg = c; }
BookConfig TrackBook::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }

}  // namespace dect2::sess
