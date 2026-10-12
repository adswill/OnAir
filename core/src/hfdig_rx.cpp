// HF digital receiver (see hfdig_rx.h): the channel to 24 kHz complex, the upper sideband to 8 kHz audio, the three decoders on that
// audio, and FreeDV's speech to the sound card. The decoders themselves are in hfdig_rtty.cpp, hfdig_sstv.cpp and hfdig_freedv.cpp.
#include "dect2/hfdig_rx.h"
#include "dect2/results_io.h"
#include "dect2/hfdig_tel.h"
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_freedv.h"
#include "dect2/hfdig_ftx.h"
#include "dect2/hfdig_hamdrm.h"
#include "dect2/marine_dsp.h"
#include "dect2/audioout.h"
#include "ham_listen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct HfdigReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> audioTapSet, speechTapSet;
    double rate = 0;                       // from configure()
    double offsetHz = 0;                   // setSignalOffset()
    HfdigTelemetry pub;                    // the published report
    std::atomic<bool> resetReq{true}, offsetReq{false}, tapsReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    // receiver thread
    HfdigTelemetry tel;
    double curRate = 0;
    bool running = false;                  // the rate is high enough for the front end
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0, audioPower = 0;      // sum of |x|^2 of the input and of the sideband since the last report
    int64_t nPower = 0, nAudio = 0;
    marine::Front front;
    hfdig::UsbAudio usb;
    hfdig::Listen listen;                  // "Listen": the sideband audio on the sound card (off by default)
    std::vector<cf32> base;
    std::vector<float> audio;
    std::unique_ptr<HfdigRtty> rtty = makeRttyDecoder();
    std::unique_ptr<HfdigSstv> sstv = makeSstvDecoder();
    std::unique_ptr<HfdigFreedv> freedv = makeFreedvDecoder();
    std::unique_ptr<HfdigFtx> ftx = makeFtxDecoder();
    std::unique_ptr<HfdigHamdrm> hamdrm = makeHamdrmDecoder();
    HfdigDecoder* decoders[5] = {rtty.get(), sstv.get(), freedv.get(), ftx.get(), hamdrm.get()};
    std::atomic<int> decMask{31};
    std::atomic<int> demodSet{HfDemodUsb}; // setDemod()          // setDecoders(): which of the five run
    std::function<void(const float*, size_t)> audioTap, speechTap;
    hfdig::Upsample6 up;
    std::vector<float> pcm;
    std::unique_ptr<AudioOut> audioOut;

    Impl() { freedv->setSpeechOut([this](const float* x, size_t n) { playSpeech(x, n); }); }

    // the decoded results stay through resets (each decoder keeps its own); Clear and a loaded file reach the decoders through these
    // requests (mu), the published report at once
    int clearMask = 0;
    bool loadReq = false;
    HfdigTelemetry loaded;
    void takeRequests() {        // the receiver thread
        std::lock_guard<std::mutex> lk(mu);
        if (clearMask & 1) rtty->clearText();
        if (clearMask & 2) sstv->setHistory({});
        if (clearMask & 8) ftx->clearDecodes();
        if (clearMask & 16) hamdrm->setHistory({});
        clearMask = 0;
        if (loadReq) {
            rtty->setText(loaded.rtty.text, loaded.rtty.chars);
            sstv->setHistory(loaded.sstv.history);
            ftx->setDecodes(loaded.ftx.decodes);
            hamdrm->setHistory(loaded.hamdrm.history);
            loaded = HfdigTelemetry();
            loadReq = false;
        }
    }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = HfdigTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = audioPower = 0; nPower = nAudio = 0;
        running = curRate >= hfdigTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); up.reset();
        base.clear(); audio.clear(); pcm.clear();
        for (HfdigDecoder* d : decoders) d->reset();
        hamdrm->setLogCallback(cb);
        if (audioOut) audioOut->flush();
        listen.reset();
        if (cb && curRate > 0) cb("HF digital: listening to the sideband audio for RTTY, SSTV, FreeDV, FT8, FT4, FT2, WSPR and HamDRM");
    }

    void playSpeech(const float* x, size_t n) {
        if (speechTap) speechTap(x, n);
        if (silent.load() || n == 0) return;
        pcm.clear();
        up.process(x, n, pcm);
        if (!audioOut) {
            audioOut = std::make_unique<AudioOut>();
            audioOut->start(48000);
            audioOut->setStartThreshold(48000 / 4);
        }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();   // the radio's clock runs ahead of the sound card: catch up
        audioOut->write(pcm.data(), (int)(pcm.size() / 2));
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        tel.audioDb = nAudio ? (float)(10 * std::log10(audioPower / (double)nAudio + 1e-20)) : -200.f;
        power = audioPower = 0; nPower = nAudio = 0;
        rtty->telemetry(tel.rtty);
        sstv->telemetry(tel.sstv);
        freedv->telemetry(tel.freedv);
        ftx->telemetry(tel.ftx);
        hamdrm->telemetry(tel.hamdrm);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

HfdigReceiver::HfdigReceiver() : p_(std::make_unique<Impl>()) {}
HfdigReceiver::~HfdigReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void HfdigReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void HfdigReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool HfdigReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= hfdigTuning().minSampleRate - 1;
}
void HfdigReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    HfdigTelemetry t;                    // the decoded results stay (clearResults)
    t.rtty.text = std::move(p_->pub.rtty.text); t.rtty.chars = p_->pub.rtty.chars;
    t.sstv.history = std::move(p_->pub.sstv.history);
    t.ftx.decodes = std::move(p_->pub.ftx.decodes);
    t.hamdrm.history = std::move(p_->pub.hamdrm.history);
    p_->pub = std::move(t);
    p_->pub.seq = s;
}

void HfdigReceiver::clearResults(int mask) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.clearMask |= mask;
    if (mask & 1) { m.pub.rtty.text.clear(); m.pub.rtty.chars = 0; m.loaded.rtty.text.clear(); m.loaded.rtty.chars = 0; }
    if (mask & 2) { m.pub.sstv.history.clear(); m.loaded.sstv.history.clear(); }
    if (mask & 8) { m.pub.ftx.decodes.clear(); m.loaded.ftx.decodes.clear(); }
    if (mask & 16) { m.pub.hamdrm.history.clear(); m.loaded.hamdrm.history.clear(); }
    if (mask & 8) m.ftx->clearDecodes();   // thread safe: at once, also when the receiver is not running
    m.pub.seq++;
}
std::string HfdigReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const HfdigTelemetry& t = p_->pub;
    return results::pack(t.rtty.text, t.rtty.chars, t.sstv.history, t.ftx.decodes, t.hamdrm.history);
}
bool HfdigReceiver::loadResults(const std::string& blob) {
    HfdigTelemetry l;
    if (!results::unpack(blob, l.rtty.text, l.rtty.chars, l.sstv.history, l.ftx.decodes, l.hamdrm.history)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.pub.rtty.text = l.rtty.text; m.pub.rtty.chars = l.rtty.chars;
    m.pub.sstv.history = l.sstv.history; m.pub.ftx.decodes = l.ftx.decodes; m.pub.hamdrm.history = l.hamdrm.history;
    m.ftx->setDecodes(l.ftx.decodes);   // thread safe: the FT8 screen reads the decoder itself
    m.loaded = std::move(l); m.loadReq = true;
    m.pub.seq++;
    return true;
}

void HfdigReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.tapsReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.audioTap = m.audioTapSet; m.speechTap = m.speechTapSet; }
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    m.takeRequests();
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) m.power += (double)std::norm(x[i]);
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        if (const int d = hfDemodResolve(m.demodSet.load(), 0); d != m.usb.demod()) m.usb.setDemod(d);
        m.base.clear(); m.audio.clear();
        m.front.process(x, n, m.base);
        m.usb.process(m.base.data(), m.base.size(), m.audio, m.audioPower);
        m.nAudio += (int64_t)m.audio.size();
        if (!m.audio.empty()) {
            if (m.audioTap) m.audioTap(m.audio.data(), m.audio.size());
            const int dm = m.decMask.load();
            for (int k = 0; k < 5; k++) if (dm & (1 << k)) m.decoders[k]->feedAudio(m.audio.data(), m.audio.size());
            if (dm != 4) m.listen.play(m.audio.data(), m.audio.size());   // FreeDV alone plays its speech instead
        }
    }
    // about four reports a second of signal
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool HfdigReceiver::telemetry(HfdigTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void HfdigReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void HfdigReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); p_->listen.volume = p_->volume.load(); }
void HfdigReceiver::setMuted(bool m) { p_->muted = m; p_->listen.muted = m; }
void HfdigReceiver::setSilent(bool s) { p_->silent = s; p_->listen.silent = s; }
void HfdigReceiver::setListen(bool on) { p_->listen.on = on; }
bool HfdigReceiver::listening() const { return p_->listen.on.load(); }
void HfdigReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void HfdigReceiver::setAudioTap(std::function<void(const float* x, size_t n)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->audioTapSet = std::move(cb); p_->tapsReq = true; }
void HfdigReceiver::setSpeechTap(std::function<void(const float* x, size_t n)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->speechTapSet = std::move(cb); p_->tapsReq = true; }

HfdigRtty& HfdigReceiver::rtty() { return *p_->rtty; }
void HfdigReceiver::setDecoders(int mask) { p_->decMask = mask & 31; }
HfdigHamdrm& HfdigReceiver::hamdrm() { return *p_->hamdrm; }
int HfdigReceiver::decoders() const { return p_->decMask.load(); }
void HfdigReceiver::setDemod(int d) { p_->demodSet = d; }
int HfdigReceiver::demod() const { return p_->demodSet.load(); }
HfdigFtx& HfdigReceiver::ftx() { return *p_->ftx; }

ModeTuning hfdigTuning() {
    ModeTuning t;
    t.stdMode = 27; t.id = "hfdig"; t.name = "HF digital";
    t.minMhz = 1.0; t.maxMhz = 30; t.defMhz = 14.230;   // 14.230 MHz: the 20 m SSTV calling frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.003;
    t.minSampleRate = 250000;                            // the front end takes any rate down to its 96 kHz first stage: as PSK and JS8
    t.tuneOffsetHz = 20000;                             // as marine: the radio sits 20 kHz above the dial frequency, away from its DC spike
    return t;
}

} // namespace dect2
