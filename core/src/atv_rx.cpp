// Analog TV receiver: the glue between the radio side (atv_front), the picture side (atv_video), the sound and the outside world.
//
//   input -> DC removal -> [searching: spectrum -> vision carrier + sound carrier]
//                       -> [running: vision channel -> picture decoder -> pictures]
//                                    sound channel  -> 48 kHz mono -> sound device
#include "dect2/atv_rx.h"
#include "atv_front.h"
#include "atv_video.h"
#include "dect2/audioout.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dect2 {

struct AtvReceiver::Impl {
    double fs = 0;
    bool ready = false;
    std::mutex mu;
    AtvTelemetry tel;
    uint64_t telSeq = 0;
    std::shared_ptr<const AtvFrame> latest;
    std::function<void(const std::string&)> logCb;

    AtvCarrierSearch search;
    AtvFront front;
    AtvVideo video;
    AtvSound sound;

    // state
    enum Mode { kSearch, kTrial, kRun } mode = kSearch;
    uint64_t nIn = 0;                       // input samples so far
    uint64_t trialAt = 0, lostAt = 0, lastTelAt = 0;
    AtvCarrier carrier;                     // the one being followed
    bool haveCarrier = false;
    std::vector<AtvCarrier> pending;        // the other candidates of the last search, best first
    struct Bad { double hz; uint64_t until; };
    std::vector<Bad> bad;
    cf32 dc = cf32(0, 0);
    float dcA = 2e-4f;
    std::vector<cf32> xd;
    std::vector<float> audioBuf;
    double soundOff = 0;                     // sound carrier spacing in Hz (from the vision carrier)
    int sysHint = 0;

    // output
    std::unique_ptr<AudioOut> audio;
    std::atomic<bool> silent{false}, muted{false};
    std::atomic<float> volume{1.f};
    std::function<void(const float*, const float*, size_t)> tap;
    std::mutex tapMu;
    std::vector<float> inter;

    // controls
    AtvVideoParams prm;
    std::atomic<int> detMode{0};
    std::atomic<int> chanWidthMhz{8};
    std::atomic<bool> prmDirty{false};
    std::mutex prmMu;
    AtvVideoParams prmNew;
    bool syncOn = false;

    void log(const std::string& s) { if (logCb) logCb(s); }

    void configure(double rate) {
        fs = rate;
        ready = false;
        if (rate < 7.9e6) return;
        dcA = (float)(1.0 / (0.0005 * rate));
        search.configure(rate);
        front.configure(rate);
        sound.configure(rate);
        video.configure(front.videoRate(), front.colourCapable());
        video.setNoiseScale(front.noiseGain() * rate / 5e6);
        video.carrierError = [this](double rad, double trust) { front.carrierError(rad, trust); };
        video.log = [this](const std::string& s) { log(s); };
        video.frameReady = [this](std::shared_ptr<const AtvFrame> f) { std::lock_guard<std::mutex> lk(mu); latest = std::move(f); };
        ready = true;
        resetAll();
        if (!silent.load() && !audio) { /* opened on the first audio */ }
    }

    void resetAll() {
        if (!ready) return;
        search.setMonitor(false);
        front.reset();
        front.setPlan(5.5);
        video.setNoiseScale(front.noiseGain() * fs / 5e6);
        pending.clear();
        video.reset();
        sound.reset();
        mode = kSearch; haveCarrier = false; syncOn = false;
        front.setSyncDetector(false);
        dc = cf32(0, 0);
        trialAt = lostAt = 0;
        bad.clear();
        if (audio) audio->flush();
        std::lock_guard<std::mutex> lk(mu);
        tel = AtvTelemetry();
        tel.seq = ++telSeq;
        latest.reset();
    }

    bool isBad(double hz) const {
        for (const auto& b : bad) if (b.until > nIn && std::fabs(hz - b.hz) < 60e3) return true;
        return false;
    }

    bool trialWeak = false;
    uint64_t afcAt = 0;
    double afcPrev = 0;
    bool afcHave = false;
    int goneN = 0;
    bool haveRef = false;
    double refDb = 0;

    // The carrier follows the strongest line in the spectrum near it: the loop on the sync tips holds it to within a few kilohertz, this brings it
    // back from a jump or a drift that is faster or larger than that. Two checks 0.2 s apart have to agree before it moves.
    void afc() {
        if (nIn - afcAt < (uint64_t)(0.2 * fs) || search.spectra() < 6) return;
        afcAt = nIn;
        double f, prom, lvl;
        const double c = front.carrierHz();
        if (!search.peakNear(c, 150e3, f, prom, lvl)) return;
        // the line is as strong as it was when the carrier was taken, within 12 dB; if not, the signal went somewhere else, or went off
        if (!haveRef) { refDb = lvl; haveRef = true; }
        refDb = std::max(refDb - 0.1, lvl);
        if (prom < 12 || lvl < refDb - 12) {
            afcHave = false;
            if (++goneN >= 4) { goneN = 0; log("analog TV: the vision carrier is gone"); backToSearch(); pending.clear(); }
            return;
        }
        goneN = 0;
        if (prom < 20) { afcHave = false; return; }
        const double d = f - c;
        if (std::fabs(d) < 1.5e3) { afcHave = false; return; }
        if (afcHave && std::fabs(d - afcPrev) < 1.0e3) {
            char b[120];
            snprintf(b, sizeof b, "analog TV: vision carrier moved by %+.1f kHz", d / 1e3);
            log(b);
            front.setCarrier(f);
            afcHave = false;
        } else { afcHave = true; afcPrev = d; }
    }
    void startCarrier(const AtvCarrier& c, bool weak = false) {
        carrier = c; haveCarrier = true; trialWeak = weak;
        video.setSlicerWidth(weak ? 2.4 : 1.0);
        front.setPlan(c.spacingMhz > 0 ? c.spacingMhz : 5.5);
        video.setNoiseScale(front.noiseGain() * fs / 5e6);
        search.setMonitor(true);
        front.setCarrier(c.visionHz);
        front.setDsbAmount((float)std::min(1.0, std::max(0.0, (c.dsbDb + 8.0) / 8.0)));
        front.setSyncDetector(false); syncOn = false;
        video.syncDetectorChanged(false);
        video.reset();
        video.setSoundSpacing(c.spacingMhz);
        soundOff = c.spacingMhz > 0 ? c.spacingMhz * 1e6 : 5.5e6;
        sound.reset();
        sound.setSystem(c.spacingMhz > 0 && c.spacingMhz < 5 ? 25 : 50, c.spacingMhz > 0 && c.spacingMhz < 5 ? 75 : 50);
        mode = kTrial; trialAt = nIn; lostAt = 0; goneN = 0; afcHave = false; haveRef = false;
        char b[200];
        snprintf(b, sizeof b, "analog TV: vision carrier at %+.3f MHz (%.0f dB above its surroundings)%s", c.visionHz / 1e6, c.visionDb, c.spacingMhz > 0 ? ", sound carrier found" : "");
        log(b);
    }

    void feed(const cf32* x, size_t n) {
        if (!ready) return;
        {
            std::lock_guard<std::mutex> lk(prmMu);
            if (prmDirty.exchange(false)) { prm = prmNew; video.setParams(prm); }
        }
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min(n - i, AtvFront::kMaxBlock);
            block(x + i, m);
            i += m;
        }
    }

    // the next carrier of the last search that has not failed a trial
    bool nextCandidate() {
        while (!pending.empty()) {
            const AtvCarrier c = pending.front();
            pending.erase(pending.begin());
            if (isBad(c.visionHz) || c.visionDb < 12) continue;
            startCarrier(c);
            return true;
        }
        return false;
    }

    void backToSearch() {
        mode = kSearch; haveCarrier = false; search.setMonitor(false); sound.reset();
        front.setSyncDetector(false); syncOn = false; video.syncDetectorChanged(false);
    }

    void block(const cf32* x, size_t m) {
        // DC removal (the radio's centre spike): the mean of the block moves a slow estimate, once per block. The picture and the sound carrier
        // turn at megahertz, so their mean over a block is nothing; the spike does not turn.
        xd.resize(m);
        {
            // the sums in four lanes: a single running sum would make every addition wait for the last
            float sr4[4] = {0, 0, 0, 0}, si4[4] = {0, 0, 0, 0};
            size_t k = 0;
            for (; k + 4 <= m; k += 4) for (size_t l = 0; l < 4; l++) { sr4[l] += x[k + l].real(); si4[l] += x[k + l].imag(); }
            for (; k < m; k++) { sr4[0] += x[k].real(); si4[0] += x[k].imag(); }
            const float sr = (sr4[0] + sr4[1]) + (sr4[2] + sr4[3]), si = (si4[0] + si4[1]) + (si4[2] + si4[3]);
            const float w = std::min(1.f, dcA * (float)m);
            dc += (cf32(sr, si) / (float)m - dc) * w;
            const float dr = dc.real(), di = dc.imag();
            for (size_t q = 0; q < m; q++) xd[q] = cf32(x[q].real() - dr, x[q].imag() - di);
        }
        nIn += m;
        if (mode == kSearch) {
            search.feed(xd.data(), m);
            if (search.spectra() >= 24) {
                pending = search.candidates();
                if (!nextCandidate()) search.reset();
            }
        } else {
            search.feed(xd.data(), m);          // the spectrum for the display, a few FFTs a second
            runBlock(m);
        }
        if (nIn - lastTelAt >= (uint64_t)(fs * 0.25)) { lastTelAt = nIn; publish(); }
    }

    void runBlock(size_t m) {
        afc();
        const size_t nv = front.process(xd.data(), m);
        if (nv) video.process(front.v(), front.i(), front.q(), nv);
        // the detector: synchronous once the carrier loop is locked (and the user did not ask for the envelope)
        const bool want = detMode.load() == 0 && video.wantSyncDetector();
        if (want != syncOn) { syncOn = want; front.setSyncDetector(want); video.syncDetectorChanged(want); }
        // sound
        sound.setCarrier(front.carrierHz() + soundOff);
        audioBuf.clear();
        sound.process(xd.data(), m, audioBuf);
        emitAudio();
        // supervision
        if (mode == kTrial) {
            if (video.lineLocked()) { mode = kRun; lostAt = 0; log("analog TV: locked"); }
            else if (nIn - trialAt > (uint64_t)((trialWeak ? 1.6 : 1.0) * fs)) {
                if (!trialWeak) {                                    // the lines may be under the noise: the same carrier again with a wider slicer
                    log("analog TV: no lines found, trying again for a weak signal");
                    startCarrier(carrier, true);
                } else {
                    bad.push_back({carrier.visionHz, nIn + (uint64_t)(6 * fs)});
                    log("analog TV: no picture signal on that carrier");
                    if (!nextCandidate()) backToSearch();
                }
            }
        } else if (mode == kRun) {
            if (video.lineLocked()) lostAt = 0;
            else { if (!lostAt) lostAt = nIn; if (nIn - lostAt > (uint64_t)(2.0 * fs)) { log("analog TV: signal lost"); backToSearch(); pending.clear(); } }
        }
    }

    void emitAudio() {
        if (audioBuf.empty()) return;
        const size_t n = audioBuf.size();
        const float vol = volume.load();
        {
            std::lock_guard<std::mutex> lk(tapMu);
            if (tap) tap(audioBuf.data(), audioBuf.data(), n);
        }
        if (silent.load()) return;
        if (!audio) {
            audio = std::make_unique<AudioOut>();
            audio->start(48000);
            audio->setStartThreshold(48000 / 5);
        }
        audio->setVolume(vol); audio->setMuted(muted.load());
        inter.resize(n * 2 + 2);
        for (size_t i = 0; i < n; i++) { inter[2 * i] = audioBuf[i]; inter[2 * i + 1] = audioBuf[i]; }
        int frames = (int)n;
        if (audio->playing()) {
            // the radio's clock and the sound card's differ by a few ppm: keep the queue near 0.2 s by adding or removing one frame per block
            const int queued = audio->bufferedFrames();
            if (queued > 14400 && frames > 1) frames--;
            else if (queued < 4800 && frames > 0) { inter[2 * (size_t)frames] = inter[2 * (size_t)frames - 2]; inter[2 * (size_t)frames + 1] = inter[2 * (size_t)frames - 1]; frames++; }
        }
        audio->write(inter.data(), frames);
    }

    void publish() {
        AtvTelemetry t;
        const AtvVideo::Info vi = video.info();
        t.system = vi.system; t.colourSystem = vi.colourSystem; t.lines = vi.lines;
        t.fieldHz = vi.fieldHz;
        t.colour = vi.colour; t.colourKiller = vi.killer;
        t.syncDetector = syncOn;
        t.snrDb = vi.snrDb;
        t.lineHz = vi.lineHz; t.lineErrPpm = vi.lineErrPpm;
        t.syncQuality = vi.syncQuality;
        t.lineCount = vi.lines_; t.fieldCount = vi.fields; t.frameCount = vi.frames; t.fieldNo = vi.fieldNo;
        t.syncDepthPct = vi.syncTip; t.whitePeak = vi.whitePeak; t.burstLevel = vi.burstLevel; t.syncCompressionPct = vi.syncCompressionPct; t.chromaPhaseErrDeg = vi.chromaErrDeg;
        t.blocksOk = vi.fieldsOk; t.blocksBad = vi.fieldsBad;
        t.lineWave = vi.lineWave; t.vbiWave = vi.vbiWave;
        t.soundPresent = sound.present(); t.soundDevKhz = sound.devKhz(); t.soundLevelDb = sound.levelDb();
        if (haveCarrier) {
            t.visionHz = front.carrierHz();
            t.soundSpacingMhz = carrier.spacingMhz;
            t.soundHz = carrier.spacingMhz > 0 ? t.visionHz + carrier.spacingMhz * 1e6 + sound.trimHz() : 0;
            t.carrierDbfs = vi.carrierLevel > 0 ? (float)(20 * std::log10(vi.carrierLevel)) : -120.f;
            t.carrierToNoiseDb = vi.carrierToNoiseDb;
            // The offset from where the standard puts the carrier in the channel (vision carrier 1.25 MHz above the channel's lower edge: -2.75,
            // -2.25 and -1.75 MHz from the centre of an 8, 7 and 6 MHz channel). The signal cannot say which width the channel has when the
            // sound is at +5.5 MHz (PAL B is 7 MHz, PAL G is 8 MHz, and an 8 MHz channel with the carrier 500 kHz high looks like a 7 MHz one),
            // so that case follows setChannelWidth() and assumes 8 MHz (the tuning table's width) when it is not set. 6.0 and 6.5 MHz
            // only exist in 8 MHz channels, 4.5 MHz and the 525-line systems only in 6 MHz channels.
            static const double kLay[3] = {-2.75e6, -2.25e6, -1.75e6};
            double best = kLay[0];
            if (vi.lines == 525 || carrier.spacingMhz == 4.5) best = kLay[2];
            else if (carrier.spacingMhz == 5.5 || carrier.spacingMhz == 0) {
                const int w = chanWidthMhz.load();
                best = w == 7 ? kLay[1] : w == 6 ? kLay[2] : kLay[0];
            }
            t.cfoHz = t.visionHz - best;
            search.display(t.visionHz, t.specLoMhz, t.specHiMhz, 170, t.specDb);
        }
        t.state = (haveCarrier && mode != kSearch) ? (video.fieldLocked() ? 2 : video.lineLocked() ? 1 : 0) : 0;
        t.dataValid = t.state == 2 || (sound.present() && mode == kRun);
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }

    void setPrm(const AtvVideoParams& p) { std::lock_guard<std::mutex> lk(prmMu); prmNew = p; prmDirty = true; }
    AtvVideoParams curPrm() { std::lock_guard<std::mutex> lk(prmMu); return prmNew; }
};

AtvReceiver::AtvReceiver() : p_(std::make_unique<Impl>()) {}
AtvReceiver::~AtvReceiver() = default;

void AtvReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
bool AtvReceiver::ready() const { return p_->ready && p_->fs >= atvTuning().minSampleRate - 1; }
void AtvReceiver::reset() { p_->resetAll(); }
void AtvReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool AtvReceiver::telemetry(AtvTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
std::shared_ptr<const AtvFrame> AtvReceiver::frame(uint64_t& lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->latest || p_->latest->seq <= lastSeq) return nullptr;
    lastSeq = p_->latest->seq;
    return p_->latest;
}
void AtvReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void AtvReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void AtvReceiver::setMuted(bool m) { p_->muted = m; }
void AtvReceiver::setSilent(bool s) { p_->silent = s; }
void AtvReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->tapMu); p_->tap = std::move(cb); }
void AtvReceiver::setDeinterlace(int mode) { auto p = p_->curPrm(); p.deinterlace = mode; p_->setPrm(p); }
void AtvReceiver::setStandard(int system, int colour) { auto p = p_->curPrm(); p.forceSys = system; p.forceColour = colour; p_->setPrm(p); }
void AtvReceiver::setBlackSetup(int mode) { auto p = p_->curPrm(); p.setupMode = mode; p_->setPrm(p); }
void AtvReceiver::setChromaDelayNs(double ns) { auto p = p_->curPrm(); p.chromaDelayNs = ns; p_->setPrm(p); }
void AtvReceiver::setColour(bool on) { auto p = p_->curPrm(); p.colourOn = on; p_->setPrm(p); }
void AtvReceiver::setSaturation(float s) { auto p = p_->curPrm(); p.saturation = s; p_->setPrm(p); }
void AtvReceiver::setHue(float d) { auto p = p_->curPrm(); p.hueDeg = d; p_->setPrm(p); }
void AtvReceiver::setDetector(int mode) { p_->detMode = mode; }
void AtvReceiver::setChannelWidth(int mhz) { p_->chanWidthMhz = mhz; }

ModeTuning atvTuning() {
    ModeTuning t;
    t.stdMode = 10; t.id = "atv"; t.name = "Analog TV";
    t.minMhz = 45; t.maxMhz = 870; t.defMhz = 600.0;
    t.sampleRate = 10000000.0; t.basebandHz = 9000000.0; t.bandwidthMhz = 8;
    t.minSampleRate = 8000000.0;
    return t;
}

} // namespace dect2
