#include "dect2/fastscan/scanner.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/common/threadpool.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <thread>

namespace dect2 {
namespace fastscan {

namespace {
using Clock = std::chrono::steady_clock;
double wall() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
constexpr int kChunks = 4;
}  // namespace

FastScanner::FastScanner() {}
FastScanner::~FastScanner() {}

bool FastScanner::configure(const FastScanConfig& c, const RadioProfile& r, std::string& err) {
    std::lock_guard<std::mutex> g(m_);
    cfg_ = c; prof_ = r;
    plan_ = planFast(c.plan, r);
    if (plan_.hops.empty() || plan_.totalBins == 0) { err = "nothing to scan: the bands are empty or excluded"; return false; }
    const int N = plan_.fftSize;
    log2n_ = 0;
    while ((1 << log2n_) < N) log2n_++;
    win_.resize(N);
    double ws = 0;
    for (int i = 0; i < N; i++) { win_[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); ws += win_[i]; }
    winSum_ = (float)ws;
    acc_.assign((size_t)kChunks * N, 0.f);
    scratchRe_.assign((size_t)kChunks * N, 0.f); scratchIm_.assign((size_t)kChunks * N, 0.f);
    pw_.assign(N, 0.f); hopDb_.assign(plan_.binsPerHop, 0.f); hopCal_.assign(plan_.binsPerHop, 0.f);
    floor_.configure(plan_.totalBins, c.floor);
    floorLoaded_ = !c.floorFile.empty() && floor_.load(c.floorFile, plan_.startHz, plan_.binHz);
    cal_.configure(plan_.binsPerHop);
    HitParams hp = c.hits;
    tracker_.configure(hp);
    qual_.configure(c.qualify); qual_.clear(); sweep_ = 0;
    view_ = SpectrumView();
    view_.startHz = plan_.startHz; view_.binHz = plan_.binHz;
    view_.cur.assign(plan_.totalBins, -150.f); view_.avg = view_.cur; view_.maxhold = view_.cur; view_.floor = view_.cur;
    view_.mask = plan_.mask;
    lastT_.assign(plan_.hops.size(), 0.0);
    st_ = ScanStats();
    st_.settleSec = plan_.settleSec;
    return true;
}

bool FastScanner::saveFloor() const {
    std::lock_guard<std::mutex> g(m_);
    return !cfg_.floorFile.empty() && floor_.save(cfg_.floorFile, plan_.startHz, plan_.binHz);
}

void FastScanner::resetTraces() {
    std::lock_guard<std::mutex> g(m_);
    std::fill(view_.avg.begin(), view_.avg.end(), -150.f);
    std::fill(view_.maxhold.begin(), view_.maxhold.end(), -150.f);
    view_.seq++;
}

std::vector<Hit> FastScanner::hitsSnapshot() const { std::lock_guard<std::mutex> g(m_); return tracker_.hits(); }
void FastScanner::setFilters(std::vector<FreqRange> ex, std::vector<double> lk, std::vector<double> pr) {
    std::lock_guard<std::mutex> g(m_);
    tracker_.setExcluded(std::move(ex)); tracker_.setLocked(std::move(lk)); tracker_.setPriority(std::move(pr));
}
std::vector<HitLabel> FastScanner::labels() const { std::lock_guard<std::mutex> g(m_); return labels_; }
void FastScanner::removeHit(uint64_t id) { std::lock_guard<std::mutex> g(m_); tracker_.remove(id); }

std::vector<ChannelInfo> FastScanner::channels(bool reliableOnly) const { std::lock_guard<std::mutex> g(m_); return qual_.channels(tracker_.hits(), reliableOnly); }
SpectrumView FastScanner::snapshot() const { std::lock_guard<std::mutex> g(m_); return view_; }
ScanStats FastScanner::stats() const { std::lock_guard<std::mutex> g(m_); return st_; }

// Averaged power of one block into pw_ (DC-centred, a full-scale complex tone reads 1.0 on its peak bin).
void FastScanner::hopPsd(const cf32* x, size_t n) {
    const int N = plan_.fftSize, L = log2n_;
    const size_t step = std::max<size_t>(1, (size_t)N / 2);
    const int K = n >= (size_t)N ? (int)((n - N) / step) + 1 : 0;
    std::fill(acc_.begin(), acc_.end(), 0.f);
    auto job = [&](size_t ci) {
        float* re = &scratchRe_[ci * N];
        float* im = &scratchIm_[ci * N];
        float* a = &acc_[ci * N];
        for (int k = (int)ci; k < K; k += kChunks) {
            const cf32* b = x + (size_t)k * step;
            for (int i = 0; i < N; i++) { re[i] = b[i].real() * win_[i]; im[i] = b[i].imag() * win_[i]; }
            fftSplit(re, im, L, false);
            for (int i = 0; i < N; i++) a[i] += re[i] * re[i] + im[i] * im[i];
        }
    };
    if (cfg_.threads == 1 || (size_t)K * N < ((size_t)1 << 18)) for (int ci = 0; ci < kChunks; ci++) job(ci);
    else ThreadPool::shared().parallelFor((size_t)kChunks, job);
    const float norm = K > 0 ? 1.0f / ((float)K * winSum_ * winSum_) : 0.f;
    for (int i = 0; i < N; i++) {
        float s = 0;
        for (int ci = 0; ci < kChunks; ci++) s += acc_[(size_t)ci * N + i];
        pw_[(i + N / 2) % N] = s * norm;   // DC-centred: index N/2 is 0 Hz
    }
}

void FastScanner::processHop(int hop, const cf32* x, size_t n, double t, std::vector<HitEvent>& ev) {
    std::lock_guard<std::mutex> g(m_);
    if (hop < 0 || hop >= (int)plan_.hops.size()) return;
    const auto t0 = Clock::now();
    const Hop& h = plan_.hops[hop];
    const int N = plan_.fftSize;
    const size_t U = plan_.binsPerHop;
    // the DC spike of zero-IF radios is a property of the block: the mean comes out before the transform
    std::vector<cf32>* dummy = nullptr; (void)dummy;
    hopPsd(x, n);
    for (size_t j = 0; j < U; j++) hopDb_[j] = 10.f * std::log10(std::max(pw_[N / 2 - U / 2 + j], 1e-14f));
    if (prof_.dcFrac > 0 && U > 12) {   // bins j = U/2 is 0 Hz: the DC region is interpolated from the bins just outside it
        const int d = (int)(U / 2), w = std::max(2, (int)std::ceil(prof_.dcFrac * plan_.fftSize * 0.5));
        const float a = hopDb_[d - w - 1], b = hopDb_[d + w + 1];
        for (int k = -w; k <= w; k++) hopDb_[d + k] = a + (b - a) * (float)(k + w + 1) / (float)(2 * w + 2);
    }
    cal_.apply(hopDb_.data(), hopCal_.data());
    const size_t nb = h.nBins;
    // the hop's median level seeds floors that have not been seen yet
    cand_.assign(hopCal_.begin(), hopCal_.begin() + (long)nb);
    std::nth_element(cand_.begin(), cand_.begin() + (long)(nb / 2), cand_.end());
    const float med = cand_[nb / 2];
    floor_.update(h.firstBin, hopCal_.data(), nb, t, med - 1.0f);
    // candidates against the floor, masks of excluded ranges ignored
    const std::vector<Candidate> cands = findCandidates(hopCal_.data(), &floor_.floors()[h.firstBin], nb, plan_.startHz + h.firstBin * plan_.binHz, plan_.binHz,
                                                        tracker_.detectDb(), std::min(tracker_.detectDb(), 4.f), 1, &plan_.mask[h.firstBin]);
    tracker_.observe(t, h.loHz - plan_.binHz * 0.5, h.hiHz + plan_.binHz * 0.5, cands, ev);
    qual_.look(plan_, hop, sweep_, t, tracker_.hits());
    // the pseudo-spectrum
    const double dt = lastT_[hop] > 0 ? std::max(0.0, t - lastT_[hop]) : 0.0;
    lastT_[hop] = t;
    const float fall = (float)(cfg_.maxholdFallDbPerSec * dt);
    const float al = (float)cfg_.avgAlpha;
    for (size_t j = 0; j < nb; j++) {
        const size_t b = h.firstBin + j;
        const float v = hopCal_[j];
        view_.cur[b] = v;
        view_.avg[b] = view_.avg[b] < -140.f ? v : view_.avg[b] + al * (v - view_.avg[b]);
        view_.maxhold[b] = cfg_.maxholdFallDbPerSec > 0 ? std::max(v, view_.maxhold[b] - fall) : std::max(v, view_.maxhold[b]);
        view_.floor[b] = floor_.floorDb(b);
    }
    view_.seq++;
    st_.hops++;
    st_.activeHits = tracker_.activeCount();
    st_.dspSecPerHop = st_.dspSecPerHop == 0 ? since(t0) : 0.9 * st_.dspSecPerHop + 0.1 * since(t0);
}

// ---------------------------------------------------------------- the scan loop
namespace {
struct Job { int hop = -1; std::vector<cf32> buf; double t = 0; };
}

bool FastScanner::run(sweep::SweepSource& src, const std::atomic<bool>* cancel, RunHooks& hk, std::string& err) {
    if (plan_.hops.empty()) { err = "not configured"; return false; }
    if (std::fabs(src.sampleRate() - plan_.rateHz) > 1.0) { err = "the radio runs at a different sample rate than the plan"; return false; }
    const auto clock = hk.clock ? hk.clock : std::function<double()>(wall);
    constexpr int kBufs = 3;
    std::vector<Job> pool(kBufs);
    for (auto& j : pool) j.buf.resize(plan_.samplesPerHop);
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<int> freeQ, workQ;
    for (int i = 0; i < kBufs; i++) freeQ.push_back(i);
    bool done = false, failed = false;
    std::string werr;
    park_ = -1;
    int idHop = -1; double idHz = 0, idBw = 0, idSince = 0;
    std::vector<cf32> idBuf;
    double parkSince = 0;
    int parkedCenterHop = -1;

    std::thread worker([&] {
        std::vector<HitEvent> ev;
        while (true) {
            int id;
            {
                std::unique_lock<std::mutex> l(qm);
                qcv.wait(l, [&] { return !workQ.empty() || done; });
                if (workQ.empty()) return;
                id = workQ.front(); workQ.pop_front();
            }
            Job& j = pool[id];
            ev.clear();
            processHop(j.hop, j.buf.data(), j.buf.size(), j.t, ev);
            // naming a new signal: the scan stays on its hop for a moment, the capture goes to the identifier
            bool identifying = false;
            if (cfg_.identifyNew) {
                if (idHop < 0 && park_.load() < 0) {
                    for (const HitEvent& e : ev) {
                        if (e.kind != HitEvent::Start) continue;
                        bool known = false;
                        { std::lock_guard<std::mutex> g(m_); for (const auto& q : labels_) if (std::fabs(q.freqHz - e.hit.freqHz) < std::max(5000.0, 0.5 * e.hit.bwHz)) known = true; }
                        if (known) continue;
                        idHop = j.hop; idHz = e.hit.freqHz; idBw = e.hit.bwHz; idSince = j.t; idBuf.clear();
                        park_ = j.hop;
                        break;
                    }
                }
                if (idHop == j.hop) {
                    idBuf.insert(idBuf.end(), j.buf.begin(), j.buf.end());
                    const size_t want = (size_t)(std::min(sigid::captureSeconds(plan_.rateHz), 0.6) * plan_.rateHz);
                    if (idBuf.size() >= want) {
                        const Hop& hp = plan_.hops[j.hop];
                        sigid::Options o; o.rfHz = hp.centerHz;
                        const double off = idHz - hp.centerHz, half = std::max(6000.0, 0.7 * idBw);
                        o.loHz = off - half; o.hiHz = off + half;
                        const sigid::Result r = sigid::identify(idBuf.data(), idBuf.size(), plan_.rateHz, o);
                        HitLabel l; l.freqHz = idHz; l.verdict = r.verdict; l.headline = r.headline;
                        if (r.verdict != sigid::VerdictUnknown && !r.cands.empty()) { l.name = r.cands[0].name; l.modeId = r.cands[0].modeId; l.score = (float)r.cands[0].score; }
                        { std::lock_guard<std::mutex> g(m_); labels_.push_back(std::move(l)); if (labels_.size() > 2000) labels_.erase(labels_.begin()); }
                        idHop = -1; idBuf.clear(); idBuf.shrink_to_fit();
                        park_ = -1;
                    } else if (j.t - idSince > 3.0) { idHop = -1; idBuf.clear(); park_ = -1; }
                }
                identifying = idHop >= 0;
            }
            RecorderBank* rec = hk.rec;
            if (rec && !identifying) {
                const Hop& h = plan_.hops[j.hop];
                // the same capture feeds the recorders while the radio stays on this hop
                for (const HitEvent& e : ev) {
                    if (e.kind == HitEvent::Start) {
                        if (rec->stats().active == 0 && (!rec->ready() || std::fabs(rec->centerHz() - h.centerHz) > 1.0)) rec->retarget(h.centerHz);
                        if (std::fabs(rec->centerHz() - h.centerHz) < 1.0 &&
                            rec->start(e.hit.id, e.hit.freqHz, hk.modeOf ? hk.modeOf(e.hit.freqHz) : guessMode(e.hit.freqHz, e.hit.bwHz), (std::time_t)j.t)) {
                            if (park_.load() < 0) { park_ = j.hop; parkSince = j.t; }
                        }
                    } else rec->stop(e.hit.id);
                }
                if (rec->ready() && std::fabs(rec->centerHz() - h.centerHz) < 1.0) rec->feed(j.buf.data(), j.buf.size());
                if (rec->stats().active == 0) park_ = -1;
                else if (park_.load() >= 0 && j.t - parkSince > hk.maxParkSec) { rec->closeAll(); park_ = -1; }
            }
            if (hk.onEvents && !ev.empty()) hk.onEvents(ev);
            std::lock_guard<std::mutex> l(qm);
            freeQ.push_back(id);
            qcv.notify_all();
        }
    });

    bool ok = true;
    int tuned = -1;
    size_t oi = 0;
    uint64_t pass = 0;
    auto passStart = Clock::now();
    while (!(cancel && cancel->load())) {
        int hop = plan_.order[oi % plan_.order.size()];
        const int pk = park_.load();
        const bool parked = pk >= 0;
        if (parked) hop = pk;
        if (hop != tuned) {
            if (!src.retune(plan_.hops[hop].centerHz, err)) { ok = false; break; }
            if (plan_.settleSec > 0) src.discard(plan_.settleSec);
            tuned = hop;
            parkedCenterHop = hop;
        }
        int id;
        {
            std::unique_lock<std::mutex> l(qm);
            qcv.wait(l, [&] { return !freeQ.empty() || done; });
            id = freeQ.front(); freeQ.pop_front();
        }
        Job& j = pool[id];
        if (src.capture(j.buf.data(), j.buf.size()) < j.buf.size()) {
            err = "the source ended"; ok = false;
            std::lock_guard<std::mutex> l(qm);
            freeQ.push_back(id);
            break;
        }
        j.hop = hop; j.t = clock();
        {
            std::lock_guard<std::mutex> l(qm);
            workQ.push_back(id);
            qcv.notify_all();
        }
        {
            std::lock_guard<std::mutex> g(m_);
            st_.parked = parked;
        }
        if (parked) continue;   // the pass does not advance while recording
        if (++oi % plan_.order.size() == 0) {
            pass++;
            beginSweep();
            const double sec = since(passStart);
            passStart = Clock::now();
            {
                std::lock_guard<std::mutex> g(m_);
                st_.passes = pass; st_.passSec = sec;
                st_.mhzPerSec = sec > 0 ? plan_.coveredHz / 1e6 / sec : 0;
                st_.hopsPerSec = sec > 0 ? (double)plan_.order.size() / sec : 0;
            }
            if (hk.onPass) hk.onPass(stats());
            if (hk.maxPasses > 0 && (int)pass >= hk.maxPasses) break;
        }
    }
    (void)parkedCenterHop;
    {   // let the worker drain what is queued, then stop it
        std::unique_lock<std::mutex> l(qm);
        qcv.wait(l, [&] { return workQ.empty() && (int)freeQ.size() == kBufs; });
        done = true;
        qcv.notify_all();
    }
    worker.join();
    std::vector<HitEvent> ev;
    {
        std::lock_guard<std::mutex> g(m_);
        tracker_.endAll(clock(), ev);
    }
    if (hk.rec) { for (const HitEvent& e : ev) hk.rec->stop(e.hit.id); hk.rec->closeAll(); }
    if (hk.onEvents && !ev.empty()) hk.onEvents(ev);
    (void)failed; (void)werr;
    return ok;
}

bool FastScanner::runBlocks(BlockSource& src, const std::atomic<bool>* cancel, RunHooks& hk, std::string& err) {
    if (plan_.hops.empty()) { err = "not configured"; return false; }
    if (std::fabs(src.sampleRate() - plan_.rateHz) > 1.0) { err = "the radio runs at a different sample rate than the plan"; return false; }
    const auto clock = hk.clock ? hk.clock : std::function<double()>(wall);
    const double tol = std::max(plan_.binHz, (double)plan_.binsPerHop * plan_.binHz * 0.25);
    std::vector<cf32> buf;
    std::vector<HitEvent> ev;
    int last = -1;
    uint64_t pass = 0, blocks = 0;
    auto passStart = Clock::now();
    double c = 0;
    bool ok = true;
    while (!(cancel && cancel->load())) {
        if (!src.next(c, buf, cancel, err)) { ok = err.empty(); break; }
        int hop = -1;
        double best = tol;
        for (size_t i = 0; i < plan_.hops.size(); i++) {
            const double d = std::fabs(plan_.hops[i].centerHz - c);
            if (d < best) { best = d; hop = (int)i; }
        }
        if (hop < 0 || buf.size() < (size_t)plan_.fftSize) continue;
        ev.clear();
        if (last >= 0 && hop <= last) {   // the sweep wrapped: one pass done
            pass++;
            beginSweep();
            const double sec = since(passStart);
            passStart = Clock::now();
            { std::lock_guard<std::mutex> g(m_); st_.passes = pass; st_.passSec = sec; st_.mhzPerSec = sec > 0 ? plan_.coveredHz / 1e6 / sec : 0;
              st_.hopsPerSec = sec > 0 ? (double)blocks / sec : 0; }
            blocks = 0;
            if (hk.onPass) hk.onPass(stats());
            if (hk.maxPasses > 0 && (int)pass >= hk.maxPasses) break;
        }
        last = hop;
        blocks++;
        processHop(hop, buf.data(), buf.size(), clock(), ev);
        if (hk.onEvents && !ev.empty()) hk.onEvents(ev);
    }
    ev.clear();
    { std::lock_guard<std::mutex> g(m_); tracker_.endAll(clock(), ev); }
    if (hk.onEvents && !ev.empty()) hk.onEvents(ev);
    return ok;
}

}  // namespace fastscan
}  // namespace dect2
