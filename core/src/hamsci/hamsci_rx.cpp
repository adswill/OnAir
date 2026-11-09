// HamsciReceiver: the engine side of the ham science modes. See hamsci_rx.h. The decoders are in hamsci_dsp.h (tested on their own).
#include "dect2/hamsci/hamsci_rx.h"
#include "dect2/marine/marine_dsp.h"
#include "../hfdig/hfdig_usb.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr double kAudioFs = 12000.0;
constexpr size_t kMaxLines = 100;
constexpr size_t kMaxText = 4000;
}

struct HamsciReceiver::Impl {
    int kind;
    mutable std::mutex mu;
    double rate = 0, offsetHz = 0, centerHz = 0;
    int64_t startUtc = 0;
    std::function<void(const std::string&)> log;
    bool autoP = true;
    // front end (audio kinds, and SuperSID at high rates)
    marine::Front front;
    std::vector<cf32> base, taps;
    std::vector<cf32> hist;          // complex 24 kHz history for the USB filter
    size_t hpos = 0;
    int dphase = 0;
    std::vector<float> audio;
    // direct mixer for low rate SuperSID input
    double nco = 0;
    // decoders
    hamsci::Fsk441Decoder fsk{kAudioFs};
    hamsci::IscatDecoder isc{kAudioFs, 2};
    hamsci::Pi4Decoder pi4{kAudioFs};
    hamsci::ThrobDecoder thr{kAudioFs};
    hamsci::QrssGrabber qrss{kAudioFs, false};
    hamsci::SidMonitor sid;
    hamsci::Riometer rio;
    hamsci::PulsarFolder psr;
    // pulsar worker
    std::thread worker;
    std::atomic<bool> searching{false};
    hamsci::PulsarResult psrResult;
    double psrNext = 20.0;
    double psrNeeded = 0, psrFlux = 0;
    // state
    uint64_t n = 0;                 // input samples
    double nextSnap = 0;
    uint64_t okCount = 0, badCount = 0;
    HamsciTelemetry snap;
    std::vector<HamsciLine> lines;
    std::string text;
    int variant = -1;
    double levelAcc = 0; uint64_t levelN = 0; float levelDb = -200;
    std::string found;
    int state = 0;
    bool sidDirty = true;
    bool manualCentre = false;
    double manualDot = 0;

    explicit Impl(int k) : kind(k) {}
    ~Impl() { if (worker.joinable()) worker.join(); }

    void say(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> l(mu); cb = log; } if (cb) cb(s); }

    void clearAll() {
        n = 0; nextSnap = 0; okCount = badCount = 0; lines.clear(); text.clear(); levelAcc = 0; levelN = 0; levelDb = -200; found.clear(); state = 0;
        base.clear(); hist.assign(taps.size(), cf32(0, 0)); hpos = 0; dphase = 0; audio.clear(); nco = 0;
        fsk.reset(); isc.reset(); pi4.reset(); thr.reset(); qrss.reset(); qrss.setDfcw(kind == kHsDfcw);
        sid.reset(); sidDirty = true;
        rio.reset(); rio.setStartUnix((double)startUtc);
        psr.reset(); psrResult = hamsci::PulsarResult(); psrNext = 20.0;
        snap = HamsciTelemetry(); snap.kind = kind;
    }

    void configureRate(double r) {
        rate = r;
        taps = hfdig::usbTaps(255, marine::kBaseRate);
        startUtc = (int64_t)std::time(nullptr);
        if (rate >= 250000.0) { front.configure(rate); front.setOffsetHz(offsetHz); }
        if (kind == kHsRiometer) rio.configure(rate);
        if (kind == kHsPulsar) {
            hamsci::PulsarConfig c = psr.config();
            c.rateHz = rate; c.centreHz = centerHz > 0 ? centerHz : c.centreHz;
            psr.configure(c);
        }
        clearAll();
    }

    // --- audio path: 24 kHz complex -> USB filter -> 12 kHz real
    void toAudio(const cf32* y, size_t m) {
        audio.clear();
        const size_t L = taps.size();
        for (size_t i = 0; i < m; i++) {
            hist[hpos] = y[i];
            hpos = (hpos + 1) % L;
            if ((dphase++ & 1) == 0) {
                cf32 acc(0, 0);
                size_t idx = hpos;   // oldest
                for (size_t k = 0; k < L; k++) {
                    // y[n] = sum h[k] x[n - k]: x[n - k] is the sample (L - 1 - k) behind the newest
                    const size_t pos = (hpos + L - 1 - k) % L;
                    acc += taps[k] * hist[pos];
                }
                (void)idx;
                audio.push_back(acc.real());
            }
        }
    }

    void pushLine(const std::string& t, double freq, double snr, const std::string& note, double timeSec) {
        HamsciLine l;
        l.timeSec = timeSec; l.utc = startUtc + (int64_t)timeSec; l.text = t; l.freqHz = freq; l.snrDb = snr; l.note = note;
        lines.insert(lines.begin(), l);
        if (lines.size() > kMaxLines) lines.pop_back();
        okCount++;
    }
    void appendText(const std::string& t) {
        text += t;
        if (text.size() > kMaxText) text.erase(0, text.size() - kMaxText);
    }

    void audioToDecoders() {
        if (audio.empty()) return;
        for (float v : audio) { levelAcc += (double)v * v; levelN++; }
        const double tSec = (double)n / rate;
        switch (kind) {
        case kHsFsk441: {
            fsk.feed(audio.data(), audio.size());
            for (auto& b : fsk.take()) { pushLine(b.text, b.freqOffsetHz, b.snrDb, "", b.startSec); state = 3; }
            if (fsk.levelDb() > 3) state = std::max(state, 2);
            break;
        }
        case kHsIscat: {
            isc.setVariant(variant < 0 || variant > 1 ? 2 : variant);
            isc.feed(audio.data(), audio.size());
            for (auto& r : isc.take()) {
                char note[64]; std::snprintf(note, sizeof note, "ISCAT-%c, sync %.0f, %d frames", r.variant ? 'B' : 'A', r.sync, r.frames);
                pushLine(r.text, r.freqOffsetHz, r.snrDb, note, r.startSec);
                found = std::string("ISCAT-") + (r.variant ? "B" : "A") + ", tuning " + std::to_string((int)std::lround(r.freqOffsetHz)) + " Hz";
                state = 3;
            }
            break;
        }
        case kHsPi4: {
            pi4.feed(audio.data(), audio.size());
            for (auto& r : pi4.take()) {
                char note[64]; std::snprintf(note, sizeof note, "tone 0 at %.1f Hz, sync %.2f", r.freqHz, r.sync);
                pushLine(r.text, r.freqHz, r.snrDb, note, r.startSec);
                found = "PI4 at " + std::to_string((int)std::lround(r.freqHz)) + " Hz";
                state = 3;
            }
            break;
        }
        case kHsThrob: {
            thr.setVariant(variant < 0 || variant > 5 ? -1 : variant);
            thr.feed(audio.data(), audio.size());
            for (auto& r : thr.take()) {
                appendText(r.text);
                variant = autoP ? -1 : variant;
                snap.variant = r.variant;
                found = std::string(hamsci::throbName(r.variant)) + " at " + std::to_string((int)std::lround(r.centreHz)) + " Hz";
                snap.snrDb = (float)r.snrDb;
                okCount += r.text.size();
                state = 3;
            }
            break;
        }
        case kHsQrss: case kHsDfcw: {
            qrss.feed(audio.data(), audio.size());
            const auto& st = qrss.state();
            text = st.text;   // the grabber re-reads the whole record each time: its text replaces ours (an earlier letter can change when more keying arrives)
            if (text.size() > kMaxText) text.erase(0, text.size() - kMaxText);
            okCount = text.size();
            if (st.locked || manualCentre) state = std::max(state, 2);
            if (st.dotSec > 0) { state = 3; char b[96]; std::snprintf(b, sizeof b, "tone %.1f Hz, %.1f s elements", st.centreHz, st.dotSec); found = b; }
            break;
        }
        default: break;
        }
        (void)tSec;
    }

    void feedAudioKind(const cf32* x, size_t cnt) {
        base.clear();
        front.process(x, cnt, base);
        for (cf32& v : base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        toAudio(base.data(), base.size());
        audioToDecoders();
    }

    void applySidConfig() {
        const double dial = centerHz + offsetHz;
        const double r = rate >= 250000.0 ? marine::kBaseRate : rate;
        sid.configure(r, dial);
        sidDirty = false;
    }

    void feedSid(const cf32* x, size_t cnt) {
        if (sidDirty) applySidConfig();
        if (rate >= 250000.0) {
            base.clear();
            front.process(x, cnt, base);
            sid.feed(base.data(), base.size());
        } else {
            std::vector<cf32> y(cnt);
            const double w = -2 * 3.14159265358979 * offsetHz / rate;
            for (size_t i = 0; i < cnt; i++) {
                nco += w;
                if (nco < -6.2831853071795862) nco += 6.2831853071795862;
                y[i] = x[i] * cf32((float)std::cos(nco), (float)std::sin(nco));
            }
            sid.feed(y.data(), cnt);
        }
    }

    void startPulsarSearch() {
        if (searching.exchange(true)) return;
        if (worker.joinable()) worker.join();
        const std::vector<float> copy = psr.series();
        const hamsci::PulsarConfig cfg = psr.config();
        hamsci::PulsarFolder* f = &psr;
        (void)f;
        worker = std::thread([this, copy, cfg] {
            hamsci::PulsarFolder tmp(cfg);
            hamsci::PulsarResult r = tmp.searchSeries(copy);
            { std::lock_guard<std::mutex> l(mu); psrResult = r; }
            searching = false;
        });
    }

    void buildSnap() {
        HamsciTelemetry t;
        t.kind = kind;
        t.inputRate = rate; t.centerHz = centerHz; t.timeSec = (double)n / std::max(1.0, rate);
        t.autoParams = autoP;
        t.blocksOk = okCount; t.blocksBad = badCount;
        t.lines = lines; t.text = text; t.variant = snap.variant; t.found = found; t.snrDb = snap.snrDb;
        t.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-12, levelAcc / (double)levelN))) : levelDb;
        if (levelN) { levelDb = t.levelDb; levelAcc = 0; levelN = 0; }
        t.dataValid = okCount > 0;
        if (kind == kHsQrss || kind == kHsDfcw) { t.qrss = qrss.state(); t.snrDb = (float)t.qrss.snrDb; t.cfoHz = t.qrss.centreHz; t.dataValid = !t.text.empty(); }
        if (kind == kHsSid) { t.sid = sid.state(); t.dataValid = sid.state().timeSec > 5; state = sid.state().eventsTotal ? 3 : (sid.state().timeSec > 5 ? 2 : 1); }
        if (kind == kHsRiometer) {
            t.rio = rio.state(); t.dataValid = !t.rio.powerHistory.empty();
            state = t.rio.event ? 3 : t.dataValid ? 2 : 1;
            char b[96]; std::snprintf(b, sizeof b, "%s", t.rio.haveQdc ? "quiet-day curve ready" : "building the quiet-day curve (needs 1 to 2 days)"); found = b; t.found = found;
        }
        if (kind == kHsPulsar) {
            t.pulsarObservedSec = psr.observedSec();
            t.pulsarSearching = searching.load();
            { std::lock_guard<std::mutex> l(mu); t.pulsar = psrResult; }
            t.dataValid = t.pulsar.ok;
            state = t.pulsar.ok ? 3 : t.pulsarObservedSec > 10 ? 2 : 1;
            t.snrDb = (float)t.pulsar.snr;
            t.pulsarNeededM2 = psrNeeded;
        }
        t.state = n == 0 ? 0 : std::max(1, state);
        if (kind <= kHsThrob && t.state < 2 && n > 0) t.state = 1;
        t.seq = snap.seq + 1;
        snap = std::move(t);
    }
};

HamsciReceiver::HamsciReceiver(int kind) : p_(new Impl(std::clamp(kind, 0, (int)kHamsciKinds - 1))), kind_(std::clamp(kind, 0, (int)kHamsciKinds - 1)) {
    p_->qrss.setDfcw(kind_ == kHsDfcw);
    p_->snap.kind = kind_;
    p_->taps = hfdig::usbTaps(255, marine::kBaseRate);
    p_->hist.assign(p_->taps.size(), cf32(0, 0));
}
HamsciReceiver::~HamsciReceiver() = default;

void HamsciReceiver::configure(double r) { std::lock_guard<std::mutex> l(p_->mu); p_->configureRate(r); }
void HamsciReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; if (p_->rate >= 250000.0) p_->front.setOffsetHz(hz); p_->sidDirty = true; }
void HamsciReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz; p_->sidDirty = true;
    if (kind_ == kHsPulsar && p_->rate > 0) { hamsci::PulsarConfig c = p_->psr.config(); if (std::fabs(c.centreHz - hz) > 1.0 && hz > 1e6) { c.centreHz = hz; p_->psr.configure(c); } }
}
bool HamsciReceiver::ready() const { std::lock_guard<std::mutex> l(p_->mu); return p_->rate >= hamsciTuning(kind_).minSampleRate; }
void HamsciReceiver::reset() { std::lock_guard<std::mutex> l(p_->mu); if (p_->rate > 0) p_->clearAll(); }
void HamsciReceiver::clearResults() { std::lock_guard<std::mutex> l(p_->mu); p_->lines.clear(); p_->text.clear(); p_->okCount = p_->badCount = 0; p_->found.clear(); }
void HamsciReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->log = std::move(cb); }

void HamsciReceiver::setAuto(bool on) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->autoP = on;
    if (on) { p_->variant = -1; p_->manualCentre = false; p_->manualDot = 0; p_->qrss.setAutoCentre(true); p_->qrss.setDotSec(0); }
}
bool HamsciReceiver::autoParams() const { std::lock_guard<std::mutex> l(p_->mu); return p_->autoP; }
void HamsciReceiver::setQrssCentreHz(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->autoP = false; p_->manualCentre = true; p_->qrss.setCentreHz(hz); }
void HamsciReceiver::setQrssDotSec(double s) { std::lock_guard<std::mutex> l(p_->mu); p_->autoP = false; p_->manualDot = s; p_->qrss.setDotSec(s); }
void HamsciReceiver::setDfcwReverse(bool on) { std::lock_guard<std::mutex> l(p_->mu); p_->qrss.setDfcwReverse(on); }
void HamsciReceiver::setVariant(int v) { std::lock_guard<std::mutex> l(p_->mu); p_->variant = v; p_->autoP = (v < 0 || (kind_ == kHsIscat && v == 2)); }
void HamsciReceiver::setSidRiseDb(double db) { std::lock_guard<std::mutex> l(p_->mu); p_->sid.setRiseThresholdDb(std::clamp(db, 0.3, 12.0)); }
void HamsciReceiver::setRioLongitude(double lon) { std::lock_guard<std::mutex> l(p_->mu); p_->rio.setPosition(std::clamp(lon, -180.0, 180.0)); }
void HamsciReceiver::setRioThresholdDb(double db) { std::lock_guard<std::mutex> l(p_->mu); p_->rio.setThresholdDb(std::clamp(db, 0.05, 6.0)); }
std::string HamsciReceiver::rioExportQdc() const { std::lock_guard<std::mutex> l(p_->mu); return p_->rio.exportQdc(); }
bool HamsciReceiver::rioImportQdc(const std::string& s) { std::lock_guard<std::mutex> l(p_->mu); return p_->rio.importQdc(s); }
void HamsciReceiver::setPulsarTarget(double periodS, double dm, double flux) {
    std::lock_guard<std::mutex> l(p_->mu);
    hamsci::PulsarConfig c = p_->psr.config();
    c.periodS = std::clamp(periodS, 0.0015, 10.0); c.dm = std::max(0.0, dm);
    p_->psrFlux = flux;
    p_->psr.configure(c);
    p_->psrResult = hamsci::PulsarResult();
    p_->psrNext = 20.0;
}
void HamsciReceiver::pulsarSearchNow() { std::lock_guard<std::mutex> l(p_->mu); if (kind_ == kHsPulsar) p_->startPulsarSearch(); }

void HamsciReceiver::feed(const cf32* x, size_t cnt) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> l(s.mu);
    if (s.rate <= 0 || s.rate < hamsciTuning(kind_).minSampleRate) return;
    // input level of the whole capture for the non-audio modes
    switch (kind_) {
    case kHsSid: s.feedSid(x, cnt); break;
    case kHsRiometer: s.rio.feed(x, cnt); break;
    case kHsPulsar: {
        s.psr.feed(x, cnt);
        const double obs = s.psr.observedSec();
        if (obs >= s.psrNext && !s.searching.load()) { s.psrNext = obs + std::max(15.0, obs * 0.3); s.startPulsarSearch(); }
        break;
    }
    default: s.feedAudioKind(x, cnt); break;   // the audio modes need the channel front end (a radio rate of 250 kHz or more)
    }
    s.n += cnt;
    const double t = (double)s.n / s.rate;
    if (t >= s.nextSnap || s.snap.seq == 0) { s.nextSnap = t + 0.25; s.buildSnap(); }
}

bool HamsciReceiver::telemetry(HamsciTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.centerHz = p_->centerHz;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------- tuning, names
const char* hamsciId(int kind) {
    static const char* id[kHamsciKinds] = {"qrss", "dfcw", "fsk441", "iscat", "pi4", "throb", "supersid", "riometer", "pulsar"};
    return id[std::clamp(kind, 0, (int)kHamsciKinds - 1)];
}
const char* hamsciName(int kind) {
    static const char* nm[kHamsciKinds] = {"QRSS grabber", "DFCW grabber", "FSK441", "ISCAT", "PI4", "Throb", "SuperSID (VLF)", "Riometer", "Pulsar folding"};
    return nm[std::clamp(kind, 0, (int)kHamsciKinds - 1)];
}

ModeTuning hamsciTuning(int kind) {
    kind = std::clamp(kind, 0, (int)kHamsciKinds - 1);
    ModeTuning t;
    t.stdMode = hamsciStd(kind);
    t.id = hamsciId(kind);
    t.name = hamsciName(kind);
    t.sampleRate = 2e6;
    t.bandwidthMhz = 0.3;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 0;
    switch (kind) {
    case kHsQrss: t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = 10.140; break;
    case kHsDfcw: t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = 0.1375; break;
    case kHsFsk441: t.minMhz = 28; t.maxMhz = 1300; t.defMhz = 144.140; break;
    case kHsIscat: t.minMhz = 28; t.maxMhz = 1300; t.defMhz = 144.140; break;
    case kHsPi4: t.minMhz = 28; t.maxMhz = 24000; t.defMhz = 1296.0; break;
    case kHsThrob: t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.075; break;
    case kHsSid: t.minMhz = 0.003; t.maxMhz = 0.1; t.defMhz = 0.022; t.sampleRate = 250000; t.bandwidthMhz = 0.2; t.minSampleRate = 48000; break;
    case kHsRiometer: t.minMhz = 20; t.maxMhz = 60; t.defMhz = 30.0; t.sampleRate = 2e6; t.bandwidthMhz = 1.0; t.minSampleRate = 48000; t.notCircular = true; break;
    default: t.minMhz = 100; t.maxMhz = 6000; t.defMhz = 408.0; t.sampleRate = 2.4e6; t.bandwidthMhz = 2.4; t.minSampleRate = 1e6; break;
    }
    return t;
}

std::string hamsciSummary(const HamsciTelemetry& t) {
    char b[240];
    const char* nm = hamsciName(t.kind);
    if (t.state == 0) { std::snprintf(b, sizeof b, "%s: waiting for samples", nm); return b; }
    switch (t.kind) {
    case kHsQrss: case kHsDfcw:
        if (!t.qrss.locked && t.qrss.dotSec <= 0) std::snprintf(b, sizeof b, "%s: looking for a steady tone", nm);
        else std::snprintf(b, sizeof b, "%s: tone %.1f Hz%s%s", nm, t.qrss.centreHz, t.qrss.dotSec > 0 ? ", elements " : ", reading the keying", t.qrss.dotSec > 0 ? (std::to_string((int)std::lround(t.qrss.dotSec)) + " s").c_str() : "");
        break;
    case kHsSid: {
        int present = 0;
        for (auto& s : t.sid.stations) present += s.present;
        std::snprintf(b, sizeof b, "SuperSID: %d stations heard, %d sudden changes", present, t.sid.eventsTotal);
        break;
    }
    case kHsRiometer:
        if (t.rio.powerHistory.empty()) std::snprintf(b, sizeof b, "Riometer: measuring the first minute");
        else if (!t.rio.haveQdc) std::snprintf(b, sizeof b, "Riometer: %.1f dB, quiet-day curve %d of 288 bins", (double)t.rio.powerDb, t.rio.qdcFill);
        else std::snprintf(b, sizeof b, "Riometer: absorption %.2f dB", (double)t.rio.absorptionDb);
        break;
    case kHsPulsar:
        if (t.pulsar.ok) std::snprintf(b, sizeof b, "Pulsar: detected, S/N %.1f, period %.6f s, DM %.1f", t.pulsar.snr, t.pulsar.periodS, t.pulsar.dm);
        else std::snprintf(b, sizeof b, "Pulsar: no detection yet (%.0f s observed, best S/N %.1f)", t.pulsarObservedSec, t.pulsar.snr);
        break;
    default:
        if (!t.lines.empty()) std::snprintf(b, sizeof b, "%s: %llu decodes, last: %.60s", nm, (unsigned long long)t.blocksOk, t.lines[0].text.c_str());
        else if (!t.text.empty()) std::snprintf(b, sizeof b, "%s: %llu characters%s%s", nm, (unsigned long long)t.blocksOk, t.found.empty() ? "" : ", ", t.found.c_str());
        else std::snprintf(b, sizeof b, "%s: listening", nm);
        break;
    }
    return b;
}

}  // namespace dect2
