// TD-SCDMA cells receiver (tdscdma_rx.h).
#include "dect2/tdscdma/tdscdma_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace dect2 {

ModeTuning tdscdmaTuning() {
    ModeTuning t;
    t.stdMode = 663; t.id = "tdscdma"; t.name = "TD-SCDMA cells";
    t.minMhz = 1880; t.maxMhz = 2400; t.defMhz = 2017.4;   // band A (2010 - 2025 MHz), band F (1880 - 1920), band E (2300 - 2400)
    t.sampleRate = 2560000; t.basebandHz = 1750000;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 1600000;
    return t;
}

struct TdscdmaReceiver::Impl {
    double fs = 0, offset = 0;
    RationalResampler rs; bool rsOk = false;
    std::vector<cf32> buf, temp; double mixPh = 0;
    const size_t kBlock = (size_t)(tdscdma::kFs * 0.08);
    std::mutex mu; TdscdmaTelemetry tel; uint64_t seq = 0;
    std::function<void(const std::string&)> log;
    std::thread th; mutable std::mutex jm; std::condition_variable jc; std::vector<cf32> job; bool have = false, stop = false;
    std::atomic<bool> working{false};
    uint64_t scanNo = 0;
    Impl() { th = std::thread([this] { run(); }); }
    ~Impl() { { std::lock_guard<std::mutex> k(jm); stop = true; } jc.notify_all(); th.join(); }
    void run() {
        for (;;) {
            std::vector<cf32> b;
            { std::unique_lock<std::mutex> k(jm); jc.wait(k, [&] { return have || stop; }); if (stop) return; b.swap(job); have = false; working = true; }
            double ms = 0;
            auto found = tdscdma::search(b.data(), b.size(), {}, &ms);
            std::lock_guard<std::mutex> k(mu);
            tel.scans = ++scanNo; tel.lastScanMs = ms; tel.state = 1;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            for (auto& c : found) {
                TdscdmaCellRow* r = nullptr;
                for (auto& e : tel.cells) if (e.syncDl == c.syncDl) r = &e;
                if (!r) {
                    tel.cells.push_back({}); r = &tel.cells.back(); r->syncDl = c.syncDl;
                    if (log) { char m[128]; snprintf(m, sizeof m, "TD-SCDMA: SYNC-DL %d (%.1f dBFS)", c.syncDl, c.levelDbfs); log(m); }
                }
                const bool sure = c.evenFirst >= 0 && c.cellParam >= 0;
                if (c.cellParam >= 0 && (sure || !r->paramSure)) {
                    if (log && sure && (!r->paramSure || r->cellParam != c.cellParam)) { char m[128]; snprintf(m, sizeof m, "TD-SCDMA: SYNC-DL %d is cell parameter %d", c.syncDl, c.cellParam); log(m); }
                    r->cellParam = c.cellParam; r->paramSure = sure;
                }
                r->levelDbfs = c.levelDbfs; r->snrDb = c.snrDb; r->cfoHz = c.cfoHz; r->midScore = c.midScore;
                r->blocks++; r->quadS1 += (uint64_t)c.quadS1; r->quadS2 += (uint64_t)c.quadS2; r->lastSeen = now;
            }
            std::sort(tel.cells.begin(), tel.cells.end(), [](const TdscdmaCellRow& a, const TdscdmaCellRow& b2) { return a.levelDbfs > b2.levelDbfs; });
            tel.dataValid = !tel.cells.empty(); tel.blocksOk = tel.cells.size(); tel.blocksBad = 0;
            tel.cfoHz = tel.cells.empty() ? 0 : tel.cells[0].cfoHz; tel.snrDb = tel.cells.empty() ? 0.f : (float)tel.cells[0].snrDb;
            tel.seq = ++seq;
            working = false;
        }
    }
    void push(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            buf.push_back(x[i]);
            if (buf.size() == kBlock) {
                std::lock_guard<std::mutex> k(jm);
                if (!have) { job.swap(buf); have = true; jc.notify_one(); }
                buf.clear();
            }
        }
    }
};

TdscdmaReceiver::TdscdmaReceiver() : p_(new Impl) {}
TdscdmaReceiver::~TdscdmaReceiver() = default;
void TdscdmaReceiver::configure(double fs) { p_->fs = fs; p_->rsOk = p_->rs.configure(fs, tdscdma::kFs, nullptr, 1.6e6); p_->buf.clear(); }
bool TdscdmaReceiver::ready() const { return p_->fs >= tdscdmaTuning().minSampleRate - 1 && p_->rsOk; }
void TdscdmaReceiver::setSignalOffset(double hz) { p_->offset = hz; }
void TdscdmaReceiver::reset() { p_->buf.clear(); p_->mixPh = 0; if (p_->rsOk) p_->rs.reset(); }
void TdscdmaReceiver::clearResults() { std::lock_guard<std::mutex> k(p_->mu); p_->tel.cells.clear(); p_->tel.seq = ++p_->seq; }
void TdscdmaReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> k(p_->mu); p_->log = std::move(cb); }
bool TdscdmaReceiver::busy() const { std::lock_guard<std::mutex> k(p_->jm); return p_->have || p_->working; }
void TdscdmaReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rsOk) return;
    I.temp.assign(x, x + n);
    if (I.offset != 0) {
        const double w = -2 * M_PI * I.offset / I.fs;
        for (auto& v : I.temp) { v *= cf32((float)std::cos(I.mixPh), (float)std::sin(I.mixPh)); I.mixPh = std::fmod(I.mixPh + w, 2 * M_PI); }
    }
    std::vector<cf32> o; o.reserve(n + 64);
    I.rs.process(I.temp.data(), I.temp.size(), o);
    I.push(o.data(), o.size());
}
bool TdscdmaReceiver::telemetry(TdscdmaTelemetry& out, uint64_t lastSeq) { std::lock_guard<std::mutex> k(p_->mu); if (p_->tel.seq == lastSeq) return false; out = p_->tel; return true; }

namespace {
// Built-in test signal: two synchronised cells (cell parameters 37 and 90), 80 ms looped (a whole number of quadruples)
class TdscdmaSynth : public ModeSynth {
public:
    TdscdmaSynth(const SynthConfig& c, double rate) : rate_(rate > 0 ? rate : tdscdma::kFs) {
        std::vector<tdscdma::TxCell> cells(2);
        cells[0].cellParam = 37; cells[0].sfn0 = 100; cells[0].cfoHz = c.cfoHz; cells[0].chipOffset = 1200;
        cells[1].cellParam = 90; cells[1].sfn0 = 100; cells[1].cfoHz = c.cfoHz + 90; cells[1].chipOffset = 1205; cells[1].amp = 0.5;
        std::vector<cf32> s = tdscdma::generate(cells, (size_t)(tdscdma::kFs * 0.08), c.snrDb);
        if (std::fabs(rate_ - tdscdma::kFs) > 1) {
            RationalResampler r;
            if (r.configure(tdscdma::kFs, rate_)) { std::vector<cf32> o; r.process(s.data(), s.size(), o); s.swap(o); }
            else rate_ = tdscdma::kFs;
        }
        double p = 0; for (auto& v : s) p += std::norm(v);
        const float g = (float)(0.22 / std::sqrt(p / std::max<size_t>(1, s.size())));
        for (auto& v : s) v *= g;
        s_.swap(s);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) { out[i] = s_[pos_]; pos_ = (pos_ + 1) % s_.size(); } }
private:
    double rate_; std::vector<cf32> s_; size_t pos_ = 0;
};
}  // namespace

std::unique_ptr<ModeSynth> makeTdscdmaSynth(const SynthConfig& c, double sampleRate) { return std::make_unique<TdscdmaSynth>(c, sampleRate); }

} // namespace dect2
