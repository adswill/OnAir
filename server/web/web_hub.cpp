#include "web_hub.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace onair_web {

double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    auto f = [](const FILETIME& t) { return (double)(((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
    return f(k) + f(u);
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
double wallSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- state
struct WebHub::Session {
    int id = 0;
    UserClass cls = UserClass::Public;
    std::string name, label, ip;
    int maxMinutes = 0;
    double created = 0, lastAction = 0;

    std::mutex m;   // everything below up to the queue
    double freqHz = 0;
    bool haveFreq = false, outside = false;
    RxSettings rx;
    std::unique_ptr<VirtualRx> vrx;
    bool dirty = true;
    std::string rxErr;
    bool audioOn = false;
    AudioCodec codec = AudioCodec::Adpcm;
    int arate = 12000;
    AdpcmState ad;
    uint32_t aSeq = 0;
    std::vector<int16_t> pend;
    std::vector<float> fbuf;
    bool specOn = false;
    int bins = 1024, fps = 10;
    double sStart = 0, sStop = 0;
    bool span = false;
    RowScale sc;
    double nextSpec = 0;
    uint32_t sSeq = 0;
    std::vector<float> rowDb;
    std::vector<uint8_t> rowQ;
    int degrade = 0;
    double overSince = 0;
    std::set<std::string> subs;
    RangeView rv;                      // the host's frequency ranges for this listener's class (cached; follows rangeGen_)
    unsigned rvGen = 0;
    bool rclamp = false, rangeNote = false;
    double cpuSec = 0, cpuMark = 0, cpuPct = 0;

    std::mutex qm;
    std::deque<OutMsg> q;
    size_t qBytes = 0;
    uint64_t bytes = 0, bytesMark = 0, dropped = 0;
    double kbps = 0;
    std::string kick;
};

struct WebHub::SharedDec {
    std::string key, kind;
    double freqHz = 0, rate = 0;
    bool needsFreq = false;
    std::unique_ptr<WebDecoder> dec;
    std::unique_ptr<SliceIq> slice;
    bool dirty = true, outside = false;
    std::set<int> subs;
    std::mutex m;
    std::vector<cf32> buf;
    double cpuSec = 0;
};

// A few worker threads that run one job over many items; the caller helps and waits. Items are taken from a shared counter, so a slow
// item does not hold the others back.
struct WebHub::Pool {
    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cv, done;
    std::function<void(size_t)> job;
    size_t n = 0;
    std::atomic<size_t> next{0};
    int active = 0;
    uint64_t gen = 0;
    bool quit = false;
    explicit Pool(int workers) {
        for (int i = 0; i < workers; i++) th.emplace_back([this] { loop(); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> g(m); quit = true; gen++; }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    void loop() {
        uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return gen != seen; });
                seen = gen;
                if (quit) return;
            }
            work();
            std::lock_guard<std::mutex> g(m);
            if (--active == 0) done.notify_all();
        }
    }
    void work() { size_t i; while ((i = next.fetch_add(1)) < n) job(i); }
    void run(size_t count, std::function<void(size_t)> f) {
        if (th.empty() || count <= 1) { for (size_t i = 0; i < count; i++) f(i); return; }
        {
            std::lock_guard<std::mutex> g(m);
            job = std::move(f); n = count; next = 0; active = (int)th.size(); gen++;
        }
        cv.notify_all();
        work();
        std::unique_lock<std::mutex> lk(m);
        done.wait(lk, [&] { return active == 0; });
    }
};

WebHub::WebHub() {
    cores_ = std::max(1u, std::thread::hardware_concurrency());
}
WebHub::~WebHub() { stop(); }

void WebHub::setLimits(const HubLimits& l) { std::lock_guard<std::mutex> g(mu_); lim_ = l; qCap_ = l.queueBytes; }
HubLimits WebHub::limits() const { std::lock_guard<std::mutex> g(mu_); return lim_; }
void WebHub::registerDecoder(DecoderFactory f) { std::lock_guard<std::mutex> g(mu_); factories_[f.kind] = std::move(f); }
void WebHub::setServerInfo(const std::string& n, const std::string& l, const std::string& a) { std::lock_guard<std::mutex> g(mu_); srvName_ = n; srvLoc_ = l; srvAnt_ = a; }

void WebHub::start() {
    if (run_) return;
    int t = lim_.threads > 0 ? lim_.threads : (int)std::clamp(cores_ / 2, 1u, 4u);
    pool_.reset(new Pool(t - 1));   // the DSP thread is the first worker
    run_ = true;
    cpuT0_ = wallSeconds();
    dsp_ = std::thread([this] { dspId_ = std::this_thread::get_id(); dspLoop(); });
}

void WebHub::stop() {
    if (!run_) return;
    run_ = false;
    inCv_.notify_all();
    if (dsp_.joinable()) dsp_.join();
    pool_.reset();
}

// ---------------------------------------------------------------- ingest and DSP
void WebHub::pushIq(const cf32* x, size_t n, double centreHz, double rateHz) {
    if (!n || !(rateHz > 0)) return;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (sessions_.empty() && decs_.empty()) { pubCentre_ = centreHz; pubRate_ = rateHz; return; }   // nobody listens: no work at all
    }
    std::lock_guard<std::mutex> g(inMu_);
    const size_t cap = (size_t)std::max(rateHz * 0.5, 262144.0);
    if (inSamples_ + n > cap) { inDropped_ += n; return; }
    Chunk c;
    c.x.assign(x, x + n);
    c.centre = centreHz; c.rate = rateHz;
    inSamples_ += n;
    inQ_.push_back(std::move(c));
    inCv_.notify_one();
}

void WebHub::processPending() {
    while (true) {
        Chunk c;
        {
            std::lock_guard<std::mutex> g(inMu_);
            if (inQ_.empty()) break;
            c = std::move(inQ_.front());
            inQ_.pop_front();
            inSamples_ -= c.x.size();
        }
        std::lock_guard<std::mutex> g(procMu_);
        processChunk(c);
    }
}

void WebHub::dspLoop() {
    double nextPoll = 0, cpuMark = threadCpuSeconds();
    while (run_) {
        Chunk c;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(inMu_);
            inCv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return !inQ_.empty() || !run_; });
            if (!run_) break;
            if (!inQ_.empty()) { c = std::move(inQ_.front()); inQ_.pop_front(); inSamples_ -= c.x.size(); have = true; }
        }
        if (have) { std::lock_guard<std::mutex> g(procMu_); processChunk(c); }
        const double now = wallSeconds();
        if (now >= nextPoll) {   // the decoders' results, 5 times a second, also when no samples come
            nextPoll = now + 0.2;
            std::vector<std::shared_ptr<SharedDec>> ds;
            { std::lock_guard<std::mutex> g(mu_); for (auto& kv : decs_) ds.push_back(kv.second); }
            for (auto& d : ds) {
                std::vector<std::string> ev;
                std::string pk;
                std::set<int> subs;
                {
                    std::lock_guard<std::mutex> g(d->m);
                    if (!d->dec) continue;
                    d->dec->poll(ev);
                    pk = d->dec->policyKind();
                    subs = d->subs;
                }
                if (ev.empty()) continue;
                bool ok;
                { std::lock_guard<std::mutex> g(polMu_); ok = policy_.decoderAllowed(d->kind) && (pk.empty() || policy_.decoderAllowed(pk)); }
                if (!ok) continue;   // the host's mode is one the policy keeps from web listeners: nothing is passed on
                for (int id : subs) {
                    std::shared_ptr<Session> s;
                    { std::lock_guard<std::mutex> g(mu_); auto it = sessions_.find(id); if (it != sessions_.end()) s = it->second; }
                    if (!s) continue;
                    if (!d->needsFreq) {   // results of the host's own receiver: only a listener who may see the whole capture gets them
                        std::lock_guard<std::mutex> sg(s->m);
                        refreshView(*s);
                        if (!captureVisible(s->rv, pubCentre_.load(), pubRate_.load())) continue;
                    }
                    for (const auto& e : ev) {
                        if (e.size() > 65536) continue;   // bigger results are not sent (documented limit)
                        enqueue(*s, OutMsg{"{\"type\":\"event\",\"decoder\":\"" + jsonEsc(d->key) + "\",\"data\":" + e + "}", false, 1});
                    }
                }
            }
        }
        // the DSP threads' share of the machine, once a second
        const double cpu = threadCpuSeconds();
        const uint64_t wus = workerCpuUs_.exchange(0);
        cpuAcc_ += cpu - cpuMark + (double)wus * 1e-6;
        cpuMark = cpu;
        if (now - cpuT0_ >= 1.0) {
            dspCpuPct_ = cpuAcc_ / ((now - cpuT0_) * cores_) * 100.0;
            cpuAcc_ = 0; cpuT0_ = now;
        }
    }
}

void WebHub::reconfigureCapture(double centre, double rate) {
    const bool rateChanged = rate != rateHz_;
    centreHz_ = centre; rateHz_ = rate;
    pubCentre_ = centre; pubRate_ = rate;
    if (rateChanged) {
        fft_.configure(rate);
        wf_.configure(rate, 30);
        wfSeen_ = 0;
    }
    std::vector<std::shared_ptr<Session>> ss;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& kv : sessions_) ss.push_back(kv.second);
        for (auto& kv : decs_) { std::lock_guard<std::mutex> g2(kv.second->m); kv.second->dirty = true; }
    }
    const std::string msg = "{\"type\":\"capture\",\"centre_hz\":" + jsonNumber(centre) + ",\"rate_hz\":" + jsonNumber(rate) + "}";
    for (auto& s : ss) {
        { std::lock_guard<std::mutex> g(s->m); s->dirty = true; }
        enqueue(*s, OutMsg{msg, false, 0});
    }
}

// under s.m, on the DSP thread (the only one that touches fft_)
void WebHub::configureRx(Session& s) {
    s.dirty = false;
    s.vrx.reset();
    s.rxErr.clear();
    if (!s.haveFreq || rateHz_ <= 0) return;
    RxSettings r = s.rx;
    r.offsetHz = s.freqHz - centreHz_;
    r.audioRate = s.arate;
    s.outside = std::fabs(r.offsetHz) > rateHz_ * 0.48;
    if (s.outside) { s.rxErr = "outside the capture"; return; }
    // the host's frequency ranges: a tuning that is not (or no longer) allowed gets no receiver at all, so no audio can come out of it
    refreshView(s);
    if (s.rv.active()) {
        const RangeVerdict rvd = checkPassband(s.rv, r.demod, s.freqHz, r.lowHz, r.highHz, centreHz_, rateHz_);
        if (rvd != RangeVerdict::Ok) {
            s.rxErr = "forbidden_range";
            if (!s.rangeNote) { s.rangeNote = true; sendError(s, err::ForbiddenRange, verdictText(rvd)); }
            return;
        }
    }
    s.rangeNote = false;
    auto v = std::make_unique<VirtualRx>();
    std::string err;
    if (!v->configure(fft_, r, err)) { s.rxErr = err; return; }
    s.vrx = std::move(v);
    s.pend.clear();
}

void WebHub::processChunk(Chunk& c) {
    if (c.rate != rateHz_ || c.centre != centreHz_) reconfigureCapture(c.centre, c.rate);
    if (fft_.size() <= 0) return;
    streamSec_ += (double)c.x.size() / rateHz_;
    std::vector<std::shared_ptr<Session>> ss;
    std::vector<std::shared_ptr<SharedDec>> ds;
    bool anySpec = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& kv : sessions_) ss.push_back(kv.second);
        for (auto& kv : decs_) ds.push_back(kv.second);
    }
    for (auto& s : ss) { std::lock_guard<std::mutex> g(s->m); anySpec = anySpec || s->specOn; }
    if (anySpec) wf_.push(c.x.data(), c.x.size());
    const size_t nItems = ss.size() + ds.size();
    fft_.push(c.x.data(), c.x.size(), [&](const float* re, const float* im) {
        const uint64_t blockNo = fft_.block();
        const unsigned rot = rot_++;
        auto item = [&](size_t k) {
            const size_t idx = (k + rot) % nItems;   // fair: everyone is first in turn
            const double t0 = threadCpuSeconds();
            if (idx < ss.size()) {
                Session& s = *ss[idx];
                std::lock_guard<std::mutex> g(s.m);
                if (s.dirty) configureRx(s);
                if (s.vrx && s.audioOn) {
                    s.fbuf.clear();
                    s.vrx->process(re, im, blockNo, s.fbuf);
                    for (float v : s.fbuf) s.pend.push_back((int16_t)std::lround(v * 32767.f));
                    const size_t frame = (size_t)s.arate / 25;   // 40 ms
                    while (s.pend.size() >= frame) {
                        std::vector<int16_t> fr(s.pend.begin(), s.pend.begin() + (long)frame);
                        s.pend.erase(s.pend.begin(), s.pend.begin() + (long)frame);
                        bool silent = s.vrx->squelched();
                        if (silent) for (int16_t v : fr) if (v) { silent = false; break; }
                        enqueue(s, OutMsg{audioFrame(s.aSeq++, s.codec, s.arate, s.vrx->levelDb(), silent, fr, s.ad), true, 2});
                    }
                }
                s.cpuSec += threadCpuSeconds() - t0;
            } else {
                SharedDec& d = *ds[idx - ss.size()];
                std::lock_guard<std::mutex> g(d.m);
                if (d.rate <= 0 || !d.dec) return;
                if (d.dirty) {
                    d.dirty = false;
                    d.slice.reset();
                    const double off = d.freqHz - centreHz_;
                    d.outside = std::fabs(off) > rateHz_ * 0.48;
                    if (!d.outside) {
                        auto sl = std::make_unique<SliceIq>();
                        std::string err;
                        if (sl->configure(fft_, off, d.rate, err)) d.slice = std::move(sl);
                    }
                }
                if (!d.slice) return;
                d.buf.clear();
                d.slice->process(re, im, blockNo, d.buf);
                d.dec->feed(d.buf.data(), d.buf.size());
                d.cpuSec += threadCpuSeconds() - t0;
            }
        };
        auto counted = [&](size_t k) {   // the workers' processor time (the DSP thread's own is measured in its loop)
            if (std::this_thread::get_id() == dspId_) { item(k); return; }
            const double t0 = threadCpuSeconds();
            item(k);
            workerCpuUs_ += (uint64_t)((threadCpuSeconds() - t0) * 1e6);
        };
        if (pool_) pool_->run(nItems, counted);
        else for (size_t k = 0; k < nItems; k++) item(k);
    });
    // spectrum rows for the listeners whose turn it is
    if (anySpec && wf_.seq() != wfSeen_) {
        wfSeen_ = wf_.seq();
        const double now = streamSec_;   // rows paced by the samples, not the wall clock (a burst of samples gives its rows)
        const double half = rateHz_ * 0.5;
        for (auto& sp : ss) {
            Session& s = *sp;
            std::lock_guard<std::mutex> g(s.m);
            if (!s.specOn || now < s.nextSpec) continue;
            const double t0 = threadCpuSeconds();
            const int fps = std::max(1, s.fps >> s.degrade);
            s.nextSpec = now + 1.0 / fps - 0.005;
            double a = s.span ? s.sStart : centreHz_ - half, b = s.span ? s.sStop : centreHz_ + half;
            a = std::max(a, centreHz_ - half); b = std::min(b, centreHz_ + half);
            if (b - a < 1000) continue;
            const int bins = s.degrade >= 2 ? std::max(64, s.bins / 2) : s.bins;
            wf_.row(a - centreHz_, b - centreHz_, bins, s.rowDb);
            refreshView(s);
            if (s.rv.active()) maskSpectrumRow(s.rowDb, a, b, s.rv);   // what the host does not show is replaced by the noise floor before it is scaled and sent
            quantizeRow(s.rowDb, s.sc, s.rowQ);
            enqueue(s, OutMsg{spectrumFrame(s.sSeq++, a, b, s.sc.floorDb, s.sc.topDb, s.rowQ), true, 3});
            s.cpuSec += threadCpuSeconds() - t0;
        }
    }
}

// ---------------------------------------------------------------- queues
void WebHub::enqueue(Session& s, OutMsg m) {
    std::lock_guard<std::mutex> g(s.qm);
    const size_t cap = qCap_.load();
    const size_t sz = m.data.size();
    if (m.prio >= 2 && s.qBytes + sz > cap) {
        // make room: spectrum rows go first, then the oldest audio; control messages and events are never dropped
        for (int p = 3; p >= 2 && s.qBytes + sz > cap; p--) {
            for (auto it = s.q.begin(); it != s.q.end() && s.qBytes + sz > cap;) {
                if (it->prio == p && (p == 3 || m.prio == 2)) { s.qBytes -= it->data.size(); it = s.q.erase(it); s.dropped++; }
                else ++it;
            }
        }
        if (s.qBytes + sz > cap) { s.dropped++; return; }
    }
    if (m.prio == 3) {   // never more than two rows waiting: a listener who cannot keep up sees fewer rows, not older ones
        int rows = 0;
        for (auto& x : s.q) if (x.prio == 3) rows++;
        if (rows >= 2) {
            for (auto it = s.q.begin(); it != s.q.end(); ++it) if (it->prio == 3) { s.qBytes -= it->data.size(); s.q.erase(it); s.dropped++; break; }
        }
    }
    if (m.prio < 2 && s.qBytes > 4 * cap) { if (s.kick.empty()) s.kick = "slow_client"; return; }
    s.qBytes += sz;
    s.bytes += sz;
    s.q.push_back(std::move(m));
}

bool WebHub::drain(int id, std::vector<OutMsg>& out, std::string& kickReason) {
    std::shared_ptr<Session> s;
    { std::lock_guard<std::mutex> g(mu_); auto it = sessions_.find(id); if (it != sessions_.end()) s = it->second; }
    if (!s) { kickReason = "gone"; return false; }
    std::lock_guard<std::mutex> g(s->qm);
    while (!s->q.empty()) { out.push_back(std::move(s->q.front())); s->q.pop_front(); }
    s->qBytes = 0;
    if (!s->kick.empty()) { kickReason = s->kick; return false; }
    return true;
}

// ---------------------------------------------------------------- sessions
std::string WebHub::ipShown(const std::string& ip) const {
    if (logIp_) return ip;
    // not the address: a short, stable, non-reversible tag so the host can tell sessions of one address apart
    uint32_t h = 2166136261u;
    for (unsigned char c : ip) { h ^= c; h *= 16777619u; }
    static const uint32_t salt = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    h ^= salt; h *= 16777619u;
    char b[16];
    snprintf(b, sizeof b, "#%06x", h & 0xFFFFFF);
    return b;
}

int WebHub::admit(const AdmitRequest& r, std::string& code, std::string& why) {
    std::lock_guard<std::mutex> g(mu_);
    code = err::Busy;
    if ((int)sessions_.size() >= lim_.maxUsers) { why = "the server is full, try again later"; rejected_++; return 0; }
    int fromIp = 0;
    for (auto& kv : sessions_) if (kv.second->ip == r.ip) fromIp++;
    if (fromIp >= lim_.maxPerIp) { why = "too many sessions from your address"; rejected_++; return 0; }
    if (dspCpuPct_ >= lim_.cpuCapPct) { why = "the server is busy (processor), try again later"; rejected_++; return 0; }
    if (kbpsTotal_ >= 0.9 * lim_.maxKbpsTotal) { why = "the server is busy (bandwidth), try again later"; rejected_++; return 0; }
    code.clear();
    auto s = std::make_shared<Session>();
    s->id = nextId_++;
    if (nextId_ > 1000000000) nextId_ = 1;
    s->cls = r.cls; s->name = r.name; s->label = r.label; s->ip = r.ip; s->maxMinutes = r.maxMinutes;
    s->created = s->lastAction = wallSeconds();
    // start on the centre of the capture, NFM (the host's mode list decides what is allowed)
    s->freqHz = pubCentre_.load();
    s->haveFreq = pubRate_.load() > 0;
    {
        std::lock_guard<std::mutex> pg(polMu_);
        if (!policy_.demodAllowed(s->rx.demod)) for (int d = 0; d < (int)Demod::Count; d++) if (policy_.demodAllowed((Demod)d)) { s->rx.demod = (Demod)d; break; }
    }
    defaultPassband(s->rx.demod, s->rx.lowHz, s->rx.highHz);
    {   // start inside what the host allows (the centre of the capture may be outside)
        std::lock_guard<std::mutex> pg(polMu_);
        const RangeView rv = ranges_.view(rangeClassOf(s->cls));
        double f = 0;
        if (rv.active() && s->haveFreq && snapFreq(rv, s->rx.demod, s->freqHz, s->rx.lowHz, s->rx.highHz, pubCentre_.load(), pubRate_.load(), f)) s->freqHz = f;
    }
    sessions_[s->id] = s;
    admitted_++;
    logLine("web: session " + std::to_string(s->id) + " joined (" + userClassName(s->cls) + (logIp_ ? ", " + r.ip : std::string()) + ")");
    return s->id;
}

void WebHub::dropSession(int id, const std::string& reason) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    std::set<std::string> subs;
    { std::lock_guard<std::mutex> g(it->second->m); subs = it->second->subs; }
    for (const auto& k : subs) {
        auto d = decs_.find(k);
        if (d == decs_.end()) continue;
        bool empty;
        { std::lock_guard<std::mutex> g(d->second->m); d->second->subs.erase(id); empty = d->second->subs.empty(); }
        if (empty) decs_.erase(d);
    }
    logLine("web: session " + std::to_string(id) + " left (" + reason + ")");
    sessions_.erase(it);
}

void WebHub::leave(int id) { std::lock_guard<std::mutex> g(mu_); dropSession(id, "closed"); }

bool WebHub::kick(int id, const std::string& reason) {
    std::shared_ptr<Session> s;
    { std::lock_guard<std::mutex> g(mu_); auto it = sessions_.find(id); if (it != sessions_.end()) s = it->second; }
    if (!s) return false;
    std::lock_guard<std::mutex> g(s->qm);
    if (s->kick.empty()) { s->kick = reason.empty() ? "kicked" : reason; kicked_++; }
    return true;
}

int WebHub::userCount() const { std::lock_guard<std::mutex> g(mu_); return (int)sessions_.size(); }

void WebHub::error(int id, const std::string& code, const std::string& why) {
    std::shared_ptr<Session> s;
    { std::lock_guard<std::mutex> g(mu_); auto it = sessions_.find(id); if (it != sessions_.end()) s = it->second; }
    if (s) sendError(*s, code, why);
}

// needs only the session's queue lock: safe to call with mu_ or s.m held
void WebHub::sendError(Session& s, const std::string& code, const std::string& why) {
    enqueue(s, OutMsg{"{\"type\":\"error\",\"code\":\"" + jsonEsc(code) + "\",\"message\":\"" + jsonEsc(why) + "\"}", false, 0});
}

static const char* agcName(AgcMode a) { return a == AgcMode::Off ? "off" : a == AgcMode::Fast ? "fast" : "slow"; }

void WebHub::sendState(Session& s) {
    // under s.m
    std::ostringstream o;
    o << "{\"type\":\"state\",\"freq_hz\":" << jsonNumber(s.freqHz) << ",\"mode\":\"" << demodName(s.rx.demod) << "\",\"low_hz\":" << jsonNumber(s.rx.lowHz)
      << ",\"high_hz\":" << jsonNumber(s.rx.highHz) << ",\"squelch_db\":" << jsonNumber(s.rx.squelchDb) << ",\"agc\":\"" << agcName(s.rx.agc)
      << "\",\"gain_db\":" << jsonNumber(s.rx.gainDb) << ",\"deemph_us\":" << jsonNumber(s.rx.deemphUs)
      << ",\"audio\":{\"on\":" << (s.audioOn ? "true" : "false") << ",\"codec\":\"" << (s.codec == AudioCodec::Pcm16 ? "pcm16" : "adpcm") << "\",\"rate\":" << s.arate << "}"
      << ",\"spectrum\":{\"on\":" << (s.specOn ? "true" : "false") << ",\"bins\":" << s.bins << ",\"fps\":" << s.fps;
    if (s.span) o << ",\"start_hz\":" << jsonNumber(s.sStart) << ",\"stop_hz\":" << jsonNumber(s.sStop);
    o << "},\"subscriptions\":[";
    bool first = true;
    for (const auto& k : s.subs) { o << (first ? "" : ",") << '"' << jsonEsc(k) << '"'; first = false; }
    o << "],\"degrade\":" << s.degrade << "}";
    enqueue(s, OutMsg{o.str(), false, 0});
}

void WebHub::handle(int id, const ClientMsg& m) {
    std::shared_ptr<Session> sp;
    { std::lock_guard<std::mutex> g(mu_); auto it = sessions_.find(id); if (it != sessions_.end()) sp = it->second; }
    if (!sp) return;
    Session& s = *sp;
    const HubLimits L = limits();
    const double centre = pubCentre_.load(), rate = pubRate_.load();
    auto fail = [&](const char* code, const std::string& why) { sendError(s, code, why); };
    switch (m.type) {
    case ClientMsg::Hello: fail(err::BadMessage, "already greeted"); return;
    case ClientMsg::Ping: {
        enqueue(s, OutMsg{"{\"type\":\"pong\",\"t\":" + jsonNumber(m.t) + "}", false, 0});
        return;
    }
    case ClientMsg::Bye: kick(id, "bye"); return;
    case ClientMsg::Tx: {
        if (!allowTx()) { fail(err::Forbidden, "not available to web listeners"); return; }
        if (!txHandler_) { fail(err::Forbidden, "this host has no transmitter for web listeners"); return; }
        const std::string reply = txHandler_(id, m.txType, m.txRaw);
        if (!reply.empty()) enqueue(s, OutMsg{reply, false, 0});
        return;
    }
    case ClientMsg::Subscribe: subscribe(sp, m); return;
    case ClientMsg::Unsubscribe: {
        std::string key = m.decoder;
        if (m.hasDecFreq) key += "@" + std::to_string((long long)std::llround(m.decFreqHz / 1000.0)) + "k";
        { std::lock_guard<std::mutex> g(mu_); unsubscribe(s, key); }
        std::lock_guard<std::mutex> g(s.m);
        sendState(s);
        return;
    }
    default: break;
    }
    std::lock_guard<std::mutex> g(s.m);
    s.lastAction = wallSeconds();
    if (m.type == ClientMsg::Tune) {
        RxSettings r = s.rx;
        if (m.hasDemod) {
            bool ok;
            { std::lock_guard<std::mutex> pg(polMu_); ok = policy_.demodAllowed(m.demod); }
            if (!ok) { fail(err::NotAllowed, "the host has turned this mode off"); return; }
            if (m.demod != r.demod && !m.hasPass) defaultPassband(m.demod, r.lowHz, r.highHz);
            r.demod = m.demod;
        }
        if (m.hasPass) {
            double lo, hi;
            passbandLimits(r.demod, lo, hi);
            if (m.lowHz < lo || m.highHz > hi) { fail(err::OutOfRange, "passband wider than this mode allows"); return; }
            r.lowHz = m.lowHz; r.highHz = m.highHz;
        }
        // the host's frequency ranges: frequency, mode and passband together must be allowed (the whole passband plus a guard margin inside one allowed range)
        double snapF = 0;
        bool useSnap = false;
        refreshView(s);
        if (s.rv.active() && (m.hasFreq || m.hasDemod || m.hasPass) && (m.hasFreq || s.haveFreq)) {
            const double nf = m.hasFreq ? m.freqHz : s.freqHz;
            const RangeVerdict rvd = checkPassband(s.rv, r.demod, nf, r.lowHz, r.highHz, centre, rate);
            if (rvd != RangeVerdict::Ok) {
                double nearF = 0;
                const bool have = snapFreq(s.rv, r.demod, nf, r.lowHz, r.highHz, centre, rate, nearF);
                if (s.rclamp && have) { snapF = nearF; useSnap = true; }
                else {
                    fail(err::ForbiddenRange, std::string(verdictText(rvd)) + (have ? "; nearest allowed frequency: " + jsonNumber(nearF) + " Hz" : std::string()));
                    return;
                }
            }
        }
        if (m.hasFreq) {
            if (!(rate > 0)) { fail(err::OutOfRange, "the receiver is not running"); return; }
            if (std::fabs(m.freqHz - centre) > rate * 0.48) { fail(err::OutOfRange, "outside the capture (" + jsonNumber(centre - rate * 0.48) + " .. " + jsonNumber(centre + rate * 0.48) + " Hz)"); return; }
            s.freqHz = m.freqHz; s.haveFreq = true;
        }
        if (useSnap) { s.freqHz = snapF; s.haveFreq = true; }
        s.rx = r;
        s.dirty = true;
    } else if (m.type == ClientMsg::Rx) {
        if (m.hasSquelch) s.rx.squelchDb = m.squelchDb;
        if (m.hasAgc) s.rx.agc = m.agc;
        if (m.hasGain) s.rx.gainDb = m.gainDb;
        if (m.hasDeemph) s.rx.deemphUs = m.deemphUs;
        s.dirty = true;
    } else if (m.type == ClientMsg::Audio) {
        if (m.on) {
            const double kbps = (m.codec == AudioCodec::Pcm16 ? 16.0 : 4.0) * m.rateHz / 1000.0 + 2.0;
            if (kbps > L.maxKbpsUser * 0.8) { fail(err::OutOfRange, "too much for this server's bandwidth per listener: use adpcm or a lower rate"); return; }
        }
        if (m.rateHz != s.arate) s.dirty = true;
        s.audioOn = m.on; s.codec = m.codec; s.arate = m.rateHz;
        s.ad = AdpcmState();
        s.pend.clear();
    } else if (m.type == ClientMsg::Spectrum) {
        if (m.bins > L.maxBins) { fail(err::OutOfRange, "at most " + std::to_string(L.maxBins) + " bins"); return; }
        if (m.fps > L.maxFps) { fail(err::OutOfRange, "at most " + std::to_string(L.maxFps) + " rows a second"); return; }
        if (m.on && m.hasSpan && rate > 0 && (m.stopHz < centre - rate * 0.5 || m.startHz > centre + rate * 0.5)) { fail(err::OutOfRange, "the span is outside the capture"); return; }
        s.specOn = m.on; s.bins = m.bins; s.fps = m.fps; s.span = m.hasSpan; s.sStart = m.startHz; s.sStop = m.stopHz;
        s.nextSpec = 0;
    }
    sendState(s);
}

void WebHub::unsubscribe(Session& s, const std::string& key) {
    // under mu_
    { std::lock_guard<std::mutex> g(s.m); s.subs.erase(key); }
    auto d = decs_.find(key);
    if (d == decs_.end()) return;
    bool empty;
    { std::lock_guard<std::mutex> g(d->second->m); d->second->subs.erase(s.id); empty = d->second->subs.empty(); }
    if (empty) decs_.erase(d);
}

void WebHub::subscribe(const std::shared_ptr<Session>& sp, const ClientMsg& m) {
    Session& s = *sp;
    bool allowed;
    { std::lock_guard<std::mutex> pg(polMu_); allowed = policy_.decoderAllowed(m.decoder); }
    if (!allowed) { sendError(s, err::NotAllowed, "the host has not enabled this decoder for web listeners"); return; }
    std::unique_lock<std::mutex> g(mu_);
    auto f = factories_.find(m.decoder);
    if (f == factories_.end()) { sendError(s, err::NotAllowed, "this server has no such decoder"); return; }
    if (f->second.needsFreq && !m.hasDecFreq) { sendError(s, err::BadMessage, "this decoder needs freq_hz"); return; }
    std::string key = m.decoder;
    double fz = 0;
    if (f->second.needsFreq) {
        const double centre = pubCentre_.load(), rate = pubRate_.load();
        if (!(rate > 0) || std::fabs(m.decFreqHz - centre) > rate * 0.48) { sendError(s, err::OutOfRange, "outside the capture"); return; }
        fz = std::round(m.decFreqHz / 1000.0) * 1000.0;
        key += "@" + std::to_string((long long)std::llround(m.decFreqHz / 1000.0)) + "k";
    }
    if (f->second.needsFreq) {   // the host's frequency ranges: the decoder's whole slice must be visible to this listener
        std::lock_guard<std::mutex> sg(s.m);
        refreshView(s);
        const double half = std::max(f->second.rateHz, 25000.0) * 0.5;
        const RangeVerdict rvd = checkWindow(s.rv, fz - half, fz + half);
        if (rvd != RangeVerdict::Ok) { sendError(s, err::ForbiddenRange, verdictText(rvd)); return; }
    }
    {
        std::lock_guard<std::mutex> sg(s.m);
        if (s.subs.count(key)) { sendState(s); return; }
        if (s.subs.size() >= 4) { sendError(s, err::OutOfRange, "at most 4 decoders a listener"); return; }
    }
    auto it = decs_.find(key);
    if (it == decs_.end()) {
        if ((int)decs_.size() >= lim_.maxDecoders) { sendError(s, err::Busy, "the server runs as many decoders as it can"); return; }
        auto d = std::make_shared<SharedDec>();
        d->key = key; d->kind = m.decoder; d->freqHz = fz; d->rate = f->second.rateHz; d->needsFreq = f->second.needsFreq;
        d->dec = f->second.make ? f->second.make(fz) : nullptr;
        if (!d->dec) { sendError(s, err::Busy, "the decoder could not be started"); return; }
        it = decs_.emplace(key, d).first;
    }
    std::vector<std::string> snap;
    {
        std::lock_guard<std::mutex> dg(it->second->m);
        it->second->subs.insert(s.id);
        if (it->second->dec) it->second->dec->snapshot(snap);   // a late listener gets the current table / picture, not only what comes next
    }
    for (const auto& e : snap) {
        if (e.size() > 65536) continue;
        enqueue(s, OutMsg{"{\"type\":\"event\",\"decoder\":\"" + jsonEsc(key) + "\",\"data\":" + e + "}", false, 1});
    }
    std::lock_guard<std::mutex> sg(s.m);
    s.subs.insert(key);
    s.lastAction = wallSeconds();
    sendState(s);
}

// ---------------------------------------------------------------- frequency ranges
void WebHub::refreshView(Session& s) {
    // under s.m (lock order: s.m, then polMu_)
    const unsigned g = rangeGen_.load();
    if (s.rvGen == g) return;
    std::lock_guard<std::mutex> pg(polMu_);
    s.rv = ranges_.view(rangeClassOf(s.cls));
    s.rclamp = ranges_.clampMode();
    s.rvGen = g;
}

void WebHub::sendRanges(Session& s) {
    enqueue(s, OutMsg{"{\"type\":\"ranges\",\"ranges\":" + rangesJson(s.rv, s.rclamp) + "}", false, 0});
}

void WebHub::rangesChanged() {
    { std::lock_guard<std::mutex> pg(polMu_); rangeGen_++; }
    std::vector<std::shared_ptr<Session>> ss;
    { std::lock_guard<std::mutex> g(mu_); for (auto& kv : sessions_) ss.push_back(kv.second); }
    for (auto& sp : ss) {
        RangeView rv;
        std::set<std::string> subs;
        {
            std::lock_guard<std::mutex> g(sp->m);
            refreshView(*sp);
            sp->dirty = true;          // the DSP thread re-checks the tuning and silences it when it is no longer allowed
            sp->rangeNote = false;
            sendRanges(*sp);
            rv = sp->rv; subs = sp->subs;
        }
        if (!rv.active() || subs.empty()) continue;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const auto& key : subs) {
                auto d = decs_.find(key);
                if (d == decs_.end() || !d->second->needsFreq) continue;
                const double half = std::max(d->second->rate, 25000.0) * 0.5;
                const RangeVerdict v = checkWindow(rv, d->second->freqHz - half, d->second->freqHz + half);
                if (v == RangeVerdict::Ok) continue;
                sendError(*sp, err::ForbiddenRange, std::string("decoder ") + key + " stopped: " + verdictText(v));
                unsubscribe(*sp, key);
            }
        }
        std::lock_guard<std::mutex> g(sp->m);
        sendState(*sp);
    }
}

std::string WebHub::welcomeJson(int id) {
    std::shared_ptr<Session> s;
    HubLimits L;
    std::vector<DecoderFactory> fs;
    std::string name, loc, ant;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) s = it->second;
        L = lim_;
        for (auto& kv : factories_) fs.push_back(kv.second);
        name = srvName_; loc = srvLoc_; ant = srvAnt_;
    }
    if (!s) return "{}";
    std::ostringstream o;
    o << "{\"type\":\"welcome\",\"proto\":" << kProtoVersion << ",\"session\":" << id << ",\"class\":\"" << userClassName(s->cls) << "\""
      << ",\"server\":{\"name\":\"" << jsonEsc(name) << "\",\"location\":\"" << jsonEsc(loc) << "\",\"antenna\":\"" << jsonEsc(ant) << "\"}"
      << ",\"capture\":{\"centre_hz\":" << jsonNumber(pubCentre_.load()) << ",\"rate_hz\":" << jsonNumber(pubRate_.load()) << "}"
      << ",\"limits\":{\"max_bins\":" << L.maxBins << ",\"max_fps\":" << L.maxFps << ",\"max_kbps\":" << L.maxKbpsUser << ",\"idle_min\":" << L.idleMin
      << ",\"time_limit_min\":" << (s->maxMinutes ? s->maxMinutes : s->cls == UserClass::Public ? L.publicMaxMin : L.memberMaxMin)
      << ",\"msg_per_sec\":20,\"max_message\":" << kMaxClientMsg << "}"
      << ",\"audio\":{\"codecs\":[\"adpcm\",\"pcm16\"],\"rates\":[8000,12000,16000,24000,48000]},\"demods\":[";
    bool first = true;
    {
        std::lock_guard<std::mutex> pg(polMu_);
        for (int d = 0; d < (int)Demod::Count; d++) if (policy_.demodAllowed((Demod)d)) { o << (first ? "" : ",") << '"' << demodName((Demod)d) << '"'; first = false; }
        o << "],\"decoders\":[";
        first = true;
        for (auto& f : fs) {
            if (!policy_.decoderAllowed(f.kind)) continue;
            o << (first ? "" : ",") << "{\"kind\":\"" << jsonEsc(f.kind) << "\",\"name\":\"" << jsonEsc(f.name) << "\",\"needs_freq\":" << (f.needsFreq ? "true" : "false") << "}";
            first = false;
        }
    }
    o << "]";
    RangeView rv;
    bool clampMode;
    { std::lock_guard<std::mutex> pg(polMu_); rv = ranges_.view(rangeClassOf(s->cls)); clampMode = ranges_.clampMode(); }
    if (rv.active()) o << ",\"ranges\":" << rangesJson(rv, clampMode);   // only when the host set ranges: older clients and open servers see no change
    o << "}";
    return o.str();
}

std::string WebHub::infoJson() const {
    std::lock_guard<std::mutex> g(mu_);
    std::ostringstream o;
    o << "{\"proto\":" << kProtoVersion << ",\"name\":\"" << jsonEsc(srvName_) << "\",\"location\":\"" << jsonEsc(srvLoc_) << "\",\"antenna\":\"" << jsonEsc(srvAnt_)
      << "\",\"users\":" << sessions_.size() << ",\"max_users\":" << lim_.maxUsers
      << ",\"capture\":{\"centre_hz\":" << jsonNumber(pubCentre_.load()) << ",\"rate_hz\":" << jsonNumber(pubRate_.load()) << "}}";
    return o.str();
}

std::string WebHub::usersJson() const {
    std::vector<std::shared_ptr<Session>> ss;
    { std::lock_guard<std::mutex> g(mu_); for (auto& kv : sessions_) ss.push_back(kv.second); }
    const double now = wallSeconds();
    std::ostringstream o;
    o << "{\"users\":[";
    bool first = true;
    for (auto& s : ss) {
        std::lock_guard<std::mutex> g(s->m);
        o << (first ? "" : ",") << "{\"id\":" << s->id << ",\"name\":\"" << jsonEsc(s->name) << "\",\"class\":\"" << userClassName(s->cls) << "\",\"label\":\"" << jsonEsc(s->label)
          << "\",\"address\":\"" << jsonEsc(ipShown(s->ip)) << "\",\"seconds\":" << (long long)(now - s->created) << ",\"idle_seconds\":" << (long long)(now - s->lastAction)
          << ",\"freq_hz\":" << jsonNumber(s->freqHz) << ",\"mode\":\"" << demodName(s->rx.demod) << "\",\"audio\":" << (s->audioOn ? "true" : "false")
          << ",\"spectrum\":" << (s->specOn ? "true" : "false") << ",\"kbps\":" << jsonNumber(std::round(s->kbps)) << ",\"cpu_pct\":" << jsonNumber(std::round(s->cpuPct * 10) / 10)
          << ",\"degrade\":" << s->degrade << ",\"dropped\":" << s->dropped << ",\"decoders\":" << s->subs.size() << "}";
        first = false;
    }
    o << "]}";
    return o.str();
}

std::string WebHub::statsJson() const {
    std::lock_guard<std::mutex> g(mu_);
    std::ostringstream o;
    o << "{\"users\":" << sessions_.size() << ",\"max_users\":" << lim_.maxUsers << ",\"cpu_pct\":" << jsonNumber(std::round(dspCpuPct_.load() * 10) / 10)
      << ",\"cpu_cap_pct\":" << jsonNumber(lim_.cpuCapPct) << ",\"kbps\":" << jsonNumber(std::round(kbpsTotal_)) << ",\"max_kbps\":" << lim_.maxKbpsTotal
      << ",\"admitted\":" << admitted_.load() << ",\"rejected\":" << rejected_.load() << ",\"kicked\":" << kicked_.load()
      << ",\"iq_dropped\":" << inDropped_.load() << ",\"capture\":{\"centre_hz\":" << jsonNumber(pubCentre_.load()) << ",\"rate_hz\":" << jsonNumber(pubRate_.load())
      << "},\"decoders\":[";
    bool first = true;
    for (auto& kv : decs_) { o << (first ? "" : ",") << "{\"key\":\"" << jsonEsc(kv.first) << "\",\"listeners\":" << kv.second->subs.size() << "}"; first = false; }
    o << "]}";
    return o.str();
}

void WebHub::tick() {
    const double now = wallSeconds();
    std::vector<std::shared_ptr<Session>> ss;
    HubLimits L;
    { std::lock_guard<std::mutex> g(mu_); for (auto& kv : sessions_) ss.push_back(kv.second); L = lim_; }
    double total = 0;
    std::shared_ptr<Session> newestPublic;
    for (auto& sp : ss) {
        Session& s = *sp;
        std::string kickWhy;
        {
            std::lock_guard<std::mutex> g(s.m);
            const double dt = std::max(0.2, now - (lastTick_ > 0 ? lastTick_ : now - 1.0));
            s.cpuPct = (s.cpuSec - s.cpuMark) / dt * 100.0;
            s.cpuMark = s.cpuSec;
            {
                std::lock_guard<std::mutex> q(s.qm);
                s.kbps = (double)(s.bytes - s.bytesMark) * 8.0 / 1000.0 / dt;
                s.bytesMark = s.bytes;
            }
            total += s.kbps;
            const bool over = s.cpuPct > L.sessionCpuPct || s.kbps > L.maxKbpsUser;
            if (over) {
                if (s.degrade < 3) s.degrade++;
                if (s.overSince == 0) s.overSince = now;
                if (s.degrade >= 3 && now - s.overSince > 10) kickWhy = s.cpuPct > L.sessionCpuPct ? "cpu_budget" : "bandwidth_budget";
            } else {
                s.overSince = 0;
                if (s.degrade > 0 && s.cpuPct < 0.5 * L.sessionCpuPct && s.kbps < 0.5 * L.maxKbpsUser) s.degrade--;
            }
            const int limit = s.maxMinutes ? s.maxMinutes : s.cls == UserClass::Public ? L.publicMaxMin : L.memberMaxMin;
            if (limit > 0 && now - s.created > limit * 60.0) kickWhy = "time_limit";
            if (L.idleMin > 0 && now - s.lastAction > L.idleMin * 60.0) kickWhy = "idle";
            if (s.cls == UserClass::Public && (!newestPublic || s.created > newestPublic->created)) newestPublic = sp;
        }
        if (!kickWhy.empty()) kick(s.id, kickWhy);
    }
    { std::lock_guard<std::mutex> g(mu_); kbpsTotal_ = total; }
    lastTick_ = now;
    // the whole server over its processor or bandwidth cap for a while: the newest public listener makes room
    if (dspCpuPct_ > L.cpuCapPct * 1.1 || total > L.maxKbpsTotal * 1.1) {
        if (overSince_ == 0) overSince_ = now;
        if (now - overSince_ > 10 && newestPublic) { kick(newestPublic->id, "server_busy"); overSince_ = now; }
    } else overSince_ = 0;
}

} // namespace onair_web
