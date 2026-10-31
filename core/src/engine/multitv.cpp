#include "dect2/common/thread_name.h"
#include "dect2/engine/multitv.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>

namespace dect2 {

namespace {
constexpr int kNone = INT_MIN / 2;
constexpr double kQueueSec = 0.2;         // signal time that may wait for a receiver (a count of blocks was 9 ms at 60 Msps: any hiccup of a receiver lost samples)
constexpr size_t kQueueMin = 1u << 19;    // samples, at least
constexpr int kAbsentEvals = 8;           // evaluations (a quarter second each) without a signal before an idle receiver is dropped
inline uint64_t nowNs() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

using Block = std::shared_ptr<const std::vector<cf32>>;

struct MultiTv::Slot {
    ChannelSlotPos pos;
    MultiTv* owner = nullptr;
    ChannelFilter filter;
    std::unique_ptr<T2Receiver> t2;
    std::unique_ptr<DvbtReceiver> t;
    std::unique_ptr<DvbsReceiver> sat;
    bool dvbt = false;
    Transponder tp;
    DvbsTelemetry dtel;
    // transport stream
    std::mutex tsMu;
    BbUnpacker unpack;
    TsDemux demux;
    TsSnapshot snap;
    int lastT2Frame = -1;
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
    bool haveTel = false;
    uint64_t cpuBusy0 = 0, cpuSig0 = 0;
    float cpuPct = 0;
    int absent = 0;
    bool occupied = true;

    ~Slot() { t2.reset(); t.reset(); sat.reset(); }   // the receivers' threads call back into this slot: they go first
    void deliver(const uint8_t* pk, size_t n, double secs) {
        TsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(tsMu);
            for (size_t i = 0; i < n; i++) demux.feed(pk + i * 188);
            demux.advance(secs);
            snap = demux.snapshot();
            s = snap;
        }
        owner->mtvDeliver(pos.index, pk, n, secs, s);
    }
    void onPlp(const PlpResult& r) {
        std::unique_lock<std::mutex> lk(tsMu);
        frameBuf.clear();
        if (lastT2Frame >= 0) {
            const int mod = r.numT2Frames > 0 ? r.numT2Frames : 256;
            if (r.t2Frame != (lastT2Frame + std::max(1, r.frameInterval)) % mod) { unpack.lost(); demux.markLoss(); }
        }
        lastT2Frame = r.t2Frame;
        for (const auto& f : r.frames) {
            if (f.bits.empty()) { unpack.lost(); demux.markLoss(); continue; }
            unpack.push(f.bits, f.header);
        }
        demux.advance(r.frameSec);
        snap = demux.snapshot();
        TsSnapshot s = snap;
        std::vector<uint8_t> out;
        out.swap(frameBuf);
        lk.unlock();
        owner->mtvDeliver(pos.index, out.data(), out.size() / 188, r.frameSec, s);
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
            if (!o.empty()) { if (sat) sat->feed(o.data(), o.size()); else if (dvbt) t->feed(o.data(), o.size()); else t2->feed(o.data(), o.size()); }
            busyNs += nowNs() - t0;
            { std::lock_guard<std::mutex> lk(qMu); busy = false; }
            qCv.notify_all();
        }
    }
};

MultiTv::MultiTv() = default;
MultiTv::~MultiTv() { reset(); }

void MultiTv::setSink(Sink s) { std::lock_guard<std::mutex> lk(sinkMu_); sink_ = std::move(s); }

void MultiTv::mtvDeliver(int slot, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap) {
    if (slot != sel_.load() || !n) return;
    Sink f;
    { std::lock_guard<std::mutex> lk(sinkMu_); f = sink_; }
    if (f) f(slot, pk, n, secs, snap);
}

bool MultiTv::configure(const MultiTvOptions& o) {
    reset();
    o_ = o;
    gridOff_ = o.gridOffsetHz; gridFixed_ = !o.autoGrid;
    slots_ = o.sat ? std::vector<ChannelSlotPos>() : spanSlots(o.spanRate, o.widthMhz * 1e6, gridOff_);
    scan_.configure(o.spanRate);
    if (o.sat) return o.spanRate >= 3e6;
    queueLimit_ = std::max(kQueueMin, (size_t)(o.spanRate * kQueueSec));
    sel_ = kNone;
    return !slots_.empty() || o.autoGrid;
}

void MultiTv::stopSlot(size_t i) {
    Slot& s = *live_[i];
    { std::lock_guard<std::mutex> lk(s.qMu); s.stop = true; }
    s.qCv.notify_all();
    if (s.th.joinable()) s.th.join();
    live_.erase(live_.begin() + (long)i);
}

void MultiTv::reset() {
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
    { std::lock_guard<std::mutex> lk(mu_); tracks_.clear(); }
    nextId_ = 1;
    sinceEval_ = 0;
    sel_ = kNone;
    lastSelPolled_ = kNone;
}

MultiTv::Slot* MultiTv::find(int slot) {
    for (auto& s : live_) if (s->pos.index == slot) return s.get();
    return nullptr;
}

void MultiTv::startSlot(const SlotPower& p) {
    auto s = std::make_unique<Slot>();
    s->pos = p.pos; s->owner = this; s->dvbt = o_.dvbt;
    const double outRate = tvChannelRate(o_.widthMhz, o_.spanRate);
    if (!s->filter.configure(o_.spanRate, p.pos.offsetHz, outRate)) return;
    Slot* raw = s.get();
    s->unpack.setSink([raw](const uint8_t* pkt) { raw->demux.feed(pkt); raw->frameBuf.insert(raw->frameBuf.end(), pkt, pkt + 188); });   // under tsMu (onPlp)
    if (o_.dvbt) {
        s->t = std::make_unique<DvbtReceiver>();
        s->t->configure(outRate, o_.widthMhz);
        s->t->setPacketCallback([raw](const uint8_t* pk, size_t n, double secs) { raw->deliver(pk, n, secs); });
    } else {
        s->t2 = std::make_unique<T2Receiver>();
        s->t2->configure(outRate, o_.widthMhz);
        s->t2->setPlpCallback([raw](const PlpResult& r) { raw->onPlp(r); });
    }
    s->th = dect2::namedThread("onair-mtv", [raw] { raw->run(); });
    std::lock_guard<std::mutex> lk(mu_);
    live_.push_back(std::move(s));
    if (sel_.load() == kNone) sel_ = p.pos.index;
}

void MultiTv::evaluate() {
    if (scan_.frames() < 12) return;
    if (o_.sat) { evaluateSat(); return; }
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
    // drop receivers whose channel has gone and that never locked, or that are no longer allowed
    for (;;) {
        size_t victim = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (restrict_) for (size_t i = 0; i < live_.size(); i++) if (std::find(allowed_.begin(), allowed_.end(), live_[i]->pos.index) == allowed_.end()) { victim = i; break; }
            if (victim == SIZE_MAX)
            for (size_t i = 0; i < live_.size(); i++)
                if (live_[i]->absent >= kAbsentEvals && !(live_[i]->haveTel && live_[i]->tel.state >= 2)) { victim = i; break; }
        }
        if (victim == SIZE_MAX) break;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int idx = live_[victim]->pos.index;
            stopSlot(victim);
            if (sel_.load() == idx) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
        }
    }
    // new receivers for the occupied slots, the strongest first, up to the cap
    std::vector<SlotPower> want;
    for (const auto& p : m) if (p.occupied) want.push_back(p);
    std::sort(want.begin(), want.end(), [](const SlotPower& a, const SlotPower& b) { return a.aboveDb > b.aboveDb; });
    for (const auto& p : want) {
        size_t cnt;
        bool have;
        bool allowed = true;
        { std::lock_guard<std::mutex> lk(mu_); cnt = live_.size(); have = find(p.pos.index) != nullptr; if (restrict_) allowed = std::find(allowed_.begin(), allowed_.end(), p.pos.index) != allowed_.end(); }
        if (have || !allowed) continue;
        if ((int)cnt >= std::max(1, o_.maxChannels)) break;
        startSlot(p);
    }
}

void MultiTv::feed(const cf32* x, size_t n) {
    if (!n || o_.spanRate <= 0) return;
    scan_.feed(x, n);
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

void MultiTv::wait() {
    std::vector<Slot*> v;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& s : live_) v.push_back(s.get()); }
    for (Slot* s : v) {
        std::unique_lock<std::mutex> lk(s->qMu);
        s->qCv.wait(lk, [&] { return (s->q.empty() && !s->busy) || s->stop; });
    }
}

void MultiTv::setAllowedSlots(const std::vector<int>& slots) { std::lock_guard<std::mutex> lk(mu_); restrict_ = true; allowed_ = slots; }
void MultiTv::clearAllowedSlots() { std::lock_guard<std::mutex> lk(mu_); restrict_ = false; allowed_.clear(); }

void MultiTv::setSelected(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    if (find(slot)) sel_ = slot;
}

bool MultiTv::poll(RxTelemetry& out) {
    std::lock_guard<std::mutex> lk(mu_);
    bool selNew = false;
    const int sel = sel_.load();
    for (auto& sp : live_) {
        Slot& s = *sp;
        RxTelemetry t;
        bool got = false;
        if (s.sat) {
            DvbsTelemetry dt;
            if (s.sat->telemetry(dt, s.telSeq)) {
                got = true;
                t.standard = 7; t.seq = dt.seq; t.state = dt.state; t.cfoHz = dt.cfoHz; t.dataValid = dt.dataValid; t.dataSnrDb = dt.snrDb;
                t.blocksOk = dt.blocksOk; t.blocksBad = dt.blocksBad; t.rateOk = true;
                s.dtel = dt;
                t.dvbs = std::move(dt);
            }
        } else got = s.dvbt ? s.t->telemetry(t, s.telSeq) : s.t2->telemetry(t, s.telSeq);
        if (got) {
            s.telSeq = t.seq; s.tel = std::move(t); s.haveTel = true;
            if (s.pos.index == sel) selNew = true;
        }
        const uint64_t sig = s.sigSamples.load();
        if (sig - s.cpuSig0 >= (uint64_t)o_.spanRate) {   // about a second of signal
            const uint64_t busy = s.busyNs.load();
            const double secs = (double)(sig - s.cpuSig0) / o_.spanRate;
            s.cpuPct = (float)(100.0 * 1e-9 * (double)(busy - s.cpuBusy0) / std::max(secs, 1e-3));
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

std::vector<TvChannelRow> MultiTv::channels() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TvChannelRow> v;
    const int sel = sel_.load();
    if (o_.sat) {
        for (const TpTrack& tr : tracks_) {
            if (tr.seen < 2 && !find(tr.id)) continue;
            TvChannelRow r;
            r.slot = tr.id; r.sat = true; r.offsetHz = tr.tp.centerHz; r.levelDb = tr.tp.levelDb; r.aboveDb = tr.tp.snrDb; r.occupied = tr.absent == 0;
            r.bwHz = tr.tp.bwHz; r.symbolRate = tr.tp.rsHz;
            if (Slot* s = find(tr.id)) {
                r.running = true; r.selected = s->pos.index == sel;
                r.cpuPct = s->cpuPct; r.dropped = s->dropped.load();
                if (s->haveTel) {
                    const DvbsTelemetry& d = s->dtel;
                    r.state = d.state; r.snrDb = d.merDb > 0 ? d.merDb : d.snrDb;
                    r.locked = d.tsLock || d.state >= 2;
                    if (d.symbolRate > 0) r.symbolRate = d.symbolRate;
                    if (d.standard > 0) {
                        r.detail = d.standard == 1 ? "DVB-S" : d.standard == 2 ? "DVB-S2" : "DVB-S2X";
                        if (!d.modulationName.empty()) r.detail += " " + d.modulationName;
                        if (!d.codeRate.empty()) r.detail += " " + d.codeRate;
                    }
                }
                std::lock_guard<std::mutex> tk(s->tsMu);
                r.services = (int)s->snap.services.size();
                r.network = s->snap.networkName;
            }
            v.push_back(r);
        }
        std::sort(v.begin(), v.end(), [](const TvChannelRow& a, const TvChannelRow& b) { return a.offsetHz < b.offsetHz; });
        return v;
    }
    for (const SlotPower& p : measured_) {
        TvChannelRow r;
        r.slot = p.pos.index; r.offsetHz = p.pos.offsetHz; r.levelDb = p.levelDb; r.aboveDb = p.aboveDb; r.occupied = p.occupied;
        if (Slot* s = find(p.pos.index)) {
            r.running = true; r.selected = s->pos.index == sel;
            r.cpuPct = s->cpuPct; r.dropped = s->dropped.load();
            if (s->haveTel) {
                r.state = s->tel.state; r.snrDb = s->tel.dataSnrDb;
                r.locked = s->tel.state >= 2 || s->tel.dvbt.fecSync || s->tel.dataValid;
            }
            std::lock_guard<std::mutex> tk(s->tsMu);
            r.services = (int)s->snap.services.size();
            r.network = s->snap.networkName;
        }
        v.push_back(r);
    }
    return v;
}

std::vector<MultiService> MultiTv::services() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MultiService> v;
    std::vector<Slot*> order;
    for (auto& s : live_) order.push_back(s.get());
    std::sort(order.begin(), order.end(), [](Slot* a, Slot* b) { return a->pos.offsetHz < b->pos.offsetHz; });
    for (Slot* s : order) {
        std::lock_guard<std::mutex> tk(s->tsMu);
        for (const TsService& sv : s->snap.services) v.push_back(MultiService{s->pos.index, s->pos.offsetHz, sv});
    }
    return v;
}

TsSnapshot MultiTv::snapshot(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return TsSnapshot();
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->snap;
}

std::map<int, std::vector<EpgEvent>> MultiTv::epg(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return {};
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->demux.epg();
}

BbStats MultiTv::bbStats(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return BbStats();
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->unpack.stats();
}

std::vector<int> MultiTv::runningSlots() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto& s : live_) v.push_back(s->pos.index);
    return v;
}

std::vector<Transponder> MultiTv::lastTransponders() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Transponder> v;
    for (const TpTrack& t : tracks_) if (t.absent == 0) v.push_back(t.tp);
    return v;
}

// DVB-S/S2: look at the averaged spectrum, follow the transponders found (same one when the centres are within a quarter of its width), start
// a receiver for each that has been there two looks in a row, strongest first, up to the cap; drop a track that has been gone for a while
void MultiTv::evaluateSat() {
    TransponderParams tp;
    tp.minRsHz = o_.minRsHz;
    tp.thresholdDb = std::max(3.0, o_.thresholdDb - 1);
    const std::vector<Transponder> found = detectTransponders(scan_.spectrumDb(), o_.spanRate, tp);
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (TpTrack& t : tracks_) t.absent++;
        for (const Transponder& f : found) {
            if (f.clipped) continue;   // runs into the edge of the capture: its other half is not here
            TpTrack* best = nullptr;
            double bd = 1e30;
            for (TpTrack& t : tracks_) {
                const double d = std::fabs(t.tp.centerHz - f.centerHz), tol = std::max(0.25 * std::min(t.tp.bwHz, f.bwHz), 150e3);
                if (d < tol && d < bd) { best = &t; bd = d; }
            }
            if (best && best->absent == 0) best = nullptr;   // already matched in this look: this one is another transponder
            if (best) {
                const bool running = find(best->id) != nullptr;
                if (best->absent > 0) best->seen++;   // a track is matched once per look; a second hit in the same look is another transponder
                best->absent = 0;
                if (!running) best->tp = f;           // a running receiver keeps the centre it was started at; the table shows the measurement
                else { best->tp.levelDb = f.levelDb; best->tp.snrDb = f.snrDb; }
            } else {
                TpTrack t; t.id = nextId_++; t.tp = f; t.seen = 1; t.absent = 0;
                tracks_.push_back(t);
            }
        }
        // a track that was not seen for 4 looks starts counting again; the long absent ones go (never a running one with a lock)
        for (TpTrack& t : tracks_) if (t.absent >= 2 && !find(t.id)) t.seen = 0;
    }
    // stop receivers whose transponder has gone and that never locked; drop dead tracks
    for (;;) {
        size_t victim = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < live_.size(); i++) {
                int ab = 0;
                for (const TpTrack& t : tracks_) if (t.id == live_[i]->pos.index) ab = t.absent;
                if (ab >= kAbsentEvals && !(live_[i]->haveTel && live_[i]->dtel.state >= 2)) { victim = i; break; }
            }
        }
        if (victim == SIZE_MAX) break;
        std::lock_guard<std::mutex> lk(mu_);
        const int idx = live_[victim]->pos.index;
        stopSlot(victim);
        if (sel_.load() == idx) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < tracks_.size();) {
            if (tracks_[i].absent >= kAbsentEvals && !find(tracks_[i].id)) tracks_.erase(tracks_.begin() + (long)i);
            else i++;
        }
    }
    std::vector<TpTrack> want;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const TpTrack& t : tracks_) if (t.absent == 0 && t.seen >= 2 && !find(t.id)) want.push_back(t);
    }
    std::sort(want.begin(), want.end(), [](const TpTrack& a, const TpTrack& b) { return a.tp.snrDb > b.tp.snrDb; });
    for (const TpTrack& t : want) {
        size_t cnt;
        { std::lock_guard<std::mutex> lk(mu_); cnt = live_.size(); }
        if ((int)cnt >= std::max(1, o_.maxChannels)) break;
        startSat(t);
    }
}

void MultiTv::startSat(const TpTrack& t) {
    auto s = std::make_unique<Slot>();
    s->pos.index = t.id; s->pos.offsetHz = t.tp.centerHz; s->owner = this; s->tp = t.tp;
    // as the secondary DVB-S receiver of the engine: 3.2 samples per symbol, 2 Msps at least, never more than the capture has
    const double outRate = std::min(o_.spanRate, std::max(2e6, t.tp.rsHz * 3.2));
    if (!s->filter.configure(o_.spanRate, t.tp.centerHz, outRate)) return;
    Slot* raw = s.get();
    s->sat = std::make_unique<DvbsReceiver>();
    s->sat->configure(outRate);
    s->sat->setBlocking(o_.blocking);
    s->sat->setPacketCallback([raw](const uint8_t* pk, size_t n, double secs) { raw->deliver(pk, n, secs); });
    s->th = dect2::namedThread("onair-mtv", [raw] { raw->run(); });
    std::lock_guard<std::mutex> lk(mu_);
    live_.push_back(std::move(s));
    if (sel_.load() == kNone) sel_ = t.id;
}

std::vector<SlotPower> MultiTv::lastMeasure() { std::lock_guard<std::mutex> lk(mu_); return measured_; }

} // namespace dect2
