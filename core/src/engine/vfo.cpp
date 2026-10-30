// Multi-VFO: see vfo.h.
#include "dect2/engine/vfo.h"
#include "dect2/engine/channelizer.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

namespace dect2 {

const char* vfoModeName(VfoMode m) {
    static const char* const n[] = {"NFM", "AM", "USB", "LSB", "WFM", "Pager", "APRS"};
    return (int)m >= 0 && (int)m < (int)VfoMode::Count ? n[(int)m] : "NFM";
}

double vfoDefaultBandwidthHz(VfoMode m) {
    switch (m) {
    case VfoMode::Nfm: return 12500;
    case VfoMode::Am: return 9000;
    case VfoMode::Usb: case VfoMode::Lsb: return 2700;
    case VfoMode::Wfm: return 220000;
    case VfoMode::Pager: case VfoMode::Packet: return 25000;
    default: return 12500;
    }
}

namespace {

constexpr double kDecoderRate = 192000.0;   // what the pager and packet receivers get in wide input (as the whole-span banks)
constexpr double kWfmRate = 500000.0;

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Blackman-windowed sinc low pass, cutoff in Hz, taps odd
std::vector<float> lowPass(double rate, double cutHz, int taps) {
    std::vector<float> h((size_t)taps);
    const double fc = std::min(0.499, cutHz / rate);
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double k = i - (taps - 1) / 2.0;
        const double s = k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(s * w); sum += s * w;
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}

// Complex FIR with real taps, streaming
class Fir {
public:
    void set(std::vector<float> t) { h_ = std::move(t); hist_.assign(h_.size(), cf32(0, 0)); pos_ = 0; }
    cf32 step(cf32 x) {
        hist_[pos_] = x;
        float re = 0, im = 0;
        size_t j = pos_;
        const size_t n = h_.size();
        for (size_t k = 0; k < n; k++) { re += h_[k] * hist_[j].real(); im += h_[k] * hist_[j].imag(); j = j == 0 ? n - 1 : j - 1; }
        pos_ = pos_ + 1 == n ? 0 : pos_ + 1;
        return cf32(re, im);
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), cf32(0, 0)); pos_ = 0; }
private:
    std::vector<float> h_;
    std::vector<cf32> hist_;
    size_t pos_ = 0;
};

} // namespace

struct Chan {
    VfoConfig cfg;
    double spanRate = 0;
    // live settings
    std::atomic<float> volume{1.0f}, pan{0};
    std::atomic<bool> mute{false}, enabled{true};
    std::atomic<double> squelchDb{-150};
    // status
    std::atomic<float> levelDb{-200};
    std::atomic<bool> open{false};
    std::string warn;
    // chain
    ChannelFilter cf;
    double chRate = 0;
    std::vector<cf32> base;
    // audio demodulator
    Fir bandFir;
    double fcHz = 0;       // SSB: the passband centre
    double ph = 0, phStep = 0;
    cf32 prev{1, 0};
    float dc = 0, lp = 0, agc = 0.1f;
    ExactResampler outRs;  // chRate -> 48 kHz when they differ
    std::vector<cf32> rsIn, rsOut;
    std::deque<float> fifo;   // mono 48 kHz audio waiting to be mixed
    int hang = 0;
    // WFM
    std::unique_ptr<FmReceiver> fm;
    std::vector<float> fmL, fmR;
    std::mutex fmMu;
    // decoders
    std::unique_ptr<PagerReceiver> pager;
    std::unique_ptr<PacketReceiver> packet;

    bool audioKind() const { return !vfoIsDecoder(cfg.mode); }

    bool build(double span, const VfoConfig& c) {
        cfg = c; spanRate = span;
        volume = c.volume; pan = c.pan; mute = c.mute; enabled = c.enabled; squelchDb = c.squelchDb;
        const double bw = c.bandwidthHz > 0 ? c.bandwidthHz : vfoDefaultBandwidthHz(c.mode);
        warn.clear();
        if (vfoIsDecoder(c.mode)) chRate = kDecoderRate;
        else if (c.mode == VfoMode::Wfm) chRate = std::max(kWfmRate, std::ceil(bw * 2.2 / 1e5) * 1e5);
        else chRate = std::max((double)VfoBank::kAudioRate, std::ceil(bw * 2.4 / VfoBank::kAudioRate) * VfoBank::kAudioRate);
        const double need = c.mode == VfoMode::Wfm ? 500e3 : vfoIsDecoder(c.mode) ? 200e3 : 0;
        if (span < need) warn = std::string(vfoModeName(c.mode)) + " needs a capture of at least " + std::to_string((int)(need / 1000)) + " kHz";
        if (std::fabs(c.offsetHz) + bw / 2 > span / 2) warn = "the channel reaches outside the capture";
        if (!cf.configure(span, c.offsetHz, chRate)) { warn = "capture rate too low"; return false; }
        ph = 0; prev = cf32(1, 0); dc = 0; lp = 0; agc = 0.1f; hang = 0; fifo.clear();
        if (c.mode == VfoMode::Wfm) {
            fm.reset(new FmReceiver());
            fm->setSilent(true);
            fm->setStationFinder(false);
            fm->setChannelWidth(bw);
            fm->configure(chRate);
            fm->setAudioTap([this](const float* l, const float* r, size_t n) {
                std::lock_guard<std::mutex> lk(fmMu);
                fmL.insert(fmL.end(), l, l + n); fmR.insert(fmR.end(), r, r + n);
            });
        } else if (c.mode == VfoMode::Pager) {
            pager.reset(new PagerReceiver());
            pager->setWideInput(true);
            pager->configure(chRate);
        } else if (c.mode == VfoMode::Packet) {
            packet.reset(new PacketReceiver());
            packet->setWideInput(true);
            packet->setSilent(true);
            packet->configure(chRate);
        } else {
            double cut = bw / 2;
            fcHz = 0;
            if (c.mode == VfoMode::Usb || c.mode == VfoMode::Lsb) {
                const double lo = 300, hi = 300 + bw;
                fcHz = (lo + hi) / 2 * (c.mode == VfoMode::Usb ? 1 : -1);
                cut = (hi - lo) / 2;
            }
            phStep = -2 * M_PI * fcHz / chRate;
            const int taps = std::min(1025, std::max(65, (int)(4.0 * chRate / std::max(200.0, cut)) | 1));
            bandFir.set(lowPass(chRate, cut, taps));
            outRs.configure(chRate, VfoBank::kAudioRate, 8);
        }
        return true;
    }

    void resetState() {
        cf.reset(); bandFir.reset(); outRs.reset(); fifo.clear(); prev = cf32(1, 0); dc = 0; lp = 0; agc = 0.1f; hang = 0;
        if (fm) fm->reset();
        if (pager) pager->reset();
        if (packet) packet->reset();
    }

    // returns the channel power (linear, mean |x|^2) of the block
    void process(const cf32* x, size_t n) {
        base.clear();
        cf.process(x, n, base);
        if (base.empty()) return;
        double pw = 0;
        for (const cf32& v : base) pw += (double)v.real() * v.real() + (double)v.imag() * v.imag();
        pw /= (double)base.size();
        const float db = pw > 1e-20 ? (float)(10 * std::log10(pw)) : -200.f;
        const float prevDb = levelDb.load();
        levelDb = prevDb < -190 ? db : prevDb * 0.7f + db * 0.3f;
        if (pager) { pager->feed(base.data(), base.size()); open = levelDb.load() > -60; return; }
        if (packet) { packet->feed(base.data(), base.size()); open = levelDb.load() > -60; return; }
        const double sq = squelchDb.load();
        // squelch with 2 dB hysteresis and a short hang so syllables are not clipped
        const float lv = levelDb.load();
        if (sq <= -150 || lv >= sq + (open ? -2.0 : 0.0)) { open = true; hang = (int)(0.25 * chRate); }
        else if (hang > 0) hang -= (int)base.size(), open = true;
        else open = false;
        if (hang < 0) hang = 0;
        if (fm) {
            fm->feed(base.data(), base.size());
            std::lock_guard<std::mutex> lk(fmMu);
            for (size_t i = 0; i < fmL.size(); i++) fifo.push_back(0.5f * (fmL[i] + fmR[i]));
            fmL.clear(); fmR.clear();
            return;
        }
        rsIn.resize(base.size());
        const bool ssb = cfg.mode == VfoMode::Usb || cfg.mode == VfoMode::Lsb;
        for (size_t i = 0; i < base.size(); i++) {
            cf32 v = base[i];
            if (ssb) { const cf32 r((float)std::cos(ph), (float)std::sin(ph)); v *= r; ph += phStep; if (ph < -M_PI) ph += 2 * M_PI; }
            v = bandFir.step(v);
            float a = 0;
            if (cfg.mode == VfoMode::Nfm) {
                const cf32 d = v * std::conj(prev); prev = v;
                a = std::atan2(d.imag(), d.real()) * (float)(chRate / (2 * M_PI * 5000.0)) * 0.7f;
                lp += 0.35f * (a - lp); a = lp;
            } else if (cfg.mode == VfoMode::Am) {
                const float m = std::abs(v);
                dc += 0.0005f * (m - dc);
                agc += 0.0002f * (std::fabs(m - dc) - agc);
                a = (m - dc) / std::max(0.01f, agc * 3.f) * 0.5f;
            } else {   // SSB: shift back and take the real part
                const cf32 r((float)std::cos(ph), (float)-std::sin(ph));
                a = (v * r).real();
                agc += (std::fabs(a) > agc ? 0.01f : 0.00005f) * (std::fabs(a) - agc);
                a = a / std::max(0.003f, agc * 3.f) * 0.5f;
            }
            rsIn[i] = cf32(std::max(-1.f, std::min(1.f, a)), 0);
        }
        if (outRs.passthrough()) for (const cf32& v : rsIn) fifo.push_back(v.real());
        else { rsOut.clear(); outRs.process(rsIn.data(), rsIn.size(), rsOut); for (const cf32& v : rsOut) fifo.push_back(v.real()); }
    }
};

struct VfoBank::Impl {
    mutable std::mutex mu;                      // chans, sink
    double rate = 0;
    std::vector<std::shared_ptr<Chan>> chans;
    std::vector<VfoConfig> want;
    Sink sink;
    std::vector<float> mix;
    // load: work seconds vs data seconds over a window
    double workSec = 0, dataSec = 0;
    std::atomic<double> load{0};
};

VfoBank::VfoBank() : p_(new Impl) {}
VfoBank::~VfoBank() = default;

static bool sameShape(const VfoConfig& a, const VfoConfig& b) {
    return a.id == b.id && a.mode == b.mode && a.offsetHz == b.offsetHz && a.bandwidthHz == b.bandwidthHz;
}

void VfoBank::configure(double spanRate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rate == spanRate && !p_->chans.empty()) return;   // same capture: keep the channels running
    p_->rate = spanRate;
    p_->chans.clear();
    for (const VfoConfig& c : p_->want) {
        auto ch = std::make_shared<Chan>();
        ch->build(spanRate, c);
        p_->chans.push_back(ch);
    }
}

double VfoBank::spanRate() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate; }

void VfoBank::setChannels(const std::vector<VfoConfig>& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<std::shared_ptr<Chan>> out;
    for (const VfoConfig& n : c) {
        std::shared_ptr<Chan> keep;
        for (auto& o : p_->chans) if (sameShape(o->cfg, n)) { keep = o; break; }
        if (keep) {
            keep->cfg = n;
            keep->volume = n.volume; keep->pan = n.pan; keep->mute = n.mute; keep->enabled = n.enabled; keep->squelchDb = n.squelchDb;
            out.push_back(keep);
        } else {
            auto ch = std::make_shared<Chan>();
            if (p_->rate > 0) ch->build(p_->rate, n); else { ch->cfg = n; }
            out.push_back(ch);
        }
    }
    p_->chans.swap(out);
    p_->want = c;
}

size_t VfoBank::channelCount() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->chans.size(); }
void VfoBank::setSink(Sink s) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = std::move(s); }

void VfoBank::feed(const cf32* x, size_t n) {
    std::vector<std::shared_ptr<Chan>> chans; Sink sink; double rate;
    { std::lock_guard<std::mutex> lk(p_->mu); chans = p_->chans; sink = p_->sink; rate = p_->rate; }
    if (chans.empty() || rate <= 0 || !n) return;
    const double t0 = nowSec();
    bool anyAudio = false;
    for (auto& c : chans) {
        if (c->chRate <= 0 || !c->enabled.load()) continue;
        c->process(x, n);
        anyAudio = anyAudio || c->audioKind();
    }
    if (anyAudio && sink) {
        size_t frames = SIZE_MAX;
        for (auto& c : chans) if (c->audioKind() && c->chRate > 0 && c->enabled.load()) frames = std::min(frames, c->fifo.size());
        if (frames != SIZE_MAX && frames > 0) {
            std::vector<float>& mix = p_->mix;
            mix.assign(frames * 2, 0.f);
            for (auto& c : chans) {
                if (!c->audioKind() || c->chRate <= 0 || !c->enabled.load()) continue;
                const bool play = c->open.load() && !c->mute.load();
                const float v = c->volume.load(), pan = std::max(-1.f, std::min(1.f, c->pan.load()));
                const float gl = v * std::sqrt(0.5f * (1 - pan)) * 1.414f, gr = v * std::sqrt(0.5f * (1 + pan)) * 1.414f;
                for (size_t i = 0; i < frames; i++) {
                    const float s = c->fifo.front(); c->fifo.pop_front();
                    if (play) { mix[2 * i] += s * gl * 0.5f; mix[2 * i + 1] += s * gr * 0.5f; }
                }
            }
            for (float& v : mix) v = std::max(-1.f, std::min(1.f, v));
            sink(mix.data(), frames);
        }
    }
    const double w = nowSec() - t0;
    p_->workSec += w; p_->dataSec += (double)n / rate;
    if (p_->dataSec >= 1.0) { p_->load = p_->workSec / p_->dataSec; p_->workSec = p_->dataSec = 0; }
}

void VfoBank::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& c : p_->chans) c->resetState();
}

std::vector<VfoStatus> VfoBank::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<VfoStatus> v;
    for (auto& c : p_->chans) {
        VfoStatus s; s.id = c->cfg.id; s.levelDb = c->levelDb.load(); s.open = c->open.load();
        if (c->pager) { PagerTelemetry t; if (c->pager->telemetry(t, 0)) { s.items = t.messagesTotal; s.info = pagerSummary(t); } }
        if (c->packet) { PacketTelemetry t; if (c->packet->telemetry(t, 0)) { s.items = t.blocksOk; s.info = "APRS: " + std::to_string((unsigned long long)t.blocksOk) + " frames"; } }
        v.push_back(s);
    }
    return v;
}

bool VfoBank::pagerTelemetry(int id, PagerTelemetry& t) const {
    std::shared_ptr<Chan> c;
    { std::lock_guard<std::mutex> lk(p_->mu); for (auto& k : p_->chans) if (k->cfg.id == id) c = k; }
    return c && c->pager && c->pager->telemetry(t, 0);
}

bool VfoBank::packetTelemetry(int id, PacketTelemetry& t) const {
    std::shared_ptr<Chan> c;
    { std::lock_guard<std::mutex> lk(p_->mu); for (auto& k : p_->chans) if (k->cfg.id == id) c = k; }
    return c && c->packet && c->packet->telemetry(t, 0);
}

void VfoBank::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& c : p_->chans) { if (c->pager) c->pager->clearResults(); if (c->packet) c->packet->clearResults(); }
}

double VfoBank::load() const { return p_->load.load(); }

std::string VfoBank::warning(int id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& c : p_->chans) if (c->cfg.id == id) return c->warn;
    return "";
}

} // namespace dect2
