// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// See avroad_rx.h. The receiver moves the signal to 0 Hz and hands it to the decoder of the active mode; it keeps the list of what was decoded.
#include "avroad_int.h"
#include "dect2/results/results_io.h"
#include <deque>
#include <mutex>

namespace dect2 {

using namespace avroad;

const char* avroad::variantName(int v) {
    static const char* n[kNumVariants] = {"FANET", "OGN tracker", "GBAS VDB", "LDACS", "AeroMACS", "End of train", "DSRC toll", "802.11p V2X", "C-V2X", "ERMES", "Marine radar", "Weather radar"};
    return v >= 0 && v < kNumVariants ? n[v] : "";
}

namespace {
constexpr size_t kMaxRows = 500;
constexpr double kReportSec = 0.25, kDecodingSec = 8, kEnergySec = 2;
}

struct AvRoadReceiver::Impl : Sink {
    mutable std::mutex procMu, telMu;
    int v = -1;
    double fs = 0, offsetHz = 0, centreHz = 0, phase = 0;
    bool isReady = false;
    std::unique_ptr<Decoder> dec;
    std::vector<cf32> mix;
    uint64_t nIn = 0;
    std::deque<AvRoadRow> rows[kNumVariants];
    uint64_t ok = 0, nok = 0;
    float snr = 0; double cfo = 0;
    double lastGood = -1e9, lastEnergy = -1e9, nextReport = 0;
    float level = -120;
    uint64_t seq = 0;
    AvRoadTelemetry pub;
    std::function<void(const std::string&)> log;

    double now() const override { return fs > 0 ? (double)nIn / fs : 0; }
    void row(const char* kind, const std::string& id, const std::string& text, float levelDb, const double* ll) override {
        if (v < 0) return;
        AvRoadRow r; r.timeSec = now(); r.kind = kind; r.id = id; r.text = text; r.levelDb = levelDb;
        if (ll) { r.hasPos = true; r.lat = ll[0]; r.lon = ll[1]; }
        rows[v].push_front(std::move(r));
        if (rows[v].size() > kMaxRows) rows[v].pop_back();
    }
    void good(float s, double c) override { ok++; snr = s; cfo = c; lastGood = now(); lastEnergy = lastGood; }
    void bad() override { nok++; lastEnergy = now(); }
    void energy() override { lastEnergy = now(); }

    void build() {
        dec.reset(); isReady = false;
        if (v < 0 || fs <= 0) return;
        const ModeTuning t = avRoadTuning(v);
        if (fs < t.minSampleRate - 1) return;
        dec = makeDecoder(v);
        if (!dec) return;
        dec->attach(this);
        dec->setCenterHz(centreHz);
        isReady = dec->configure(fs);
    }
    void publish() {
        AvRoadTelemetry t;
        t.seq = ++seq; t.variant = v; t.timeSec = now(); t.inputRate = fs; t.levelDb = level;
        t.blocksOk = ok; t.blocksBad = nok; t.snrDb = snr; t.cfoHz = cfo;
        t.state = now() - lastGood < kDecodingSec ? 2 : now() - lastEnergy < kEnergySec ? 1 : 0;
        t.dataValid = t.state == 2;
        if (dec) dec->fill(t);
        if (v >= 0) t.rows.assign(rows[v].begin(), rows[v].end());
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(t);
    }
    void feed(const cf32* x, size_t n) {
        if (!isReady || !n) return;
        mix.resize(n);
        double p = phase; const double dp = -2 * kPi * offsetHz / fs;
        float e = 0;
        if (offsetHz != 0) for (size_t i = 0; i < n; i++) { mix[i] = x[i] * cf32((float)std::cos(p), (float)std::sin(p)); p += dp; if (p > kPi) p -= 2 * kPi; else if (p < -kPi) p += 2 * kPi; }
        else std::copy(x, x + n, mix.begin());
        phase = p;
        for (size_t i = 0; i < n; i += 16) e += std::norm(mix[i]);
        level = toDb(e / (float)std::max<size_t>(1, n / 16));
        nIn += n;
        dec->feed(mix.data(), n);
        if (now() >= nextReport) { nextReport = now() + kReportSec; publish(); }
    }
};

AvRoadReceiver::AvRoadReceiver() : p_(new Impl) {}
AvRoadReceiver::~AvRoadReceiver() = default;
void AvRoadReceiver::setVariant(int v) { std::lock_guard<std::mutex> lk(p_->procMu); p_->v = v; }
int AvRoadReceiver::variant() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->v; }
void AvRoadReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->procMu); p_->fs = r; p_->nIn = 0; p_->nextReport = 0; p_->lastGood = p_->lastEnergy = -1e9; p_->build(); }
void AvRoadReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; }
void AvRoadReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->centreHz = hz; if (p_->dec) p_->dec->setCenterHz(hz); }
bool AvRoadReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void AvRoadReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->dec) p_->dec->reset(); p_->phase = 0; }
void AvRoadReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool AvRoadReceiver::telemetry(AvRoadTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AvRoadReceiver::clearResults() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->v >= 0) p_->rows[p_->v].clear(); p_->ok = p_->nok = 0; }
void AvRoadReceiver::clearResults(int v) { std::lock_guard<std::mutex> lk(p_->procMu); if (v >= 0 && v < kNumVariants) p_->rows[v].clear(); }
std::string AvRoadReceiver::saveResults(int v) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (v < 0 || v >= kNumVariants) return {};
    std::vector<AvRoadRow> r(p_->rows[v].begin(), p_->rows[v].end());
    return results::pack(r);
}
bool AvRoadReceiver::loadResults(const std::string& blob, int v) {
    std::vector<AvRoadRow> r;
    if (v < 0 || v >= kNumVariants || !results::unpack(blob, r)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->rows[v].assign(r.begin(), r.end());
    return true;
}
void AvRoadReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

} // namespace dect2
