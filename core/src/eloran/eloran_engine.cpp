// eLoran / Loran-C in the engine: mixes the carrier to 0 Hz, decimates to 250 kHz or more, low-pass filters to +-11 kHz,
// keeps the last 2 s and runs the detector once a second.
#include "dect2/eloran/eloran_engine.h"
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTargetFs = 250000.0;
constexpr double kCutoffHz = 11000.0;
constexpr int kTaps = 129;
constexpr double kWindowS = 2.0;
}

struct ElLoranReceiver::Impl {
    mutable std::mutex mu;
    double rate = 0;
    int decim = 1;
    double fs = 0;
    double offsetHz = 0;
    double centerHz = 0;
    double phase = 0;
    std::complex<float> acc{0, 0};
    int accN = 0;
    std::vector<float> taps;
    std::vector<std::complex<float>> hist;   // circular, kTaps
    int hpos = 0;
    std::vector<std::complex<float>> ring;   // the last 2 s
    size_t wr = 0, filled = 0, sinceRun = 0;
    int lastGri = 0, consec = 0;
    uint64_t seq = 0;
    ElLoranTelemetry snap;
    std::function<void(const std::string&)> log;

    void emit(const std::string& s) { if (log) log(s); }

    void clearState() {
        acc = {0, 0};
        accN = 0;
        phase = 0;
        hist.assign(kTaps, std::complex<float>(0, 0));
        hpos = 0;
        ring.assign((size_t)std::llround(fs * kWindowS), std::complex<float>(0, 0));
        wr = filled = 0;
        sinceRun = (size_t)std::llround(fs);   // the first analysis comes as soon as 2 s are in
        lastGri = consec = 0;
        snap = ElLoranTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
    }

    void makeTaps() {
        taps.assign(kTaps, 0.f);
        const double fc = kCutoffHz / fs;
        double sum = 0;
        for (int i = 0; i < kTaps; i++) {
            const double t = i - (kTaps - 1) / 2.0;
            const double sinc = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
            const double win = 0.54 - 0.46 * std::cos(2 * kPi * i / (kTaps - 1));
            taps[(size_t)i] = (float)(sinc * win);
            sum += taps[(size_t)i];
        }
        for (float& v : taps) v = (float)(v / sum);
    }

    void analyse() {
        std::vector<std::complex<float>> lin(ring.size());
        for (size_t i = 0; i < ring.size(); i++) lin[i] = ring[(wr + i) % ring.size()];
        loran::Detector det(fs);
        const loran::Result r = det.analyze(lin.data(), lin.size(), lastGri);
        if (r.found) {
            consec = (r.griUs == lastGri) ? consec + 1 : 1;
            lastGri = r.griUs;
            snap.blocksOk++;
        } else {
            consec = 0;
            lastGri = 0;
            snap.blocksBad++;
        }
        snap.dataValid = r.found;
        snap.griUs = r.found ? r.griUs : 0;
        { const char* cn = r.found ? loran::chainName(r.griUs) : nullptr; snap.chain = cn ? cn : ""; }
        snap.zScore = r.zScore;
        snap.cfoHz = r.found && !r.stations.empty() ? r.stations[0].cfoHz : 0.0;
        snap.masterFound = r.found && r.masterFound;
        snap.locked = r.found && r.masterFound && consec >= 2;
        snap.state = !r.found ? 1 : snap.locked ? 3 : 2;
        snap.stations = r.stations;
        snap.pulseEnv = r.pulseEnv;
        snap.envStartUs = r.envStartUs;
        snap.envStepUs = r.envStepUs;
        snap.levelDb = -200;
        snap.snrDb = 0;
        for (const loran::Station& s : r.stations)
            if (s.levelDb > snap.levelDb) { snap.levelDb = s.levelDb; snap.snrDb = s.marginDb; }
        if (r.found && (consec == 1 || consec == 2)) {
            char b[120];
            snprintf(b, sizeof b, "Loran-C: GRI %d, %d station(s)%s", r.griUs / 10, (int)r.stations.size(), snap.locked ? ", locked" : "");
            emit(b);
        }
        snap.seq = ++seq;
    }
};

ElLoranReceiver::ElLoranReceiver() : p_(new Impl) {}
ElLoranReceiver::~ElLoranReceiver() = default;

void ElLoranReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = inputRateHz;
    p_->decim = inputRateHz >= kTargetFs ? (int)std::floor(inputRateHz / kTargetFs) : 1;
    if (p_->decim < 1) p_->decim = 1;
    p_->fs = inputRateHz / p_->decim;
    p_->makeTaps();
    p_->clearState();
}

void ElLoranReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->offsetHz = hz;
}

void ElLoranReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz;
    p_->snap.centerHz = hz;
}

bool ElLoranReceiver::ready() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->rate >= elLoranTuning().minSampleRate && p_->fs > 0;
}

void ElLoranReceiver::reset() {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->fs > 0) p_->clearState();
}

void ElLoranReceiver::feed(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& s = *p_;
    if (s.fs <= 0 || s.rate < elLoranTuning().minSampleRate) return;
    const double step = -2.0 * kPi * s.offsetHz / s.rate;
    const size_t hop = (size_t)std::llround(s.fs);
    for (size_t i = 0; i < n; i++) {
        const std::complex<float> m = x[i] * std::complex<float>((float)std::cos(s.phase), (float)std::sin(s.phase));
        s.phase += step;
        if (s.phase > kPi) s.phase -= 2 * kPi;
        if (s.phase < -kPi) s.phase += 2 * kPi;
        s.acc += m;
        if (++s.accN < s.decim) continue;
        const std::complex<float> d = s.acc / (float)s.decim;
        s.acc = {0, 0};
        s.accN = 0;
        s.hist[(size_t)s.hpos] = d;
        std::complex<float> y(0, 0);
        for (int t = 0; t < kTaps; t++) y += s.taps[(size_t)t] * s.hist[(size_t)((s.hpos - t + kTaps) % kTaps)];
        s.hpos = (s.hpos + 1) % kTaps;
        s.ring[s.wr] = y;
        s.wr = (s.wr + 1) % s.ring.size();
        if (s.filled < s.ring.size()) s.filled++;
        if (s.filled == s.ring.size() && ++s.sinceRun >= hop) {
            s.sinceRun = 0;
            s.analyse();
        }
    }
    if (s.snap.seq == 0 && s.snap.state == 0 && s.filled > 0) {
        s.snap.state = 1;
        s.snap.seq = ++s.seq;
    }
}

bool ElLoranReceiver::telemetry(ElLoranTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.inputRate = p_->rate;
    out.centerHz = p_->centerHz;
    return true;
}

void ElLoranReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->log = std::move(cb);
}

ModeTuning elLoranTuning() {
    ModeTuning t;
    t.stdMode = 128;
    t.id = "eloran";
    t.name = "eLoran / Loran-C";
    t.minMhz = 0.090;
    t.maxMhz = 0.110;
    t.defMhz = 0.100;
    t.sampleRate = 500000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.03;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 30000;     // the radio sits 30 kHz above the carrier, the receiver mixes it out
    return t;
}

std::string elLoranSummary(const ElLoranTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Loran-C: waiting for samples";
    if (!t.dataValid) return "Loran-C: searching for a GRI";
    snprintf(b, sizeof b, "Loran-C: GRI %d, %d station(s)%s", t.griUs / 10, (int)t.stations.size(), t.locked ? ", locked" : "");
    return b;
}

}  // namespace dect2
