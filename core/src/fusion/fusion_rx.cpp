// Yaesu System Fusion receiver (see fusion_rx.h).
#include "dect2/fusion/fusion_rx.h"
#include "dect2/dsp/cic3.h"
#include "dect2/fusion/fusion_proto.h"
#include "dect2/audio/audioout.h"
#include "dect2/voice/voice_monitor.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 48000;           // demodulator rate, 10 samples per symbol
constexpr int kSps = 10;
constexpr int kFrame = fusion::kFrameDibits * kSps;   // samples per frame (100 ms)
constexpr int kYMask = 16383;

float pearson(const float* v, const int8_t* s, int n) {
    double mv = 0, ms = 0;
    for (int i = 0; i < n; i++) { mv += v[i]; ms += s[i]; }
    mv /= n; ms /= n;
    double c = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) { const double x = v[i] - mv, y = s[i] - ms; c += x * y; a += x * x; b += y * y; }
    return a > 0 && b > 0 ? (float)(c / std::sqrt(a * b)) : 0.f;
}
inline int slice(float z) { return z >= 2 ? 3 : z >= 0 ? 1 : z >= -2 ? -1 : -3; }

std::string remText(const uint8_t* p, size_t n) {
    bool print = true;
    for (size_t i = 0; i < n; i++) if (p[i] != 0 && (p[i] < 32 || p[i] > 126)) print = false;
    if (print) return fusion::trimField(p, n);
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; }
    return s;
}
} // namespace

struct FusionReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0, centerHz = 0;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;

    // voice
    std::mutex codecMu;
    std::unique_ptr<codec::SpeechCodec> codec;
    std::string codecText = "codec not installed";
    bool codecChanged = true;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    voice::Monitor* monSet = nullptr;   // setVoiceMonitor; else voice::monitor("ysf") while not silent
    voice::CallInfo playing;            // the call being played
    voice::Monitor* activeMonitor() { return monSet ? monSet : silent.load() ? nullptr : &voice::monitor("ysf"); }
    std::function<void(const float*, size_t, int)> tap;
    std::unique_ptr<AudioOut> audioOut;
    float upPrev = 0;
    std::vector<float> pcmOut;

    FusionTelemetry pub, tel;
    bool dirty = false;

    // front end
    double rate = 0, offset = 0;
    int D = 1;
    double fd = 0;
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    Cic3 cic;   // the decimator (dsp/cic3.h: exact for as long as it runs)
    int decCount = 0;
    std::vector<cf32> dbuf;
    int64_t dN = 0;
    double rp = 0, rstep = 0;
    int M = 0, nPh = 64;
    std::vector<float> fir;
    cf32 prevZ{0, 0};
    float fh[7] = {0, 0, 0, 0, 0, 0, 0};
    int fhPos = 0;
    std::vector<float> ybuf = std::vector<float>(kYMask + 1, 0.f);
    int64_t yN = 0;
    double power = 0; int64_t nPower = 0, nIn = 0;
    double nextReport = 0;

    // frames
    struct Cand { int64_t S; float r; };
    std::vector<Cand> cands;
    bool locked = false;
    int64_t nextS = 0;
    int fails = 0;
    int polarity = 0;
    double lastFrameSec = -10;
    int8_t sync[fusion::kSyncDibits];

    // transmission state
    bool txOn = false, term = false;
    std::string chunk[13];
    bool chunkHave[13] = {};

    Impl() { fusion::syncLevels(sync); }

    float yAt(int64_t i) const { return ybuf[(size_t)(i & kYMask)]; }
    double sigSec() const { return (double)yN / kFs; }
    void emit(const std::string& s) { if (log) log(s); }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        D = std::max(1, (int)std::floor(rate / 100000.0));
        fd = rate > 0 ? rate / D : 0;
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        cic.reset();
        decCount = 0;
        dbuf.assign(8192, cf32(0, 0)); dN = 0;
        if (fd > 0) {
            rstep = fd / kFs;
            M = std::max(8, (int)std::ceil(5.5 * fd / 5000.0 / 2));
            fir.assign((size_t)nPh * 2 * M, 0.f);
            const double fc = 8000.0 / fd;   // the channel filter: windowed sinc, 8 kHz cut
            for (int ph = 0; ph < nPh; ph++) {
                const double frac = (double)ph / nPh;
                double sum = 0;
                for (int k = 0; k < 2 * M; k++) {
                    const double t = (double)(k - M + 1) - frac;
                    const double x = 2 * fc * t;
                    const double s = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                    const double w = 0.42 + 0.5 * std::cos(kPi * t / M) + 0.08 * std::cos(2 * kPi * t / M);
                    const double v = std::fabs(t) < M ? 2 * fc * s * w : 0;
                    fir[(size_t)ph * 2 * M + k] = (float)v;
                    sum += v;
                }
                for (int k = 0; k < 2 * M; k++) fir[(size_t)ph * 2 * M + k] /= (float)sum;
            }
            rp = M + 2;
        }
        prevZ = cf32(0, 0);
        for (float& f : fh) f = 0;
        fhPos = 0;
        std::fill(ybuf.begin(), ybuf.end(), 0.f); yN = 0;
        cands.clear(); locked = false; fails = 0; polarity = 0; lastFrameSec = -10;
        power = 0; nPower = 0; nIn = 0; nextReport = 0.25 * rate;
        txOn = false;
        tel = FusionTelemetry();
        tel.inputRate = rate;
        { std::lock_guard<std::mutex> lk(mu); tel.centerHz = centerHz; }
        codecChanged = true;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
        pub.seq++;
        tel.seq = pub.seq;
    }

    void input(std::complex<double> x, double inv) {
        const std::complex<double> v = x * lo;
        lo *= loStep;
        if (++loCount >= 4096) { loCount = 0; lo /= std::abs(lo); }
        cic.integrate(v);
        if (++decCount >= D) {
            decCount = 0;
            const std::complex<double> c = cic.comb();
            pushDecimated(cf32((float)(c.real() * inv), (float)(c.imag() * inv)));
        }
    }

    void pushDecimated(cf32 z) {
        dbuf[(size_t)(dN & 8191)] = z;
        dN++;
        while (dN > (int64_t)std::floor(rp) + M) {
            const int64_t base = (int64_t)std::floor(rp);
            const double frac = rp - (double)base;
            const int ph = std::min(nPh - 1, (int)std::lround(frac * nPh));
            const float* h = &fir[(size_t)ph * 2 * M];
            cf32 acc(0, 0);
            const int64_t i0 = base - M + 1;
            for (int k = 0; k < 2 * M; k++) acc += dbuf[(size_t)((i0 + k) & 8191)] * h[k];
            rp += rstep;
            demod(acc);
        }
    }

    void demod(cf32 z) {
        const cf32 d = z * std::conj(prevZ);
        prevZ = z;
        const float f = (float)(std::atan2(d.imag(), d.real()) * kFs / (2 * kPi));
        fh[fhPos] = f;
        fhPos = (fhPos + 1) % 7;
        static const float w[7] = {1, 2, 3, 4, 3, 2, 1};
        float y = 0;
        for (int k = 0; k < 7; k++) y += w[k] * fh[(fhPos + k) % 7];
        ybuf[(size_t)(yN & kYMask)] = y / 16.f;
        yN++;
        onSample();
    }

    // ------------------------------------------------------------ frames

    float syncCorr(int64_t S) const {
        float v[fusion::kSyncDibits];
        for (int i = 0; i < fusion::kSyncDibits; i++) v[i] = yAt(S + (int64_t)i * kSps);
        return pearson(v, sync, fusion::kSyncDibits);
    }

    static void lsq(const float* s, const float* v, int n, double& g, double& d) {
        double ms = 0, mv = 0;
        for (int i = 0; i < n; i++) { ms += s[i]; mv += v[i]; }
        ms /= n; mv /= n;
        double c = 0, a = 0;
        for (int i = 0; i < n; i++) { c += (s[i] - ms) * (v[i] - mv); a += (s[i] - ms) * (s[i] - ms); }
        g = a > 0 ? c / a : 0; d = mv - g * ms;
    }

    bool tryFrame(int64_t S, bool acq) {
        float v[fusion::kFrameDibits], s[fusion::kFrameDibits];
        for (int i = 0; i < fusion::kFrameDibits; i++) v[i] = yAt(S + (int64_t)i * kSps);
        for (int i = 0; i < fusion::kSyncDibits; i++) s[i] = sync[i];
        double g, d;
        lsq(s, v, fusion::kSyncDibits, g, d);
        if (std::fabs(g) < 300 || std::fabs(g) > 1800) return false;   // 900 Hz per level step nominal
        const int pol = g > 0 ? 1 : -1;
        if (polarity && pol != polarity) return false;
        for (int it = 0; it < 2; it++) {
            for (int i = fusion::kSyncDibits; i < fusion::kFrameDibits; i++) s[i] = (float)slice((float)((v[i] - d) / g));
            lsq(s, v, fusion::kFrameDibits, g, d);
            if (std::fabs(g) < 300 || std::fabs(g) > 1800) return false;
        }
        double e = 0;
        std::vector<uint8_t> bits((size_t)fusion::kFrameBits);
        for (int i = 0; i < fusion::kFrameDibits; i++) {
            const int lv = slice((float)((v[i] - d) / g));
            const double r = v[i] - g * s[i] - d;
            e += r * r;
            const int dib = fusion::levelDibit(lv);
            bits[(size_t)(2 * i)] = (uint8_t)(dib >> 1);
            bits[(size_t)(2 * i + 1)] = (uint8_t)(dib & 1);
        }
        const float snr = (float)(10 * std::log10(5 * g * g / std::max(e / fusion::kFrameDibits, 1e-9)));
        if (acq && snr < 4) return false;
        fusion::FrameContent c;
        if (!fusion::parseFrameBits(bits.data(), c)) { tel.blocksBad++; return false; }
        polarity = pol;
        tel.inverted = pol < 0;
        tel.cfoHz = d;
        tel.snrDb = snr;
        tel.blocksOk++;
        lastFrameSec = (double)S / kFs;
        onFrame(c);
        return true;
    }

    void onSample() {
        const int64_t n = yN - 1;
        if (locked) {
            if (n < nextS + (int64_t)(fusion::kFrameDibits - 1) * kSps + 4) return;
            int64_t best = nextS;
            float bq = -1;
            for (int o = -3; o <= 3; o++) {
                const float q = std::fabs(syncCorr(nextS + o));
                if (q > bq) { bq = q; best = nextS + o; }
            }
            if (bq > 0.7f && tryFrame(best, false)) fails = 0;
            else { fails++; if (bq <= 0.7f) tel.blocksBad++; }
            nextS = best + kFrame;
            if (term) { term = false; locked = false; }
            else if (fails >= 4) { locked = false; endTx(); }
            return;
        }
        const int64_t S = n - (int64_t)(fusion::kSyncDibits - 1) * kSps;
        if (S >= 0) {
            const float r = syncCorr(S);
            if (std::fabs(r) >= 0.9f) {
                if (!cands.empty() && S - cands.back().S <= 5) { if (std::fabs(r) > std::fabs(cands.back().r)) cands.back() = {S, r}; }
                else { cands.push_back({S, r}); if (cands.size() > 48) cands.erase(cands.begin()); }
            }
        }
        while (!cands.empty() && n >= cands.front().S + (int64_t)(fusion::kFrameDibits - 1) * kSps + 2) {
            const Cand c = cands.front();
            cands.erase(cands.begin());
            if (!tryFrame(c.S, true)) continue;
            cands.clear();
            locked = !term; term = false; fails = 0;
            nextS = c.S + kFrame;
            break;
        }
    }

    // ------------------------------------------------------------ transmission

    void beginTx() {
        txOn = true;
        tel.dest.clear(); tel.src.clear(); tel.down.clear(); tel.up.clear();
        for (auto& r : tel.rem) r.clear();
        tel.slowText.clear();
        for (bool& h : tel.haveCsd) h = false;
        for (int i = 0; i < 13; i++) { chunk[i].clear(); chunkHave[i] = false; }
        tel.voiceSubFrames = 0; tel.voiceDecoded = 0;
        tel.dchOk = 0; tel.dchBad = 0;
        emit("Yaesu Fusion: transmission started");
    }
    void endTx() {
        if (txOn) emit("Yaesu Fusion: transmission ended" + (tel.src.empty() ? std::string() : " (" + tel.src + ")"));
        txOn = false;
        report();
    }

    void csd1(const uint8_t* p) { tel.dest = fusion::trimField(p, 10); tel.src = fusion::trimField(p + 10, 10); tel.haveCsd[0] = true; }
    void csd2(const uint8_t* p) { tel.down = fusion::trimField(p, 10); tel.up = fusion::trimField(p + 10, 10); tel.haveCsd[1] = true; }
    void csd3(const uint8_t* p) { for (int i = 0; i < 4; i++) tel.rem[i] = remText(p + 5 * i, 5); tel.haveCsd[2] = true; }
    void slow(int idx, const uint8_t* p, size_t n) {
        if (idx < 0 || idx >= 13) return;
        std::string c;
        for (size_t i = 0; i < n; i++) c += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : ' ';
        chunk[idx] = c;
        chunkHave[idx] = true;
        std::string t;
        for (int i = 0; i < 13; i++) if (chunkHave[i]) t += chunk[i];
        while (!t.empty() && t.back() == ' ') t.pop_back();
        tel.slowText = t;
    }

    void onFrame(const fusion::FrameContent& c) {
        const fusion::Fich& f = c.fich;
        tel.frameType = f.fi; tel.dataType = f.dt; tel.callMode = f.cm; tel.routing = f.mr; tel.voip = f.voip;
        tel.squelch = f.sqlOff ? 0 : f.sqlCode;
        tel.fn = f.fn; tel.ft = f.ft; tel.bn = f.bn; tel.bt = f.bt;
        if (f.fi == 3) { tel.tests++; dirty = true; return; }
        if (f.fi == 0) { if (txOn) endTx(); beginTx(); tel.headers++; }
        else if (f.fi == 1) { if (!txOn) beginTx(); tel.comms++; }
        else { tel.terminators++; }
        for (size_t i = 0; i < c.units.size(); i++) {
            if (c.unitOk[i]) tel.dchOk++; else { tel.dchBad++; continue; }
            const uint8_t* p = c.units[i].data();
            if (f.fi == 0 || f.fi == 2) { if (i == 0) csd1(p); else csd2(p); continue; }
            if (f.dt == 0) {   // V/D mode 1: FN 0..2 callsign data 1..3, then slow data of 20 bytes
                if (f.fn == 0) csd1(p); else if (f.fn == 1) csd2(p); else if (f.fn == 2) csd3(p); else slow(f.fn - 3, p, 20);
            } else if (f.dt == 2) {   // V/D mode 2: FN 0..5 in 10 byte fields, then slow data of 10 bytes
                switch (f.fn) {
                case 0: tel.dest = fusion::trimField(p, 10); break;
                case 1: tel.src = fusion::trimField(p, 10); tel.haveCsd[0] = true; break;
                case 2: tel.down = fusion::trimField(p, 10); break;
                case 3: tel.up = fusion::trimField(p, 10); tel.haveCsd[1] = true; break;
                case 4: tel.rem[0] = remText(p, 5); tel.rem[1] = remText(p + 5, 5); break;
                case 5: tel.rem[2] = remText(p, 5); tel.rem[3] = remText(p + 5, 5); tel.haveCsd[2] = true; break;
                default: slow(f.fn - 6, p, 10); break;
                }
            } else if (f.dt == 1) {   // data FR: two units a frame, CSD1, CSD2, CSD3, then data
                const int u = 2 * f.fn + (int)i;
                if (u == 0) csd1(p); else if (u == 1) csd2(p); else if (u == 2) csd3(p); else slow(u - 3, p, 20);
            } else if (f.dt == 3 && f.fn == 0 && f.ft == 1) csd3(p);
        }
        for (const auto& v : c.voice) voice(v);
        if (f.fi == 2) { endTx(); term = true; }
        dirty = true;
        report();
    }

    // ------------------------------------------------------------ voice

    void voice(const fusion::VoiceUnit& v) {
        tel.voiceSubFrames++;
        voice::Monitor* mon = activeMonitor();
        if (mon) {   // the voice monitor (voice_monitor.h) lists the call and says whether it is played
            voice::CallInfo ci;
            ci.srcText = tel.src; ci.dstText = tel.dest; ci.group = tel.callMode != 3;
            { std::lock_guard<std::mutex> lk(codecMu); ci.codecMissing = !codec; }
            if (tel.squelch >= 0 && tel.squelch < 127) ci.access = tel.squelch ? "SQL " + std::to_string(tel.squelch) : std::string("SQL off");
            if (!tel.down.empty()) ci.access += (ci.access.empty() ? "" : ", ") + std::string("via ") + tel.down;
            if (mon->offer(ci) != voice::Verdict::Play) return;
            playing = ci;
        }
        std::vector<int16_t> pcm;
        int sr = 8000;
        {
            std::lock_guard<std::mutex> lk(codecMu);
            if (!codec) return;
            pcm = codec->decode(v.bytes);
            sr = codec->sampleRate();
        }
        if (pcm.empty()) return;
        tel.voiceDecoded++;
        std::vector<float> f(pcm.size());
        for (size_t i = 0; i < pcm.size(); i++) f[i] = pcm[i] / 32768.f;
        if (tap) tap(f.data(), f.size(), sr);
        if (mon) mon->audio(playing, f.data(), f.size(), 1, sr);   // the call's recording (when on)
        if (silent.load() || sr <= 0 || !mon) return;
        const int k = std::max(1, (int)std::lround(48000.0 / sr));
        pcmOut.clear();
        for (float s : f) {
            for (int j = 1; j <= k; j++) { const float x = upPrev + (s - upPrev) * j / (float)k; pcmOut.push_back(x); pcmOut.push_back(x); }
            upPrev = s;
        }
        voice::ensureOutput(audioOut, *mon);   // on the mode's output device
        audioOut->setVolume(volume.load() * mon->volume()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        audioOut->write(pcmOut.data(), (int)(pcmOut.size() / 2));
    }

    void report() {
        tel.seq++;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : tel.levelDb;
        power = 0; nPower = 0;
        if (txOn && !locked && sigSec() - lastFrameSec > 0.5) { txOn = false; }
        tel.dataValid = txOn;
        tel.state = txOn ? 2 : (nIn > 0 ? 1 : 0);
        {
            std::lock_guard<std::mutex> lk(codecMu);
            tel.codecReady = codec != nullptr;
            tel.codecText = codec ? std::string() : codecText;
        }
        std::lock_guard<std::mutex> lk(mu);
        tel.centerHz = centerHz;
        pub = tel;
    }
};

FusionReceiver::FusionReceiver() : p_(std::make_unique<Impl>()) {}
FusionReceiver::~FusionReceiver() = default;

void FusionReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void FusionReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void FusionReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centerHz = hz; }
bool FusionReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= fusionTuning().minSampleRate - 1; }
void FusionReceiver::reset() { p_->resetReq = true; }

void FusionReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.rate <= 0 || n == 0) return;
    const double inv = 1.0 / ((double)m.D * m.D * m.D);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.nPower++;
        m.input(std::complex<double>(re, im), inv);
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

bool FusionReceiver::telemetry(FusionTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void FusionReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void FusionReceiver::setSpeechCodec(std::unique_ptr<codec::SpeechCodec> c, const std::string& text) {
    std::lock_guard<std::mutex> lk(p_->codecMu);
    p_->codec = std::move(c);
    p_->codecText = text.empty() ? std::string("codec not installed") : text;
}
void FusionReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void FusionReceiver::setMuted(bool m) { p_->muted = m; }
void FusionReceiver::setSilent(bool s) { p_->silent = s; }
void FusionReceiver::setVoiceMonitor(voice::Monitor* m) { p_->monSet = m; }
void FusionReceiver::setAudioTap(std::function<void(const float*, size_t, int)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tap = std::move(cb); }

ModeTuning fusionTuning() {
    ModeTuning t;
    t.stdMode = 130;
    t.id = "fusion";
    t.name = "Yaesu Fusion (WIP)";
    t.minMhz = 28; t.maxMhz = 1300; t.defMhz = 145.000;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.0125;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;     // the radio sits 50 kHz above the channel, away from its DC spike
    return t;
}

std::string fusionSummary(const FusionTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Fusion: waiting for samples";
    if (!t.dataValid && t.blocksOk == 0) return "Fusion: listening";
    if (t.src.empty()) {
        snprintf(b, sizeof b, "Fusion: %llu frames", (unsigned long long)t.blocksOk);
        return b;
    }
    snprintf(b, sizeof b, "Fusion: %s to %s, %llu frames", t.src.c_str(), t.dest.empty() ? "-" : t.dest.c_str(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
