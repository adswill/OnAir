// MPT 1327 trunking control channel in the engine: mixes the channel to 0 Hz, decimates to about 24 kHz, takes the FM discriminator
// output and hands it to mpt1327::Decoder.
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTargetFs = 24000.0;
constexpr size_t kMaxRows = 200;
constexpr float kAudioScale = 1.0f / 3000.0f;   // discriminator Hz to audio units (any scale works: the detector uses signs)
}

struct MptReceiver::Impl {
    mutable std::mutex mu;
    mpt1327::Decoder dec;
    double rate = 0;
    int decim = 1;
    double fsEff = 0;
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

MptReceiver::MptReceiver() : p_(new Impl) {}
MptReceiver::~MptReceiver() = default;

void MptReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = inputRateHz;
    p_->decim = inputRateHz > kTargetFs ? (int)std::lround(inputRateHz / kTargetFs) : 1;
    if (p_->decim < 1) p_->decim = 1;
    p_->fsEff = inputRateHz / p_->decim;
    p_->clearState();
}

void MptReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; }

void MptReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz;
    p_->snap.centerHz = hz;
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
    const double step = -2.0 * kPi * s.offsetHz / s.rate;
    const size_t block = (size_t)std::llround(s.fsEff / 5.0);   // 0.2 s of audio per decoder call
    for (size_t i = 0; i < n; i++) {
        const std::complex<float> m = x[i] * std::complex<float>((float)std::cos(s.phase), (float)std::sin(s.phase));
        s.phase += step;
        if (s.phase > kPi) s.phase -= 2 * kPi;
        if (s.phase < -kPi) s.phase += 2 * kPi;
        s.acc += m;
        if (++s.accN == s.decim) {
            const std::complex<float> z = s.acc / (float)s.decim;
            s.acc = {0, 0};
            s.accN = 0;
            s.powSum += (double)std::norm(z);
            s.powN++;
            const std::complex<float> d = z * std::conj(s.lastZ);   // FM discriminator
            s.lastZ = z;
            const float hz = (float)(std::atan2(d.imag(), d.real()) * s.fsEff / (2 * kPi));
            s.audio.push_back(hz * kAudioScale);
            if (s.audio.size() >= block) s.processAudio();
        }
    }
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

ModeTuning mpt1327Tuning() {
    ModeTuning t;
    t.stdMode = 129;
    t.id = "mpt1327";
    t.name = "MPT1327 trunking (WIP)";
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
