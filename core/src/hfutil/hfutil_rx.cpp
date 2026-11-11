// HF utility receiver: channel to 24 kHz (marine::Front), then the decoder of this mode. See hfutil_rx.h.
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfu_serial.h"
#include "dect2/hfutil/hfu_hdr.h"
#include "dect2/hfutil/hfu_ale3g.h"
#include "dect2/hfutil/hfu_ale3gbw.h"
#include "dect2/hfutil/hfu_link11.h"
#include "dect2/hfutil/hfu_4481.h"
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
    std::unique_ptr<hfu::Ale3gRx> ale3g;
    std::unique_ptr<hfu::Link11Rx> l11;
    std::unique_ptr<hfu::Stanag4481Rx> s4481;
    int s4481Last = -1;
    double l11Dop = 0;
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
        } else if (kind == kHfuKind4481) {
            s4481 = std::make_unique<hfu::Stanag4481Rx>();
            s4481->setEventSink([this](const std::string& s) { addEvent(s); });
            s4481->setTextSink([this](int ch, const std::string& s) {
                // a channel tag whenever the text comes from another channel than the last one
                std::string t;
                if (ch != s4481Last) { char b[24]; snprintf(b, sizeof b, "\n[%s %d] ", ch ? "ch" : "single", ch); t = ch ? b : "\n[single] "; s4481Last = ch; }
                addText(t + s);
            });
        } else if (kind == kHfuKindLink11) {
            l11 = std::make_unique<hfu::Link11Rx>();
            l11->setEventSink([this](const std::string& s) { if (s.rfind("message", 0) != 0) addEvent(s); });
            l11->setMessageSink([this](const hfu::Link11Msg& m) {
                char t[220];
                snprintf(t, sizeof t, "Link 11 message: %s frames, %d data frames, %.2f s, Doppler tone %+.1f Hz, phase changes at %.0f deg", m.fast ? "fast (13.33 ms)" : "slow (22 ms)", m.frames, m.durationSec, m.dopplerHz, m.offsetDeg);
                addEvent(t);
                std::string s = t;
                s += "\n";
                // the first frames' raw phase changes (quarter turns, tones from 935 Hz up to 2915 Hz)
                for (size_t i = 0; i < m.phases.size() && i < 8; i++) {
                    s += "  ";
                    for (uint8_t q : m.phases[i]) s += (char)('0' + q);
                    s += "\n";
                }
                if (m.phases.size() > 8) s += "  ...\n";
                addText(s);
                std::lock_guard<std::mutex> l(mu);
                l11Dop = m.dopplerHz;
            });
        } else if (kind == kHfuKindAle3g) {
            ale3g = std::make_unique<hfu::Ale3gRx>();
            ale3g->setPduSink([this](const hfu::Ale3gPdu& p) {
                char t[300];
                snprintf(t, sizeof t, "%s%s: %s (SNR %.0f dB, %+.1f Hz)", p.type.c_str(), p.crcOk ? "" : " (CRC bad)", p.text.c_str(), p.snrDb, p.cfoHz);
                addEvent(t);
                if (p.crcOk) { addText(p.type + ": " + p.text + "\n"); std::lock_guard<std::mutex> l(mu); tel.pdus++; }
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
        if (ale3g) ale3g->reset();
        if (l11) l11->reset();
        if (s4481) s4481->reset();
        s4481Last = -1;
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
    if (p_->ale3g) p_->ale3g->push(p_->base.data(), p_->base.size());
    if (p_->l11) p_->l11->push(p_->base.data(), p_->base.size());
    if (p_->s4481) p_->s4481->push(p_->base.data(), p_->base.size());
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
    } else if (p_->s4481) {
        const auto s = p_->s4481->status();
        p_->tel.state = s.mode ? 2 : 0;
        std::string m = s.mode == 2 ? "multi-channel" : s.mode == 1 ? "single channel" : "";
        uint64_t ok = 0, bad = 0;
        double baud = 0;
        for (const auto& c : s.channels) {
            char b[96];
            if (c.n) snprintf(b, sizeof b, ", ch %d %s", c.n, c.startStop ? "text" : "sync");
            else snprintf(b, sizeof b, ", %.0f/%.0f Hz %s", c.markHz, c.spaceHz, c.startStop ? "text" : "sync");
            m += b;
            if (c.baud > 0) { snprintf(b, sizeof b, " %.2f Bd", c.baud); m += b; baud = c.baud; }
            ok += c.chars; bad += c.framingErrors;
        }
        p_->tel.mode = m;
        p_->tel.baud = baud;
        p_->tel.blocksOk = s.channels.size(); p_->tel.blocksBad = bad; p_->tel.dataValid = ok > 0;
        if (s.mode == 1 && !s.channels.empty()) { p_->tel.centreHz = 0.5 * (s.channels[0].markHz + s.channels[0].spaceHz); p_->tel.shiftHz = s.channels[0].spaceHz - s.channels[0].markHz; }
    } else if (p_->l11) {
        const auto& r = *p_->l11;
        p_->tel.state = r.state() ? 2 : (r.messages() ? 1 : 0); p_->tel.cfoHz = p_->l11Dop;
        char m[80]; snprintf(m, sizeof m, "%s, %llu messages", r.state() == 2 ? "frames" : r.state() == 1 ? "preamble" : "waiting", (unsigned long long)r.messages());
        p_->tel.mode = r.messages() || r.state() ? m : "";
        p_->tel.blocksOk = r.messages(); p_->tel.dataValid = r.messages() > 0;
    } else if (p_->ale3g) {
        const auto& r = *p_->ale3g;
        p_->tel.state = r.bursts() ? 2 : 0; p_->tel.cfoHz = r.lastCfoHz(); p_->tel.snrDb = (float)r.lastSnrDb();
        char m[80]; snprintf(m, sizeof m, "%llu bursts, %llu PDUs", (unsigned long long)r.bursts(), (unsigned long long)r.good());
        p_->tel.mode = r.bursts() ? m : "";
        p_->tel.blocksOk = r.good(); p_->tel.blocksBad = r.bursts() - r.good(); p_->tel.dataValid = r.good() > 0;
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
    static const char* ids[kHfuKindsAll] = {"milstd110", "stanag4285", "arqe", "arqn", "autospec", "spreadhf", "gtor", "efr", "piccolo", "coquelet", "stanag4539", "ale3g", "link11", "stanag4481"};
    static const double def[kHfuKindsAll] = {7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 14.0, 0.1276, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0};
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
        if (kind == kHfuKind4481) {
            hfu::S4481GenCfg c;
            c.multi = true;
            x = hfu::genStanag4481(c, {{3, msg, 0, 1}, {7, "", 600, 2}, {11, msg, 0, 3}}, 10.0);
        } else if (kind == kHfuKindLink11) {
            std::vector<std::array<uint8_t, 15>> fr(60);
            std::mt19937 g(11);
            for (auto& f : fr) for (auto& q : f) q = (uint8_t)(g() & 3);
            hfu::Link11GenCfg c;
            for (int m = 0; m < 2; m++) { const auto b = hfu::genLink11(c, fr); x.insert(x.end(), b.begin(), b.end()); }
        } else if (kind == kHfuKindAle3g) {
            // a call, the handshake and its answer, with gaps
            for (uint32_t pdu : {hfu::ale3gCall(5, 2, 17, 3), hfu::ale3gHandshake(hfu::ale3gLinkId((17 << 5) | 3, (5 << 5) | 3), 2, 12), hfu::ale3gNotification(0, 5, 3)}) {
                const auto b = hfu::genBw0(pdu, 0.2, 0.2);
                x.insert(x.end(), b.begin(), b.end());
            }
            // then traffic management and a short LDL transfer: TM_REQUEST, TM_CONFIRM, LDL_DATA, LDL_ACK, TM_TERM
            const std::vector<uint8_t> d(msg.begin(), msg.begin() + std::min<size_t>(msg.size(), 64));
            for (const auto& b : {hfu::genBw1(hfu::tmPdu(0, 2, (5 << 5) | 3, (17 << 5) | 3, 7), 0.2, 0.2), hfu::genBw1(hfu::tmPdu(1, 3, (17 << 5) | 3, (5 << 5) | 3, 7), 0.2, 0.2),
                                  hfu::genBw3(hfu::ldlPacket(d, 64, hfu::seqOnly((int)d.size())), 64, 0, 0.2, 0.2), hfu::genBw4(3, 0.2, 0.2),
                                  hfu::genBw1(hfu::tmPdu(2, 3, (17 << 5) | 3, (5 << 5) | 3, 0), 0.2, 0.2)})
                x.insert(x.end(), b.begin(), b.end());
        } else if (kind == kHfuKindHdr) {
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
