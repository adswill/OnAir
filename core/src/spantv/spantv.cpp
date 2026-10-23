#include "dect2/common/thread_name.h"
#include "dect2/engine/spantv/spantv.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>
#include <tuple>

namespace dect2 {

namespace {
constexpr int kNone = INT_MIN / 2;
constexpr double kQueueSec = 0.2;         // signal time that may wait for a receiver (a count of blocks was 9 ms at 60 Msps: any hiccup of a receiver lost samples)
constexpr size_t kQueueMin = 1u << 19;    // samples, at least
constexpr int kAbsentEvals = 8;           // evaluations (a quarter second each) without a signal before an idle receiver is dropped
inline uint64_t nowNs() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

using Block = std::shared_ptr<const std::vector<cf32>>;

struct SpanTv::Slot : TsOut {
    ChannelSlotPos pos;            // index = the signal's id, offsetHz = its centre
    SignalCand cand;
    SpanTv* owner = nullptr;
    ChannelFilter filter;
    std::unique_ptr<TvRx> rx;
    // transport stream
    std::mutex tsMu;
    TsDemux demux;
    TsSnapshot snap;
    std::vector<uint8_t> frameBuf;
    // the receiver's thread
    std::mutex qMu;
    std::condition_variable qCv;
    std::deque<Block> q;
    size_t qN = 0;   // samples in q
    bool stop = false, busy = false;
    std::thread th;
    std::atomic<uint64_t> busyNs{0}, dropped{0}, sigSamples{0};
    // polling (the owner's thread)
    uint64_t telSeq = 0;
    RxTelemetry tel;
    bool haveTel = false, everLocked = false;
    uint64_t cpuBusy0 = 0, cpuSig0 = 0;
    float cpuPct = 0;
    bool cpuMeasured = false;
    int absent = 0;
    bool occupied = true;
    double startedAt = 0;

    ~Slot() override { rx.reset(); }   // the receiver's threads call back into this slot: they go first
    bool lockedNow() const { return haveTel && (tel.state >= 2 || tel.dvbt.fecSync || tel.dataValid); }

    void packets(const uint8_t* pk, size_t n) override {
        std::lock_guard<std::mutex> lk(tsMu);
        for (size_t i = 0; i < n; i++) demux.feed(pk + i * 188);
        frameBuf.insert(frameBuf.end(), pk, pk + n * 188);
    }
    void loss() override { std::lock_guard<std::mutex> lk(tsMu); demux.markLoss(); }
    void burst(double secs) override {
        std::vector<uint8_t> out;
        TsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(tsMu);
            demux.advance(secs);
            snap = demux.snapshot();
            s = snap;
            out.swap(frameBuf);
        }
        owner->deliver(pos.index, out.data(), out.size() / 188, secs, s);
    }
    void run() {
        std::vector<cf32> o;
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(qMu);
                qCv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop) return;
                b = std::move(q.front()); q.pop_front(); qN -= b->size();
                busy = true;
            }
            qCv.notify_all();
            const uint64_t t0 = nowNs();
            o.clear();
            filter.process(b->data(), b->size(), o);
            if (!o.empty()) rx->feed(o.data(), o.size());
            busyNs += nowNs() - t0;
            { std::lock_guard<std::mutex> lk(qMu); busy = false; }
            qCv.notify_all();
        }
    }
};

SpanTv::SpanTv() = default;
SpanTv::~SpanTv() { reset(); }

void SpanTv::setSink(Sink s) { std::lock_guard<std::mutex> lk(sinkMu_); sink_ = std::move(s); }

void SpanTv::deliver(int slot, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap) {
    if (slot != sel_.load() || !n) return;
    Sink f;
    { std::lock_guard<std::mutex> lk(sinkMu_); f = sink_; }
    if (f) f(slot, pk, n, secs, snap);
}

bool SpanTv::configure(const SpanTvOptions& o) {
    reset();
    o_ = o;
    ad_ = makeTvAdapter(o.stdMode);
    if (!ad_ || o.spanRate <= 0) return false;
    raster_ = ad_->raster();
    gridOff_ = o.gridOffsetHz; gridFixed_ = !o.autoGrid;
    queueLimit_ = std::max(kQueueMin, (size_t)(o.spanRate * kQueueSec));
    sel_ = kNone;
    pinned_.clear(); decodeOff_.clear(); budgetStopAt_.clear();
    if (raster_) {
        slots_ = spanSlots(o.spanRate, o.widthMhz * 1e6, gridOff_);
        scan_.configure(o.spanRate);
        return !slots_.empty() || o.autoGrid;
    }
    // bins: the narrowest signal of the standard at least 40 bins wide, 4096..32768
    const double want = o.spanRate / std::max(1.0, ad_->minBwHz() / 40.0);
    ana_.configure(o.spanRate, (int)std::min(32768.0, std::max(4096.0, want)), 4);
    trk_.reset();
    trk_.setBinHz(ana_.binHz());
    return true;
}

void SpanTv::stopSlot(size_t i) {
    Slot& s = *live_[i];
    { std::lock_guard<std::mutex> lk(s.qMu); s.stop = true; }
    s.qCv.notify_all();
    if (s.th.joinable()) s.th.join();
    live_.erase(live_.begin() + (long)i);
}

void SpanTv::reset() {
    std::vector<std::unique_ptr<Slot>> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dead.swap(live_);
    }
    for (auto& s : dead) {
        { std::lock_guard<std::mutex> lk(s->qMu); s->stop = true; }
        s->qCv.notify_all();
        if (s->th.joinable()) s->th.join();
    }
    dead.clear();   // receivers go (their own threads are joined in their destructors)
    scan_.reset();
    measured_.clear();
    if (!raster_) { ana_.reset(); trk_.reset(); }
    lastCands_.clear();
    sinceEval_ = 0;
    fed_ = 0;
    sel_ = kNone;
    lastSelPolled_ = kNone;
}

SpanTv::Slot* SpanTv::find(int slot) {
    for (auto& s : live_) if (s->pos.index == slot) return s.get();
    return nullptr;
}

bool SpanTv::startSlot(int id, const SignalCand& c) {
    auto s = std::make_unique<Slot>();
    s->pos.index = id; s->pos.offsetHz = c.centerHz; s->cand = c; s->owner = this;
    s->startedAt = now();
    const double outRate = ad_->nativeRate(c, o_);
    if (outRate <= 0 || !s->filter.configure(o_.spanRate, c.centerHz, outRate)) return false;
    s->rx = ad_->make(c, outRate, o_);
    if (!s->rx) return false;
    Slot* raw = s.get();
    s->rx->setOut(raw);
    s->th = dect2::namedThread("onair-mtv", [raw] { raw->run(); });
    std::lock_guard<std::mutex> lk(mu_);
    live_.push_back(std::move(s));
    if (sel_.load() == kNone) sel_ = id;
    return true;
}

void SpanTv::evaluate() {
    if (raster_) evaluateRaster(); else evaluateFree();
}

// The plan for the entries (what runs now and what is wanted), then the receivers that stop and those that start
void SpanTv::applyPlan(const std::vector<PoolEntry>& entries, const std::map<int, SignalCand>& cands) {
    PoolLimits lim;
    lim.maxReceivers = std::max(1, o_.maxChannels);
    lim.budgetCores = budgetCores();
    lim.decodeAll = o_.decodeAll;
    const PoolPlan plan = planPool(entries, lim);
    for (int id : plan.stop) {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < live_.size(); i++) if (live_[i]->pos.index == id) { stopSlot(i); break; }
        budgetStopAt_[id] = now();
        if (sel_.load() == id) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
    }
    for (int id : plan.start) {
        auto it = cands.find(id);
        if (it == cands.end()) continue;
        { std::lock_guard<std::mutex> lk(mu_); if (find(id)) continue; }
        startSlot(id, it->second);
    }
}

void SpanTv::evaluateRaster() {
    if (scan_.frames() < 12) return;
    if (!gridFixed_) {   // the capture is centred on a channel or between two: whichever fits more signals
        const double w = o_.widthMhz * 1e6;
        double best = -1, bestOff = 0;
        for (double off : {0.0, 0.5 * w}) {
            const auto sl = spanSlots(o_.spanRate, w, off);
            const auto m = scan_.measure(sl, w, o_.thresholdDb);
            double score = 0;
            for (const auto& p : m) if (p.occupied) score += 1000.0 + std::min(p.aboveDb, 60.0);
            if (score > best) { best = score; bestOff = off; }
        }
        gridOff_ = bestOff; gridFixed_ = true;
        slots_ = spanSlots(o_.spanRate, w, gridOff_);
    }
    auto m = scan_.measure(slots_, o_.widthMhz * 1e6, o_.thresholdDb);
    {
        std::lock_guard<std::mutex> lk(mu_);
        measured_ = m;
        for (auto& s : live_) {
            bool occ = false;
            for (const auto& p : m) if (p.pos.index == s->pos.index) occ = p.occupied;
            s->occupied = occ;
            s->absent = occ ? 0 : s->absent + 1;
        }
    }
    // drop receivers whose channel has gone and that never locked
    for (;;) {
        std::lock_guard<std::mutex> lk(mu_);
        size_t victim = SIZE_MAX;
        for (size_t i = 0; i < live_.size(); i++)
            if (live_[i]->absent >= kAbsentEvals && !(live_[i]->haveTel && live_[i]->tel.state >= 2)) { victim = i; break; }
        if (victim == SIZE_MAX) break;
        const int idx = live_[victim]->pos.index;
        stopSlot(victim);
        if (sel_.load() == idx) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
    }
    // the pool: the running receivers and the occupied slots
    std::vector<PoolEntry> entries;
    std::map<int, SignalCand> cands;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int sel = sel_.load();
        for (const auto& p : m) {
            Slot* s = find(p.pos.index);
            if (!s && !p.occupied) continue;
            SignalCand c; c.centerHz = p.pos.offsetHz; c.bwHz = c.bw3Hz = o_.widthMhz * 1e6; c.plateauDb = p.levelDb; c.snrDb = p.aboveDb;
            cands[p.pos.index] = c;
            PoolEntry e;
            e.id = p.pos.index; e.running = s != nullptr; e.selected = p.pos.index == sel; e.snrDb = p.aboveDb;
            e.pinned = pinned_.count(e.id) && pinned_[e.id]; e.manualOff = decodeOff_.count(e.id) && decodeOff_[e.id];
            e.locked = s && s->everLocked;
            e.cost = s && s->cpuMeasured ? s->cpuPct / 100.f : ad_->costPerSecond(c);
            e.runSecs = s ? now() - s->startedAt : 0;
            auto b = budgetStopAt_.find(e.id);
            if (b != budgetStopAt_.end()) e.sinceBudgetStop = now() - b->second;
            entries.push_back(e);
        }
        // receivers of slots outside the raster now (the grid moved): keep them as they are
        for (auto& s : live_) if (!cands.count(s->pos.index)) {
            PoolEntry e; e.id = s->pos.index; e.running = true; e.selected = s->pos.index == sel; e.locked = s->everLocked; e.cost = s->cpuPct / 100.f; e.runSecs = 1e9;
            cands[e.id] = s->cand;
            entries.push_back(e);
        }
    }
    applyPlan(entries, cands);
}

void SpanTv::evaluateFree() {
    if (ana_.frames() < 12) return;
    SpanAnalyzer::Params p;
    p.thresholdDb = o_.thresholdDb;
    p.minBwHz = ad_->minBwHz();
    p.maxBwHz = ad_->maxBwHz();
    const auto cands = ana_.detect(p);
    const auto& tracks = trk_.update(cands);
    const double lockSecs = o_.lockSecs > 0 ? o_.lockSecs : 30.0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lastCands_ = cands;
        for (auto& s : live_) {
            const TrackedSignal* t = trk_.find(s->pos.index);
            s->occupied = t && t->misses == 0;
            s->absent = s->occupied ? 0 : s->absent + 1;
            if (t) s->cand.snrDb = t->cand.snrDb;
        }
    }
    // drop receivers whose signal has gone and that are not locked
    for (;;) {
        std::lock_guard<std::mutex> lk(mu_);
        size_t victim = SIZE_MAX;
        for (size_t i = 0; i < live_.size(); i++)
            if (live_[i]->absent >= kAbsentEvals && !live_[i]->lockedNow()) { victim = i; break; }
        if (victim == SIZE_MAX) break;
        const int idx = live_[victim]->pos.index;
        stopSlot(victim);
        if (sel_.load() == idx) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        trk_.prune(kAbsentEvals, [this](int id) { return find(id) != nullptr || sel_.load() == id; });
    }
    std::vector<PoolEntry> entries;
    std::map<int, SignalCand> cm;
    {
        std::lock_guard<std::mutex> lk(mu_);
        int sel = sel_.load();
        if (sel == kNone) {   // nothing chosen yet: the strongest signal is the one that will play
            double best = -1e9;
            for (const auto& t : tracks) if (t.present && !t.cand.clipped && t.cand.snrDb > best) { best = t.cand.snrDb; sel = t.id; }
        }
        for (const auto& t : trk_.tracks()) {
            Slot* s = find(t.id);
            if (!s && (!t.present || t.cand.clipped || t.misses > 0)) continue;
            cm[t.id] = s ? s->cand : t.cand;
            PoolEntry e;
            e.id = t.id; e.running = s != nullptr; e.selected = t.id == sel; e.snrDb = t.cand.snrDb;
            e.pinned = pinned_.count(e.id) && pinned_[e.id]; e.manualOff = decodeOff_.count(e.id) && decodeOff_[e.id];
            e.locked = s && s->everLocked;
            e.noLock = s && !s->everLocked && now() - s->startedAt > lockSecs;
            e.cost = s && s->cpuMeasured ? s->cpuPct / 100.f : ad_->costPerSecond(t.cand);
            e.runSecs = s ? now() - s->startedAt : 0;
            auto b = budgetStopAt_.find(e.id);
            if (b != budgetStopAt_.end()) e.sinceBudgetStop = now() - b->second;
            entries.push_back(e);
        }
    }
    applyPlan(entries, cm);
}

void SpanTv::feed(const cf32* x, size_t n) {
    if (!n || o_.spanRate <= 0 || !ad_) return;
    if (raster_) scan_.feed(x, n); else ana_.feed(x, n);
    fed_ += n;
    sinceEval_ += n;
    if (sinceEval_ >= (size_t)(o_.spanRate * 0.25)) { sinceEval_ = 0; evaluate(); }
    std::vector<Slot*> v;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& s : live_) v.push_back(s.get()); }
    if (v.empty()) return;
    auto blk = std::make_shared<const std::vector<cf32>>(x, x + n);
    for (Slot* s : v) {
        s->sigSamples += n;
        std::unique_lock<std::mutex> lk(s->qMu);
        if (o_.blocking) s->qCv.wait(lk, [&] { return s->qN < queueLimit_ || s->stop; });
        else if (s->qN >= queueLimit_) { s->dropped += n; continue; }
        s->q.push_back(blk);
        s->qN += blk->size();
        lk.unlock();
        s->qCv.notify_all();
    }
}

void SpanTv::wait() {
    std::vector<Slot*> v;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& s : live_) v.push_back(s.get()); }
    for (Slot* s : v) {
        std::unique_lock<std::mutex> lk(s->qMu);
        s->qCv.wait(lk, [&] { return (s->q.empty() && !s->busy) || s->stop; });
    }
}

void SpanTv::flush() {
    wait();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : live_) s->rx->flush();
}

void SpanTv::setSelected(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    if (find(slot)) { sel_ = slot; return; }
    // a known signal without a receiver: it gets one at the next evaluation
    if (raster_) { for (const auto& p : measured_) if (p.pos.index == slot && p.occupied) { sel_ = slot; return; } }
    else if (const TrackedSignal* t = trk_.find(slot)) { if (t->present && !t->cand.clipped) sel_ = slot; }
}

void SpanTv::setPinned(int slot, bool on) { std::lock_guard<std::mutex> lk(mu_); pinned_[slot] = on; }
void SpanTv::setDecode(int slot, bool on) { std::lock_guard<std::mutex> lk(mu_); decodeOff_[slot] = !on; }
void SpanTv::setDecodeAll(bool on) { std::lock_guard<std::mutex> lk(mu_); o_.decodeAll = on; }
void SpanTv::setMaxChannels(int n) { std::lock_guard<std::mutex> lk(mu_); o_.maxChannels = std::max(1, n); }
void SpanTv::setBudget(double cores) { std::lock_guard<std::mutex> lk(mu_); o_.cpuBudgetCores = cores; }

double SpanTv::budgetCores() const {
    if (o_.cpuBudgetCores > 0) return o_.cpuBudgetCores;
    if (o_.cpuBudgetCores < 0) return defaultPoolBudget(std::thread::hardware_concurrency());
    return 0;
}

double SpanTv::poolCores() {
    std::lock_guard<std::mutex> lk(mu_);
    double c = 0;
    for (auto& s : live_) c += s->cpuPct / 100.0;
    return c;
}

bool SpanTv::poll(RxTelemetry& out) {
    std::lock_guard<std::mutex> lk(mu_);
    bool selNew = false;
    const int sel = sel_.load();
    for (auto& sp : live_) {
        Slot& s = *sp;
        RxTelemetry t;
        if (s.rx->telemetry(t, s.telSeq)) {
            s.telSeq = t.seq; s.tel = std::move(t); s.haveTel = true;
            if (s.lockedNow()) s.everLocked = true;
            if (s.pos.index == sel) selNew = true;
        }
        const uint64_t sig = s.sigSamples.load();
        if (sig - s.cpuSig0 >= (uint64_t)o_.spanRate) {   // about a second of signal
            const uint64_t busy = s.busyNs.load();
            const double secs = (double)(sig - s.cpuSig0) / o_.spanRate;
            s.cpuPct = (float)(100.0 * 1e-9 * (double)(busy - s.cpuBusy0) / std::max(secs, 1e-3));
            if (s.cpuBusy0 || s.cpuSig0) s.cpuMeasured = true;   // the first second includes the receiver's start
            s.cpuBusy0 = busy; s.cpuSig0 = sig;
        }
    }
    if (sel != lastSelPolled_) { lastSelPolled_ = sel; selNew = true; }
    if (!selNew) return false;
    Slot* s = find(sel);
    if (!s || !s->haveTel) return false;
    out = s->tel;
    return true;
}

std::vector<TvChannelRow> SpanTv::channels() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TvChannelRow> v;
    const int sel = sel_.load();
    auto fill = [&](TvChannelRow& r) {
        r.stdMode = o_.stdMode;
        r.pinned = pinned_.count(r.slot) && pinned_[r.slot];
        r.decodeOff = decodeOff_.count(r.slot) && decodeOff_[r.slot];
        r.selected = r.slot == sel;
        if (Slot* s = find(r.slot)) {
            r.running = true;
            r.cpuPct = s->cpuPct; r.dropped = s->dropped.load();
            if (s->haveTel) {
                r.state = s->tel.state; r.snrDb = s->tel.dataSnrDb;
                r.locked = s->tel.state >= 2 || s->tel.dvbt.fecSync || s->tel.dataValid;
                s->rx->fillRow(s->tel, r);
            }
            r.noLock = !s->everLocked && now() - s->startedAt > (o_.lockSecs > 0 ? o_.lockSecs : 30.0) && !raster_;
            std::lock_guard<std::mutex> tk(s->tsMu);
            r.services = (int)s->snap.services.size();
            r.network = s->snap.networkName;
        }
    };
    if (raster_) {
        for (const SlotPower& p : measured_) {
            TvChannelRow r;
            r.slot = p.pos.index; r.offsetHz = p.pos.offsetHz; r.levelDb = p.levelDb; r.aboveDb = p.aboveDb; r.occupied = p.occupied;
            r.bwHz = o_.widthMhz * 1e6;
            fill(r);
            r.selected = r.running && r.slot == sel;   // as before: a slot is shown selected only with a receiver
            v.push_back(r);
        }
        return v;
    }
    for (const TrackedSignal& t : trk_.tracks()) {
        if (!t.present && !find(t.id)) continue;
        TvChannelRow r;
        r.slot = t.id; r.offsetHz = t.cand.centerHz; r.levelDb = t.cand.plateauDb; r.aboveDb = t.cand.snrDb; r.occupied = t.misses == 0;
        r.bwHz = t.cand.bwHz; r.rsHz = t.cand.rsEstimate; r.rollOff = t.cand.rollOff; r.clipped = t.cand.clipped;
        fill(r);
        v.push_back(r);
    }
    return v;
}

std::vector<MultiService> SpanTv::services(bool markDuplicates) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MultiService> v;
    std::vector<Slot*> order;
    for (auto& s : live_) order.push_back(s.get());
    std::sort(order.begin(), order.end(), [](Slot* a, Slot* b) { return a->pos.offsetHz < b->pos.offsetHz; });
    for (Slot* s : order) {
        const bool locked = s->lockedNow();
        const double rs = s->haveTel && s->tel.dvbs.symbolRate > 0 ? s->tel.dvbs.symbolRate : s->cand.rsEstimate;
        std::lock_guard<std::mutex> tk(s->tsMu);
        for (const TsService& sv : s->snap.services) {
            MultiService m{s->pos.index, s->pos.offsetHz, sv};
            m.stdMode = o_.stdMode; m.bwHz = s->cand.bwHz; m.rsHz = raster_ ? 0 : rs;
            m.snrDb = s->haveTel ? s->tel.dataSnrDb : s->cand.snrDb; m.locked = locked; m.onid = s->snap.onid; m.tsid = s->snap.tsid;
            v.push_back(m);
        }
    }
    if (markDuplicates) {   // the same (network, stream, service): the signal with the better SNR is the source, the others point at it
        for (size_t i = 0; i < v.size(); i++)
            for (size_t j = 0; j < v.size(); j++) {
                if (i == j || v[i].slot == v[j].slot) continue;
                const auto ki = std::make_tuple(v[i].onid, v[i].tsid, v[i].svc.id), kj = std::make_tuple(v[j].onid, v[j].tsid, v[j].svc.id);
                if (ki != kj) continue;
                if (v[j].snrDb > v[i].snrDb || (v[j].snrDb == v[i].snrDb && v[j].slot < v[i].slot)) { v[i].duplicateOf = v[j].slot; break; }
            }
    }
    return v;
}

TsSnapshot SpanTv::snapshot(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return TsSnapshot();
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->snap;
}

std::map<int, std::vector<EpgEvent>> SpanTv::epg(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return {};
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->demux.epg();
}

BbStats SpanTv::bbStats(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    return s ? s->rx->bbStats() : BbStats();
}

std::vector<int> SpanTv::runningSlots() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto& s : live_) v.push_back(s->pos.index);
    return v;
}

std::vector<SlotPower> SpanTv::lastMeasure() { std::lock_guard<std::mutex> lk(mu_); return measured_; }
std::vector<SignalCand> SpanTv::lastSignals() { std::lock_guard<std::mutex> lk(mu_); return lastCands_; }

} // namespace dect2
