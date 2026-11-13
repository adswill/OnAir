// MPT 1327 trunking control channel in the engine: mixes the channel to 0 Hz, decimates to about 24 kHz, takes the FM discriminator
// output and hands it to mpt1327::Decoder. A wide capture (1 Msps and up) cuts the control channel out with a FIR channel filter first
// (SliceDdc), so the other channels of the capture do not alias into it. The GTCs' traffic channels inside the capture are followed as FM
// slices of the same samples, several at once (tpool::OneRadio, docs/TRUNK1-3.9.md).
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/mpt1327/mpt1327_chan.h"
#include "dect2/trunkpool/one_radio.h"
#include "dect2/trunkpool/slice_ddc.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/dsp/spectral_afc.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTargetFs = 24000.0;
constexpr double kNarrowHz = 4500.0;
constexpr size_t kMaxRows = 200;
constexpr float kAudioScale = 1.0f / 3000.0f;   // discriminator Hz to audio units (any scale works: the detector uses signs)
}

struct MptReceiver::Impl {
    mutable std::mutex mu;
    mpt1327::Decoder dec;
    double rate = 0;
    int decim = 1;
    double fsEff = 0;
    // 4.0 (docs/SENSITIVITY-4.0-A2.md): a narrow channel filter next to the plain average. The signal's centre is found from the power
    // spectrum (open loop); once two looks agree the stream is shifted to 0 Hz, filtered at kNarrowHz and shifted back, so the
    // discriminator sees the same frequencies as without the filter.
    SpectralAfc afc;
    FmBiquad nbRe[2], nbIm[2];
    bool narrowOn = false;
    bool weakPath = true;                 // false: the 3.9 receiver without the narrow filter (setWeakSignalPath, for before / after measurements)
    double narrowF0 = 0, narrowPh = 0;
    double offsetHz = 0;
    double centerHz = 0;
    double phase = 0;
    std::complex<float> acc{0, 0};
    int accN = 0;
    std::complex<float> lastZ{0, 0};
    std::vector<float> audio;
    double powSum = 0;
    size_t powN = 0;
    uint64_t seq = 0;
    uint64_t seenMessages = 0;
    MptTelemetry snap;
    std::map<int, MptChannelRow> chans;
    std::function<void(const std::string&)> log;
    // the wide capture's control channel filter
    tpool::SliceDdc ccDdc;
    bool useDdc = false;
    // single-radio trunking
    tpool::OneRadio one{"mpt1327", 4};
    bool follow = true;
    int ctrlCh = -1, listenTg = -1, playing = -1;
    double spacing = 12500;
    std::function<void(int, const float*, size_t, int)> audioTap;
    std::unique_ptr<AudioOut> audioOut;
    std::vector<float> stereo;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};

    double controlHz() const { return centerHz > 0 ? centerHz + offsetHz : 0; }
    void setupFront() {
        useDdc = rate >= 1e6 && ccDdc.configure(rate, offsetHz, 9000, 48000);
        const double r = useDdc ? ccDdc.outRate() : rate;
        decim = r > kTargetFs ? (int)std::lround(r / kTargetFs) : 1;
        if (decim < 1) decim = 1;
        fsEff = r > 0 ? r / decim : 0;
        afc.configure(fsEff > 0 ? fsEff : 24000.0); afc.setOpenLoop(true);
        for (int i = 0; i < 2; i++) { nbRe[i].lowpass(kNarrowHz, std::max(fsEff, 1.0), i ? 1.3066 : 0.5412); nbIm[i].lowpass(kNarrowHz, std::max(fsEff, 1.0), i ? 1.3066 : 0.5412); }
        narrowOn = false; narrowF0 = 0; narrowPh = 0;
    }
    void play(int tg, const float* x, size_t cnt, int r) {
        if (audioTap) audioTap(tg, x, cnt, r);
        if (silent.load() || tg != playing || tg < 0) return;
        const int up = r > 0 ? std::max(1, (int)std::lround(48000.0 / r)) : 1;
        stereo.resize(cnt * (size_t)up * 2);
        for (size_t i = 0; i < cnt; i++)
            for (int k = 0; k < up; k++) stereo[(i * (size_t)up + (size_t)k) * 2] = stereo[(i * (size_t)up + (size_t)k) * 2 + 1] = x[i];
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 5); }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        audioOut->write(stereo.data(), (int)(cnt * (size_t)up));
    }
    void grant(const mpt1327::Message& m) {
        if (m.channel < 0 || m.called.ident < 0) return;
        const int called = (m.called.pfix >= 0 ? m.called.pfix << 13 : 0) | m.called.ident;
        const int caller = m.caller.ident;
        const double cc = controlHz();
        if (ctrlCh < 0 || !(cc > 0)) { one.onControlSeen(); return; }   // the channel plan is not known: nothing can be placed
        tpool::GrantEvent g = tpool::grantMpt1327(m.channel, ctrlCh, cc, spacing, called, caller, true);
        if (!(g.freqHz > 0)) return;
        one.onGrant(g);
    }
    void followSnapshot() {
        snap.follow = follow; snap.voiceSlots = one.slots(); snap.controlHz = controlHz();
        snap.planControlChannel = ctrlCh; snap.planSpacingHz = spacing;
        snap.calls.clear();
        playing = -1;
        for (const tpool::OneRadioCall& c : one.calls(64)) {
            MptCall r;
            r.id = c.id; r.called = c.tg; r.caller = c.src; r.freqHz = c.freqHz; r.active = c.active; r.followed = c.followed;
            r.voiceSlot = c.voiceSlot; r.durationSec = c.durSec; r.state = c.state; r.note = c.note;
            if (ctrlCh >= 0 && spacing > 0 && controlHz() > 0) r.channel = ctrlCh + (int)std::lround((c.freqHz - controlHz()) / spacing);
            if (r.active && r.followed && playing < 0 && (listenTg < 0 || listenTg == r.called)) playing = r.called;
            snap.calls.push_back(std::move(r));
        }
        if (!follow) snap.followStatus = "Following is off";
        else if (ctrlCh < 0) snap.followStatus = "Enter the control channel number and the spacing to follow the calls";
        else if (!(centerHz > 0)) snap.followStatus = "The frequency of the capture is not known (file input): the calls cannot be placed";
        else snap.followStatus = one.status();
        snap.followNote = one.lastNote();
    }

    void emit(const std::string& s) { if (log) log(s); }

    void clearState() {
        dec.reset();
        audio.clear();
        acc = {0, 0}; accN = 0; lastZ = {0, 0}; phase = 0;
        powSum = 0; powN = 0;
        seenMessages = 0;
        chans.clear();
        snap = MptTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
    }

    void processAudio() {
        dec.process(audio.data(), audio.size(), fsEff);
        audio.clear();
        const mpt1327::Stats& st = dec.stats();
        for (const mpt1327::Event& e : dec.takeEvents()) {
            MptMessageRow r;
            r.t = e.t;
            r.type = e.msg.type;
            r.called = mpt1327::addressText(e.msg.called);
            r.caller = e.msg.caller.ident >= 0 ? mpt1327::addressText(e.msg.caller) : "-";
            r.channel = e.msg.channel;
            r.detail = e.msg.detail;
            snap.rows.push_back(r);
            if (e.msg.type == "GTC") grant(e.msg);
            else if (!e.msg.type.empty()) one.onControlSeen();
            if (snap.rows.size() > kMaxRows) snap.rows.erase(snap.rows.begin(), snap.rows.begin() + (long)(snap.rows.size() - kMaxRows));
            if (e.msg.channel >= 0) {
                MptChannelRow& c = chans[e.msg.channel];
                c.channel = e.msg.channel;
                c.count++;
                c.lastT = e.t;
                if (e.msg.type == "GTC") { c.called = r.called; c.caller = r.caller; }
                else c.controlChannel = true;
            }
        }
        snap.channels.clear();
        for (const auto& kv : chans) snap.channels.push_back(kv.second);
        snap.good = st.good;
        snap.bad = st.bad;
        snap.messages = st.messages;
        snap.sysId = st.sys;
        snap.locked = st.locked;
        snap.syncTolerant = st.tolerant;
        snap.dataValid = st.locked;
        snap.blocksOk = st.good;
        snap.blocksBad = st.bad;
        snap.state = st.locked ? 2 : 1;
        followSnapshot();
        if (powN) snap.levelDb = (float)(10.0 * std::log10(powSum / (double)powN + 1e-20));
        powSum = 0; powN = 0;
        if (snap.messages != seenMessages) {
            seenMessages = snap.messages;
            char b[120];
            snprintf(b, sizeof b, "MPT1327: %llu messages, system %d", (unsigned long long)seenMessages, snap.sysId);
            emit(b);
        }
        snap.seq = ++seq;
    }
};

MptReceiver::MptReceiver() : p_(new Impl) {
    Impl* ip = p_.get();
    p_->one.setAudioTap([ip](int tg, const float* x, size_t n, int r) { ip->play(tg, x, n, r); });
}
MptReceiver::~MptReceiver() = default;

void MptReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = inputRateHz;
    p_->setupFront();
    p_->one.configure(inputRateHz);
    p_->one.setCentreHz(p_->centerHz);
    p_->one.setControlHz(p_->controlHz());
    p_->clearState();
}

void MptReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (hz == p_->offsetHz) return;
    p_->offsetHz = hz;
    if (p_->useDdc || p_->rate >= 1e6) p_->setupFront();
    p_->one.setControlHz(p_->controlHz());
}

void MptReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz;
    p_->snap.centerHz = hz;
    p_->one.setCentreHz(hz);
    p_->one.setControlHz(p_->controlHz());
}

bool MptReceiver::ready() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->rate >= mpt1327Tuning().minSampleRate && p_->fsEff > 0;
}

void MptReceiver::setSyncMode(mpt1327::SyncMode m) { std::lock_guard<std::mutex> l(p_->mu); p_->dec.setSyncMode(m); }
mpt1327::SyncMode MptReceiver::syncMode() const { std::lock_guard<std::mutex> l(p_->mu); return p_->dec.syncMode(); }

void MptReceiver::reset() { std::lock_guard<std::mutex> l(p_->mu); p_->clearState(); }

void MptReceiver::feed(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& s = *p_;
    if (s.fsEff <= 0) return;
    const std::complex<float>* in = x;
    size_t m = n;
    double step = -2.0 * kPi * s.offsetHz / s.rate;
    if (s.useDdc) {   // a wide capture: the channel filter first (the channel is at 0 Hz after it)
        const std::vector<cf32>& c = s.ccDdc.process(x, n);
        in = c.data(); m = c.size(); step = 0;
    }
    const size_t block = (size_t)std::llround(s.fsEff / 5.0);   // 0.2 s of audio per decoder call
    for (size_t i = 0; i < m; i++) {
        const std::complex<float> mx = step == 0 ? in[i] : in[i] * std::complex<float>((float)std::cos(s.phase), (float)std::sin(s.phase));
        s.phase += step;
        if (s.phase > kPi) s.phase -= 2 * kPi;
        if (s.phase < -kPi) s.phase += 2 * kPi;
        s.acc += mx;
        if (++s.accN == s.decim) {
            std::complex<float> z = s.acc / (float)s.decim;
            s.acc = {0, 0};
            s.accN = 0;
            s.afc.push(&z, 1);
            if (s.afc.settled() && (!s.narrowOn || std::fabs(s.afc.adjustHz() - s.narrowF0) > 150.0)) { s.narrowOn = true; s.narrowF0 = s.afc.adjustHz(); }
            if (s.weakPath && s.narrowOn) {
                s.narrowPh -= 2 * kPi * s.narrowF0 / s.fsEff;
                if (s.narrowPh < -kPi) s.narrowPh += 2 * kPi;
                const double c = std::cos(s.narrowPh), sn = std::sin(s.narrowPh);
                double re = z.real() * c - z.imag() * sn, im = z.real() * sn + z.imag() * c;
                for (int i = 0; i < 2; i++) { re = s.nbRe[i].run(re); im = s.nbIm[i].run(im); }
                z = std::complex<float>((float)(re * c + im * sn), (float)(im * c - re * sn));
            }
            s.powSum += (double)std::norm(z);
            s.powN++;
            const std::complex<float> d = z * std::conj(s.lastZ);   // FM discriminator
            s.lastZ = z;
            const float hz = (float)(std::atan2(d.imag(), d.real()) * s.fsEff / (2 * kPi));
            s.audio.push_back(hz * kAudioScale);
            if (s.audio.size() >= block) s.processAudio();
        }
    }
    s.one.setEnabled(s.follow);
    s.one.feed(x, n);   // the calls inside the capture, from the same samples
}

bool MptReceiver::telemetry(MptTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.inputRate = p_->rate;
    out.centerHz = p_->centerHz;
    return true;
}

void MptReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->log = std::move(cb);
}

void MptReceiver::setWeakSignalPath(bool on) { std::lock_guard<std::mutex> l(p_->mu); p_->weakPath = on; }
void MptReceiver::setFollow(bool on) { std::lock_guard<std::mutex> l(p_->mu); p_->follow = on; p_->snap.follow = on; }
void MptReceiver::setVoiceSlots(int n) { std::lock_guard<std::mutex> l(p_->mu); p_->one.setSlots(n); p_->snap.voiceSlots = p_->one.slots(); }
void MptReceiver::setChannelPlan(int controlChannel, double spacingHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->ctrlCh = controlChannel;
    if (spacingHz > 0) p_->spacing = spacingHz;
    p_->snap.planControlChannel = p_->ctrlCh; p_->snap.planSpacingHz = p_->spacing;
}
void MptReceiver::setRecordDir(const std::string& dir) { std::lock_guard<std::mutex> l(p_->mu); p_->one.setRecordDir(dir); }
void MptReceiver::setListenTg(int called) { std::lock_guard<std::mutex> l(p_->mu); p_->listenTg = called; }
void MptReceiver::setSilent(bool s) { p_->silent = s; }
void MptReceiver::setVolume(float v) { p_->volume = v; }
void MptReceiver::setMuted(bool m) { p_->muted = m; }
void MptReceiver::setAudioTap(std::function<void(int, const float*, size_t, int)> cb) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->audioTap = std::move(cb);
    Impl* ip = p_.get();
    p_->one.setAudioTap([ip](int tg, const float* x, size_t n, int r) { ip->play(tg, x, n, r); });
}

ModeTuning mpt1327Tuning() {
    ModeTuning t;
    t.stdMode = 129;
    t.id = "mpt1327";
    t.name = "MPT1327 trunking";
    t.minMhz = 30;
    t.maxMhz = 520;
    t.defMhz = 170;
    t.sampleRate = 240000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 50000;     // the radio sits 50 kHz above the channel, the receiver mixes it out
    return t;
}

std::string mpt1327Summary(const MptTelemetry& t) {
    char b[160];
    if (t.state == 0) return "MPT1327: waiting for samples";
    if (!t.locked) {
        snprintf(b, sizeof b, "MPT1327: searching for a control channel (%.0f dBFS)", t.levelDb);
        return b;
    }
    snprintf(b, sizeof b, "MPT1327: system %d, %llu messages", t.sysId, (unsigned long long)t.messages);
    return b;
}

}  // namespace dect2
