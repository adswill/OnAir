// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT cells receiver (nbiot_rx.h): blocks of input, raster points searched, cells found and their broadcast decoded.
#include "dect2/common/thread_name.h"
#include "dect2/nbiot/nbiot_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/nbiot/nbiot_phy.h"
#include "dect2/nbiot/nbiot_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

ModeTuning nbiotTuning() {
    ModeTuning t;
    t.stdMode = 71; t.id = "nbiot"; t.name = "NB-IoT cells";
    t.minMhz = 450; t.maxMhz = 2200; t.defMhz = 806;
    t.sampleRate = 2000000;      // 1.2 MHz of the band at a time: a dozen raster points; even an RTL-SDR (up to 3.2 Msps) does this
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.75;
    t.minSampleRate = 1000000;
    return t;
}

const std::vector<int>& nbiotBands() {
    static const std::vector<int> b = {20, 8, 3, 1, 28, 5, 2, 4, 12, 13, 17, 18, 19, 25, 26, 66, 71, 85};
    return b;
}

namespace {

constexpr double kBlockSec = 1.0;
constexpr double kRaster = 100e3;

int64_t wallNow() { return dect2::unixNow(); }

struct Cand { double off; bool snapped; double score; };

} // namespace

struct NbiotReceiver::Impl {
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
    NbiotTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-nbiot", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        NbEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kNbLogMax) tel.events.resize(kNbLogMax);
        if (log) log("nbiot: " + s);
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

    void process(const std::vector<cf32>& x, double center, double dialOff, double fs, double sec, uint64_t g) {
        std::vector<Cand> cands;
        auto add = [&](double off, bool snapIt, double score) {
            if (center > 0 && snapIt) off = std::round((center + off) / kRaster) * kRaster - center;
            for (const auto& o : cands) if (std::fabs(o.off - off) < 50e3) return;
            cands.push_back({off, snapIt, score});
        };
        add(dialOff, center > 0, 0);
        // a quick NPSS look at every raster point inside the sample band; the ones that show one are searched in full
        if (center > 0) {
            const double lim = fs / 2 * 0.9 - 150e3;
            std::vector<double> offs;
            for (long k = (long)std::ceil((center - lim) / kRaster); k * kRaster <= center + lim; k++) offs.push_back(k * kRaster - center);
            const std::vector<nbiot::Probe> pr = nbiot::probe(x.data(), x.size(), fs, offs, 22500, 4.6f);
            std::vector<nbiot::Probe> srt = pr;
            std::sort(srt.begin(), srt.end(), [](const nbiot::Probe& a, const nbiot::Probe& b) { return a.score > b.score; });
            size_t kept = 0;
            for (const auto& p : srt) { if (kept++ >= 6) break; add(p.offsetHz, true, p.score); }
        }
        std::vector<std::pair<Cand, nbiot::CellResult>> found;
        for (const Cand& c : cands) {
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            nbiot::SearchOpts o;
            o.maxCfoHz = 22500;
            std::vector<nbiot::CellResult> r = nbiot::searchDecode(x.data(), x.size(), fs, c.off, o);
            for (auto& cr : r) found.push_back({c, std::move(cr)});
        }
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.lastBlockCenterHz = center;
        for (auto& fr : found) merge(fr.first, fr.second, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const NbCell& a, const NbCell& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kNbCellMax) tel.cells.resize(kNbCellMax);
        refreshState(sec);
        publish();
    }

    void refreshState(double sec) {
        const NbCell* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 10) recent = true;
            if (sec - c.lastSec < 10 && (!best || c.levelDb > best->levelDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->snrDb; }
    }

    void merge(const Cand& cd, const nbiot::CellResult& r, double center, double sec) {
        const double actual = (center > 0 ? center : 0.0) + cd.off + r.found.cfoHz;
        const double nominal = center > 0 ? std::round(actual / kRaster) * kRaster : cd.off;
        const int ear = center > 0 ? lteHzToEarfcn(nominal) : -1;
        NbCell* t = nullptr;
        for (auto& c : tel.cells) if (c.pci == r.found.pci && std::fabs(c.freqHz - nominal) < 60e3) { t = &c; break; }
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(NbCell());
            t = &tel.cells.back();
            t->freqHz = nominal; t->earfcn = ear; t->pci = r.found.pci; t->firstWall = wallNow();
            t->band = ear >= 0 ? lteBandOfEarfcn(ear) : 0;
        }
        t->offsetKhz = center > 0 ? (actual - nominal) / 1e3 : 0;
        if (r.mib.ok) {
            const nbiot::Mib& m = r.mib.mib;
            t->hasMib = true; t->mode = m.mode; t->ports = r.mib.ports; t->sfn = r.mib.sfn; t->hsfnLsb = m.hsfnLsb; t->sched = m.sched; t->valueTag = m.valueTag; t->ab = m.ab;
            if (m.mode == nbiot::kInbandDiff || m.mode == nbiot::kGuardband) t->offsetKhz = nbiot::rasterOffsetKhz(m.raster);   // the cell says it
        }
        t->levelDb = r.meas.levelDb; t->snrDb = r.meas.snrDb; t->npssScore = r.found.npssScore;
        t->cfoHz = center > 0 ? actual - nominal - t->offsetKhz * 1e3 : r.found.cfoHz;   // the radio's tuning error once the raster offset is taken out
        t->clockPpm = r.found.clockPpm;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        char b[256];
        if (isNew) {
            if (center > 0) snprintf(b, sizeof b, "PCI %d at %.1f MHz (EARFCN %d)%s%s", t->pci, nominal / 1e6, ear, r.mib.ok ? ", " : ", synchronisation signals only", r.mib.ok ? nbiot::opModeName(r.mib.mib.mode) : "");
            else snprintf(b, sizeof b, "PCI %d at %+.1f kHz%s%s", t->pci, nominal / 1e3, r.mib.ok ? ", " : ", synchronisation signals only", r.mib.ok ? nbiot::opModeName(r.mib.mib.mode) : "");
            event(b, sec);
        }
        tel.blocksBad += (uint64_t)std::max(0, r.sib1Tries - (r.sib1Ok ? 1 : 0));
        if (r.sib1Ok) {
            const nbiot::Sib1& s = r.sib1;
            const bool first = !t->hasSib1 || t->cellId != s.cellId || t->tac != s.tac;
            tel.blocksOk++;
            t->sib1Msgs++;
            t->hasSib1 = true;
            t->plmns.clear();
            for (const auto& p : s.plmns) t->plmns.push_back(cell::plmnText(p));
            if (!s.plmns.empty()) { t->mcc = s.plmns[0].mcc; t->mnc = s.plmns[0].mnc; t->mncDigits = s.plmns[0].mncDigits; t->reservedOp = s.plmns[0].reserved; }
            t->op = lteOperatorName(t->mcc, t->mnc);
            t->tac = s.tac; t->cellId = s.cellId; t->enb = s.cellId >> 8; t->sector = (int)(s.cellId & 255);
            t->barred = s.barred; t->qRxLevMinDbm = s.qRxLevMinDbm; t->pMaxDbm = s.pMax;
            if (s.band > 0) t->band = s.band;
            if (first) {
                snprintf(b, sizeof b, "PCI %d: %s%s%s, TAC %u, eNB %u sector %d, band %d, %s", t->pci, t->plmns.empty() ? "?" : t->plmns[0].c_str(), t->op.empty() ? "" : " ",
                         t->op.c_str(), t->tac, t->enb, t->sector, t->band, nbiotModeName(t->mode));
                event(b, sec);
            }
        }
    }
};

NbiotReceiver::NbiotReceiver() : p_(new Impl) {}
NbiotReceiver::~NbiotReceiver() = default;

void NbiotReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * kBlockSec);
    p_->gen++;
}

void NbiotReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->tel.dialOffsetHz = hz; p_->gen++; }
void NbiotReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) { p_->gen++; p_->tel.retunes++; }
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool NbiotReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= nbiotTuning().minSampleRate - 1; }
void NbiotReceiver::reset() { p_->gen++; }
void NbiotReceiver::clearCells() { p_->clearReq = true; std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->clearReq = false; p_->publish(); }
void NbiotReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool NbiotReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void NbiotReceiver::clearResults() { clearCells(); }
std::string NbiotReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool NbiotReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    NbiotTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool NbiotReceiver::telemetry(NbiotTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void NbiotReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center, off;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; off = m.sigOff; }
    if (rate < nbiotTuning().minSampleRate - 1 || m.want == 0) return;
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
