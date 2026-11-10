// HF utility receiver: channel to 24 kHz (marine::Front), then the decoder of this mode. See hfutil_rx.h.
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfu_serial.h"
#include "dect2/hfutil/hfu_hdr.h"
#include "dect2/hfutil/hfu_fsk.h"
#include "dect2/marine/marine_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>
#include <vector>

namespace dect2 {

namespace {
constexpr double kMinRate = 250000;
constexpr size_t kTextMax = 6000, kEvMax = 200;
}

struct HfUtilReceiver::Impl {
    int kind;
    mutable std::mutex mu;
    double rate = 0, offsetHz = 0;
    marine::Front front;
    std::vector<cf32> base;
    std::unique_ptr<hfu::SerialRx> serial;
    std::unique_ptr<hfu::FskRx> fsk;
    std::unique_ptr<hfu::HighRateRx> hdr;
    HfUtilTelemetry tel;
    uint64_t seq = 0;
    std::function<void(const std::string&)> log;
    uint8_t curByte = 0;
    int curBits = 0;
    std::string hexLine;
    int inByte = 0;

    explicit Impl(int k) : kind(k) { build(); }

    void addText(const std::string& s) {
        std::lock_guard<std::mutex> l(mu);
        tel.text += s;
        if (tel.text.size() > kTextMax) tel.text.erase(0, tel.text.size() - kTextMax);
        tel.seq = ++seq;
    }
    void addEvent(const std::string& s) {
        HfUtilEvent e;
        e.utc = (int64_t)std::time(nullptr);
        e.text = s;
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> l(mu);
            tel.events.insert(tel.events.begin(), e);
            if (tel.events.size() > kEvMax) tel.events.pop_back();
            tel.seq = ++seq;
            cb = log;
        }
        if (cb) cb(std::string(hfuName(kind)) + ": " + s);
    }

    void build() {
        if (kind == 0 || kind == 1) {
            serial = std::make_unique<hfu::SerialRx>(kind == 0 ? hfu::SerialStd::Mil110 : hfu::SerialStd::Stanag4285);
            serial->setEventSink([this](const hfu::SerialEvent& e) { addEvent(e.text); });
            serial->setBitSink([this](uint8_t b) {
                // the decoded bits as bytes, least significant bit first: printable text, the rest as hex
                curByte = (uint8_t)(curByte | (b << curBits));
                if (++curBits == 8) {
                    const uint8_t c = curByte;
                    curBits = 0; curByte = 0;
                    char t[6];
                    if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r') snprintf(t, sizeof t, "%c", c);
                    else snprintf(t, sizeof t, "[%02X]", c);
                    addText(t);
                }
            });
            serial->setD5066Sink([this](const hfu::D5066& d) {
                char t[200];
                snprintf(t, sizeof t, "STANAG 5066 %s  %s -> %s  %d bytes%s%s", d.typeName.c_str(), d.src.c_str(), d.dst.c_str(), d.payload, d.payload ? (d.dataCrcOk ? ", CRC ok" : ", CRC bad") : "", d.hex.empty() ? "" : "  (payload not shown)");
                addEvent(t);
                std::lock_guard<std::mutex> l(mu);
                tel.pdus++;
            });
        } else if (kind == kHfuKindHdr) {
            hdr = std::make_unique<hfu::HighRateRx>();
            hdr->setEventSink([this](const hfu::SerialEvent& e) { addEvent(e.text); });
            hdr->setByteSink([this](uint8_t c) {
                char t[6];
                if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r') snprintf(t, sizeof t, "%c", c);
                else snprintf(t, sizeof t, "[%02X]", c);
                addText(t);
            });
            hdr->setD5066Sink([this](const hfu::D5066& d) {
                char t[200];
                snprintf(t, sizeof t, "STANAG 5066 %s  %s -> %s  %d bytes%s", d.typeName.c_str(), d.src.c_str(), d.dst.c_str(), d.payload, d.payload ? (d.dataCrcOk ? ", CRC ok" : ", CRC bad") : "");
                addEvent(t);
                std::lock_guard<std::mutex> l(mu);
                tel.pdus++;
            });
        } else {
            static const hfu::FskKind kk[] = {hfu::FskKind::ArqE, hfu::FskKind::ArqN, hfu::FskKind::Autospec, hfu::FskKind::Spread, hfu::FskKind::GTor, hfu::FskKind::Efr, hfu::FskKind::Piccolo6, hfu::FskKind::Coquelet8};
            fsk = std::make_unique<hfu::FskRx>(kk[kind - 2]);
            fsk->setTextSink([this](const std::string& s) { addText(s); });
            fsk->setEventSink([this](const std::string& s) { addEvent(s); });
        }
    }

    void resetDec() {
        if (serial) serial->reset();
        if (fsk) fsk->reset();
        if (hdr) hdr->reset();
        curByte = 0; curBits = 0;
        std::lock_guard<std::mutex> l(mu);
        tel = HfUtilTelemetry();
        tel.kind = kind;
        tel.seq = ++seq;
    }
};

HfUtilReceiver::HfUtilReceiver(int kind) : p_(new Impl(kind)) { p_->tel.kind = kind; }
HfUtilReceiver::~HfUtilReceiver() = default;

void HfUtilReceiver::configure(double inputRateHz) {
    p_->rate = inputRateHz;
    p_->front.configure(inputRateHz);
    p_->front.setOffsetHz(p_->offsetHz);
    p_->front.reset();
    p_->resetDec();
}

void HfUtilReceiver::setSignalOffset(double hz) { p_->offsetHz = hz; if (p_->rate > 0) p_->front.setOffsetHz(hz); }
bool HfUtilReceiver::ready() const { return p_->rate >= kMinRate - 1; }
void HfUtilReceiver::reset() { p_->front.reset(); p_->resetDec(); }
void HfUtilReceiver::clearResults() { p_->resetDec(); }

void HfUtilReceiver::feed(const cf32* x, size_t n) {
    if (p_->rate < kMinRate - 1) return;
    p_->base.clear();
    p_->front.process(x, n, p_->base);
    if (p_->base.empty()) return;
    if (p_->serial) p_->serial->push(p_->base.data(), p_->base.size());
    if (p_->fsk) p_->fsk->push(p_->base.data(), p_->base.size());
    if (p_->hdr) p_->hdr->push(p_->base.data(), p_->base.size());
    double e = 0;
    for (const auto& v : p_->base) e += std::norm(v);
    std::lock_guard<std::mutex> l(p_->mu);
    p_->tel.levelDb = (float)(10 * std::log10(e / (double)p_->base.size() + 1e-20));
    if (p_->serial) {
        const auto s = p_->serial->status();
        p_->tel.state = s.state; p_->tel.cfoHz = s.cfoHz; p_->tel.snrDb = (float)s.snrDb; p_->tel.mode = s.mode;
        p_->tel.blocksOk = s.blocks; p_->tel.dataValid = s.state >= 2;
    } else if (p_->hdr) {
        const auto s = p_->hdr->status();
        p_->tel.state = s.state; p_->tel.cfoHz = s.cfoHz; p_->tel.snrDb = (float)s.snrDb; p_->tel.mode = s.state >= 2 || s.messages ? s.mode : "";
        p_->tel.blocksOk = s.blocks; p_->tel.dataValid = s.state >= 2;
    } else if (p_->fsk) {
        const auto s = p_->fsk->status();
        p_->tel.state = s.state; p_->tel.baud = s.baud; p_->tel.centreHz = s.centreHz; p_->tel.shiftHz = s.shiftHz;
        char m[120];
        if (s.baud > 0 && s.shiftHz > 0) snprintf(m, sizeof m, "%.1f Bd, %.0f Hz shift%s%s", s.baud, s.shiftHz, s.detail.empty() ? "" : ", ", s.detail.c_str());
        else snprintf(m, sizeof m, "%s", s.detail.c_str());
        p_->tel.mode = m;
        p_->tel.blocksOk = s.good; p_->tel.blocksBad = s.bad; p_->tel.dataValid = s.state >= 2;
    }
    p_->tel.seq = ++p_->seq;
}

bool HfUtilReceiver::telemetry(HfUtilTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}

void HfUtilReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->log = std::move(cb); }
void HfUtilReceiver::setForcedProfile(int p) { if (p_->serial) p_->serial->forceProfile(p); }
void HfUtilReceiver::setForcedFsk(double baud, double shiftHz) { if (p_->fsk) { p_->fsk->setBaud(baud); p_->fsk->setShift(shiftHz); } }

ModeTuning hfUtilTuning(int kind) {
    static const char* ids[kHfuKindsAll] = {"milstd110", "stanag4285", "arqe", "arqn", "autospec", "spreadhf", "gtor", "efr", "piccolo", "coquelet", "stanag4539"};
    static const double def[kHfuKindsAll] = {7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 14.0, 0.1276, 7.0, 7.0, 7.0};
    kind = std::clamp(kind, 0, kHfuKindsAll - 1);
    ModeTuning t;
    t.stdMode = hfuFamilyOf(kind) + 2;
    t.id = ids[kind];
    t.name = hfuName(kind);
    t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = def[kind];
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = kMinRate;
    t.tuneOffsetHz = 20000;
    return t;
}

const HfuPreset* hfuPresets(int kind, int& count) {
    static const HfuPreset efr[] = {{"DCF49", 0.1276}, {"DCF39", 0.1375}};
    static const HfuPreset hf[] = {{"80 m", 3.6}, {"40 m", 7.0}, {"30 m", 10.14}, {"20 m", 14.08}, {"17 m", 18.1}, {"15 m", 21.09}};
    if (kind == 7) { count = 2; return efr; }
    count = 6;
    return hf;
}

namespace {
class HfuSynth : public ModeSynth {
public:
    HfuSynth(int kind, const SynthConfig& s, double rate) : kind_(kind), rate_(rate), cfo_(s.cfoHz), snr_(s.snrDb), rng_(1234) {
        std::vector<cf32> x;
        const std::string msg = "ONAIR HF UTILITY TEST SIGNAL 12345 THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
        if (kind == kHfuKindHdr) {
            hfu::HdrGenCfg c;
            c.bps = 4800; c.frames = 3;
            x = hfu::genHighRate(c, hfu::hdrBytesToBits(msg));
        } else if (kind <= 1) {
            hfu::SerialGenCfg c;
            c.std = kind == 0 ? hfu::SerialStd::Mil110 : hfu::SerialStd::Stanag4285;
            c.bps = 600;
            std::vector<uint8_t> bits;
            for (char ch : msg) for (int i = 0; i < 8; i++) bits.push_back((uint8_t)((ch >> i) & 1));
            x = hfu::genSerial(c, bits);
        } else {
            static const hfu::FskKind kk[] = {hfu::FskKind::ArqE, hfu::FskKind::ArqN, hfu::FskKind::Autospec, hfu::FskKind::Spread, hfu::FskKind::GTor, hfu::FskKind::Efr, hfu::FskKind::Piccolo6, hfu::FskKind::Coquelet8};
            hfu::FskGenCfg c;
            c.kind = kk[kind - 2];
            c.baud = kind == 4 || kind == 5 ? 68.5 : 96;
            c.shift = 400;
            x = hfu::genFsk(c, msg);
        }
        double p = 0;
        for (auto& v : x) p += std::norm(v);
        p /= (double)std::max<size_t>(1, x.size());
        // the signal sits tuneOffsetHz below the radio
        sig_.resize(x.size());
        const float g = (float)(0.2 / std::sqrt(std::max(p, 1e-12)));
        for (size_t i = 0; i < x.size(); i++) sig_[i] = x[i] * g;
        step_ = hfu::kBaseRate / rate_;
        noise_ = (float)(snr_ < 90 ? 0.2 / std::pow(10.0, snr_ / 20) * std::sqrt(rate_ / 3000.0 / 2) : 0);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> d(0, 1);
        for (size_t i = 0; i < n; i++) {
            const size_t k = (size_t)pos_ % sig_.size(), k2 = (k + 1) % sig_.size();
            const float f = (float)(pos_ - std::floor(pos_));
            cf32 v = sig_[k] * (1 - f) + sig_[k2] * f;
            const double a = 2 * M_PI * (cfo_ - 20000.0) * t_ / rate_;
            v *= cf32((float)std::cos(a), (float)std::sin(a));
            if (noise_ > 0) v += cf32(noise_ * d(rng_), noise_ * d(rng_));
            out[i] = v;
            pos_ += step_;
            if (pos_ >= (double)sig_.size()) pos_ -= (double)sig_.size();
            t_ += 1;
            if (t_ > 1e15) t_ = 0;
        }
    }
private:
    int kind_;
    double rate_, cfo_, snr_;
    std::mt19937 rng_;
    std::vector<cf32> sig_;
    double pos_ = 0, step_ = 1;
    double t_ = 0;
    float noise_ = 0;
};
}  // namespace

std::unique_ptr<ModeSynth> makeHfUtilSynth(int kind, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0 || kind < 0 || kind >= kHfuKindsAll) return nullptr;
    return std::make_unique<HfuSynth>(kind, cfg, sampleRate);
}

}  // namespace dect2
