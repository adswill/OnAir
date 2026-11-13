// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells receiver (nr_rx.h): blocks of input, the sync raster points inside them searched and the PBCH of every SSB decoded.
#include "dect2/common/thread_name.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/nr/nr_phy.h"
#include "dect2/nr/nr_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

ModeTuning nrTuning() {
    ModeTuning t;
    t.stdMode = 59; t.id = "nr"; t.name = "5G NR cells";
    t.minMhz = 600; t.maxMhz = 4200; t.defMhz = 3541.44;   // n78 GSCN 7875
    t.sampleRate = 20000000;     // several sync raster points per step; the HackRF Pro tunes exactly at 20 Msps
    t.basebandHz = 15000000;
    t.bandwidthMhz = 20;
    t.minSampleRate = 3840000;   // one 15 kHz SSB (3.6 MHz); a 30 kHz SSB (7.2 MHz) needs 7.68 Msps
    return t;
}

namespace {

constexpr double kBlockSec = 0.080;

int64_t wallNow() { return dect2::unixNow(); }

struct Cand { double off; int scs; int gscn; double maxCfo; };

} // namespace

struct NrReceiver::Impl {
    mutable std::mutex mu;
    double rateSet = 0, sigOff = 0, centerHz = 0;
    std::function<void(const std::string&)> log;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> idle{true};
    std::atomic<bool> clearReq{false};
    // capture (analysis thread)
    std::vector<cf32> acc;
    size_t want = 0;
    uint64_t accGen = 0;
    double accCenter = 0, accOff = 0;
    uint64_t total = 0;
    double levelAcc = 0; uint64_t levelN = 0;
    double lastPubSec = -1;
    // worker
    std::thread th;
    std::condition_variable cv;
    bool stop = false, jobReady = false;
    std::vector<cf32> job;
    double jobCenter = 0, jobOff = 0, jobRate = 0, jobSec = 0;
    uint64_t jobGen = 0;
    // results (under mu)
    NrTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-nr", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        NrEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kNrLogMax) tel.events.resize(kNrLogMax);
        if (log) log("nr: " + s);
    }

    void publish() { tel.seq++; pub = tel; }

    void run() {
        for (;;) {
            std::vector<cf32> x;
            double center, off, rate, sec; uint64_t g;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || jobReady; });
                if (stop) return;
                x.swap(job); jobReady = false;
                center = jobCenter; off = jobOff; rate = jobRate; sec = jobSec; g = jobGen;
            }
            process(x, center, off, rate, sec, g);
            idle = true;
        }
    }

    static double maxCfoAt(double hz, int scs) {   // the radio's tuning error: about 10 ppm, at least half a subcarrier
        return std::max(scs * 1e3 / 2, std::min(45e3, hz * 10e-6));
    }

    std::vector<Cand> candidates(double center, double dialOff, double fs, int& lo, int& hi) {
        std::vector<Cand> out;
        lo = hi = -1;
        auto add = [&](double off, int scs, int g, double maxCfo) {
            if (fs < 256e3 * scs * 0.97) return;
            for (const auto& c : out) if (c.scs == scs && std::fabs(c.off - off) < 5e3) return;
            out.push_back({off, scs, g, maxCfo});
        };
        if (center > 0) {
            const double dial = center + dialOff;
            const int gd = nr::hzToGscn(dial);
            // the dial first, on the raster when it is close to it
            for (int scs : {30, 15}) {
                const int b = gd > 0 ? nr::nrBandOfGscn(gd, scs) : 0;
                if (b && std::fabs(nr::gscnToHz(gd) - dial) < 30e3) add(nr::gscnToHz(gd) - center, scs, gd, maxCfoAt(dial, scs));
            }
            for (const auto& b : nr::nrBands()) {
                const double half = 120 * b.scsKhz * 1e3 + 0.2e6;
                for (int g = b.gscnLo; g <= b.gscnHi; g += b.gscnStep) {
                    const double f = nr::gscnToHz(g), off = f - center;
                    if (std::fabs(off) + half > fs * 0.46) continue;
                    add(off, b.scsKhz, g, maxCfoAt(f, b.scsKhz));
                    lo = lo < 0 ? g : std::min(lo, g); hi = std::max(hi, g);
                }
            }
            if (out.empty()) for (int scs : {30, 15}) add(dialOff, scs, -1, maxCfoAt(dial, scs));   // off every band: the dial itself
        } else {
            for (int scs : {30, 15}) add(dialOff, scs, -1, 60e3);
        }
        return out;
    }

    void process(const std::vector<cf32>& x, double center, double dialOff, double fs, double sec, uint64_t g) {
        int lo = -1, hi = -1;
        const std::vector<Cand> cands = candidates(center, dialOff, fs, lo, hi);
        std::vector<std::pair<Cand, std::vector<nr::CellResult>>> found;
        for (const Cand& cd : cands) {
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            nr::SearchOpts o;
            o.scsKhz = cd.scs; o.maxCfoHz = cd.maxCfo;
            auto r = nr::searchDecode(x.data(), x.size(), fs, cd.off, o);
            if (!r.empty()) found.push_back({cd, std::move(r)});
        }
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.lastBlockCenterHz = center;
        tel.lastGscnLo = lo; tel.lastGscnHi = hi; tel.lastCandidates = (int)cands.size();
        for (auto& fr : found) for (auto& r : fr.second) merge(fr.first, r, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const NrCell& a, const NrCell& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kNrCellMax) tel.cells.resize(kNrCellMax);
        refreshState(sec);
        publish();
    }

    void refreshState(double sec) {
        const NrCell* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 10) recent = true;
            if (sec - c.lastSec < 10 && (!best || c.rsrpDb > best->rsrpDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->sinrDb; }
    }

    void merge(const Cand& cd, const nr::CellResult& r, double center, double sec) {
        if (!r.hasMib && r.sssSig < 6.0f) return;   // PSS / SSS alone must stand well clear of the other identities
        const double nominal = center > 0 ? center + cd.off : cd.off;
        NrCell* t = nullptr;
        for (auto& c : tel.cells) if (c.pci == r.pci && c.scsKhz == r.scsKhz && std::fabs(c.freqHz - nominal) < 30e3) { t = &c; break; }
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(NrCell());
            t = &tel.cells.back();
            t->freqHz = nominal; t->gscn = cd.gscn; t->pci = r.pci; t->nid1 = r.nid1; t->nid2 = r.nid2; t->scsKhz = r.scsKhz; t->firstWall = wallNow();
            t->band = cd.gscn > 0 ? nr::nrBandOfGscn(cd.gscn, r.scsKhz) : 0;
        }
        const bool firstMib = r.hasMib && !t->hasMib;
        if (r.hasMib) {
            const nr::Mib& m = r.mib;
            t->hasMib = true; t->sfn = m.sfn; t->hrf = m.hrf; t->scsCommonKhz = m.scs30 ? 30 : 15; t->kSsb = m.kSsb; t->dmrsTypeAPos = m.dmrsTypeAPos;
            t->coreset0 = m.coreset0; t->ss0 = m.ss0; t->barred = m.barred; t->intraFreqResel = m.intraFreqResel;
            // DMRS index 4..7: the decode told Lmax 4 (second half frame) from 8; below 4 both give the same bits: the band's usual one
            t->lmax = r.issbBar >= 4 ? m.lmax : (center > 0 ? nr::lmaxFor(t->band, nominal) : 8);
            t->ssbIdx = m.ssbIdx;
            t->sib1Scheduled = m.kSsb <= 23;
            int rb = 0, sy = 0, of = 0;
            if (t->sib1Scheduled && nr::coreset0Info(r.scsKhz, t->scsCommonKhz, m.coreset0, rb, sy, of)) { t->cs0Rbs = rb; t->cs0Syms = sy; t->cs0OffRb = of; }
            else t->cs0Rbs = t->cs0Syms = t->cs0OffRb = 0;
            t->mibs += (uint64_t)r.pbchOk;
            tel.blocksOk += (uint64_t)r.pbchOk;
        }
        tel.blocksBad += (uint64_t)std::max(0, r.pbchTried - r.pbchOk);
        for (const auto& b : r.beams) {
            bool hit = false;
            for (auto& q : t->beams) if (q.ssbIdx == b.ssbIdx) { q.rsrpDb = b.rsrpDb; hit = true; }
            if (!hit) t->beams.push_back({b.ssbIdx, b.rsrpDb});
        }
        std::sort(t->beams.begin(), t->beams.end(), [](const NrBeamSeen& a, const NrBeamSeen& b) { return a.ssbIdx < b.ssbIdx; });
        t->rsrpDb = r.rsrpDb; t->rsrqDb = r.rsrqDb; t->sinrDb = r.sinrDb; t->pssDb = r.pssDb;
        t->cfoHz = r.cfoHz;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        char b[256];
        const std::string where = center > 0 ? [&] { char w[96]; snprintf(w, sizeof w, "%.2f MHz (GSCN %d, n%d)", nominal / 1e6, t->gscn, t->band); return std::string(w); }()
                                             : [&] { char w[64]; snprintf(w, sizeof w, "%+.1f kHz", nominal / 1e3); return std::string(w); }();
        if (isNew && !r.hasMib) { snprintf(b, sizeof b, "PCI %d at %s, %d kHz SSB, synchronisation signals only", t->pci, where.c_str(), t->scsKhz); event(b, sec); }
        else if (isNew || firstMib) {
            snprintf(b, sizeof b, "PCI %d at %s, %d kHz SSB, SSB %d, SFN %d, %s%s", t->pci, where.c_str(), t->scsKhz, t->ssbIdx, t->sfn,
                     t->sib1Scheduled ? "SIB1 scheduled" : "no SIB1 (k_SSB > 23)", t->barred ? ", barred" : "");
            event(b, sec);
        }
    }
};

NrReceiver::NrReceiver() : p_(new Impl) {}
NrReceiver::~NrReceiver() = default;

void NrReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * kBlockSec);
    p_->gen++;
}

void NrReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->tel.dialOffsetHz = hz; p_->gen++; }
void NrReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) { p_->gen++; p_->tel.retunes++; }
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool NrReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= nrTuning().minSampleRate - 1; }
void NrReceiver::reset() { p_->gen++; }
void NrReceiver::clearCells() { p_->clearReq = true; std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->clearReq = false; p_->publish(); }
void NrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool NrReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void NrReceiver::clearResults() { clearCells(); }
std::string NrReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool NrReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    NrTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool NrReceiver::telemetry(NrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void NrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center, off;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; off = m.sigOff; }
    if (rate < nrTuning().minSampleRate - 1 || m.want == 0) return;
    for (size_t i = 0; i < n; i++) { const cf32 v = x[i]; if (std::isfinite(v.real()) && std::isfinite(v.imag())) m.levelAcc += std::norm(v); }
    m.levelN += n;
    m.total += n;
    const uint64_t g = m.gen.load();
    if (g != m.accGen) { m.acc.clear(); m.accGen = g; }
    if (m.idle.load()) {
        if (m.acc.empty()) { m.accCenter = center; m.accOff = off; m.acc.reserve(m.want); }
        const size_t take = std::min(n, m.want - m.acc.size());
        for (size_t i = 0; i < take; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            m.acc.push_back(v);
        }
        if (m.acc.size() >= m.want) {
            std::lock_guard<std::mutex> lk(m.mu);
            m.job.swap(m.acc); m.acc.clear();
            m.jobCenter = m.accCenter; m.jobOff = m.accOff; m.jobRate = rate; m.jobSec = (double)m.total / rate; m.jobGen = g;
            m.jobReady = true; m.idle = false;
            m.cv.notify_one();
        }
    }
    const double now = (double)m.total / rate;
    if (now - m.lastPubSec >= 0.5) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.tel.timeSec = now;
        m.tel.levelDb = m.levelN ? (float)(10 * std::log10(m.levelAcc / (double)m.levelN + 1e-20)) : -200.f;
        m.levelAcc = 0; m.levelN = 0;
        m.lastPubSec = now;
        m.refreshState(now);
        if (m.tel.state == 0) m.tel.state = 1;
        m.publish();
    }
}

} // namespace dect2
