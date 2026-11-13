// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SDR transmit path of the ham modes (see ham_sdr.h): audio to single-sideband IQ, ALC, interpolation, and the HamRigIo that keys a TxSink
// only after the gate.
#include "dect2/ham/ham_sdr.h"
#include "dect2/ham/ham_safety.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2::ham {

namespace {
constexpr int kTaps = 361;                 // the band-pass: 600 Hz transition at 48 kHz, Kaiser beta 6.76 (about 70 dB)
constexpr int kMid = kTaps / 2;
constexpr double kFs = 48000;
constexpr double kCentreHz = (kSdrAudioLoHz + kSdrAudioHiHz) / 2;   // 1600 Hz
constexpr double kHalfHz = (kSdrAudioHiHz - kSdrAudioLoHz) / 2 + 300; // the low-pass prototype's cutoff: the middle of the transition
constexpr int kRampIn = 240;               // 5 ms raised-cosine start: no key click whatever the audio does
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
// cubic B-spline weights for the segment between s1 and s2 at fraction u
inline void bspline(double u, float* w) {
    const double u2 = u * u, u3 = u2 * u, v = 1 - u;
    w[0] = (float)(v * v * v / 6);
    w[1] = (float)((3 * u3 - 6 * u2 + 4) / 6);
    w[2] = (float)((-3 * u3 + 3 * u2 + 3 * u + 1) / 6);
    w[3] = (float)(u3 / 6);
}
inline bool isBoard(const DeviceInfo& d, const char* what) {
    std::string a = d.board + " " + d.name;
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    return a.find(what) != std::string::npos;
}
}  // namespace

// ---- what cannot be done, and warnings ----
// Only what is physically impossible is refused here: a radio that cannot transmit, a CAT rig (it has its own audio path). Everything else (the TX
// gain range, the audio window, the HackRF amplifier, the radio's tuning range, the power limit) is the operator's decision: see sdrTxLimitWarnings.
bool sdrTxRefused(const DeviceInfo& d, const SdrTxConfig&, double, double, double, std::string& why) {
    why.clear();
    if (!d.canTransmit) why = txUnavailableReason(d).empty() ? "this radio cannot transmit" : txUnavailableReason(d);
    else if (d.kind == DeviceInfo::Hamlib) why = "a CAT rig transmits through its own audio input, not through the SDR path";
    return !why.empty();
}

// Warnings (never a refusal) for settings outside what the radio or the chain is made for. The operator is responsible for the law and the hardware.
std::string sdrTxLimitWarnings(const DeviceInfo& d, const SdrTxConfig& c, double dialHz, double audioHz, double bwHz) {
    std::string w;
    auto add = [&](const std::string& s) { w += (w.empty() ? "" : " ") + s; };
    if (!(c.powerLimitPct > 0 && c.powerLimitPct <= 100)) add("The power limit is outside 1 to 100 %.");
    if (!(c.drive > 0 && c.drive <= 4)) add("The drive is outside 0 to 4: the signal will be distorted or silent.");
    if (d.txGainMaxDb > d.txGainMinDb && (c.gainDb < d.txGainMinDb - 1e-9 || c.gainDb > d.txGainMaxDb + 1e-9)) {
        char b[160];
        snprintf(b, sizeof b, "The TX gain %.1f dB is outside the radio's %.0f to %.0f dB: overdriving can damage the radio.", c.gainDb, d.txGainMinDb, d.txGainMaxDb);
        add(b);
    }
    if (!(audioHz >= kSdrAudioLoHz && audioHz + std::max(0.0, bwHz) <= kSdrAudioHiHz)) add("The TX audio is outside 300 to 2900 Hz, which the SDR chain filters: part of the signal will be cut.");
    if (!txRangeInfo(d, dialHz).empty()) add(txRangeInfo(d, dialHz).substr(0, txRangeInfo(d, dialHz).find(';')) + ": the radio may not be able to send it.");
    if (c.ampOn && !isBoard(d, "hackrf")) add("Only a HackRF has the +14 dB amplifier switch.");
    if (c.ampOn) add("The amplifier is on: overdriving or a long duty cycle can damage the radio.");
    return w;
}

std::vector<std::string> sdrTxWarnings(const DeviceInfo& d, const SdrTxConfig& c, double dialHz) {
    std::vector<std::string> w;
    w.push_back("Band filter: an SDR's output has harmonics and DAC images; a low-pass or band filter for this band must sit between the radio and "
                "the antenna (or the amplifier).");
    if (isBoard(d, "hackrf")) {
        w.push_back("HackRF has no power amplifier: a few milliwatts (about 0 to 15 dBm), weaker below 10 MHz. Its output turns non-linear near the top "
                    "of the gain range: a cleaner signal at lower gain.");
        if (dialHz > 0 && dialHz < 30e6) w.push_back("HF from a HackRF: the second and third harmonics are strong below 30 MHz; never transmit without the low-pass filter.");
        if (c.ampOn) w.push_back("The HackRF +14 dB amplifier is on: do not drive an external amplifier from it without an attenuator.");
    } else if (isBoard(d, "pluto")) {
        w.push_back("PlutoSDR: about 0 to 7 dBm, no power amplifier; the stock radio tunes from 325 MHz (70 MHz with the AD9364 setting).");
    } else if (isBoard(d, "lime")) {
        w.push_back("LimeSDR: about 0 to 10 dBm, no power amplifier; below 30 MHz the output is weak and rich in harmonics.");
    } else if (isBoard(d, "usrp") || isBoard(d, "uhd") || isBoard(d, "b200") || isBoard(d, "b210")) {
        w.push_back("USRP: up to about 10 dBm, no power amplifier and no output filter below 50 MHz.");
    } else if (isBoard(d, "blade")) {
        w.push_back("bladeRF: up to about 6 dBm, no power amplifier; the bladeRF 2.0 tunes from 47 MHz.");
    } else {
        w.push_back("This SDR has no power amplifier: its output is milliwatts.");
    }
    if (d.txGainMaxDb > d.txGainMinDb && c.gainDb >= d.txGainMaxDb - 3)
        w.push_back("The TX gain is at the top of the range: the signal will splatter. Lower it and watch the spectrum.");
    if (c.externalPa)
        w.push_back("External amplifier: keep the SDR level below the amplifier's maximum input, and filter after the amplifier.");
    if (c.powerLimitPct < 100) {
        char b[96];
        snprintf(b, sizeof b, "Power limit: %.0f %% of the radio's full output.", c.powerLimitPct);
        w.push_back(b);
    }
    w.push_back(d.txFullDuplex ? "Full duplex: the receiver can stay on to watch your own signal (keep the receive input attenuated: the transmitter is close)."
                               : "Half duplex: the receiver of this radio stops while it sends; the TX monitor shows what was sent.");
    return w;
}

double sdrTxRate(const DeviceInfo& d, const SdrTxConfig& c) {
    if (c.rateHz > 0) return c.rateHz;
    const double target = std::max(d.txMinRateHz, 1.92e6);
    double r = std::ceil(target / kFs - 1e-9) * kFs;
    if (d.txMaxRateHz > 0 && r > d.txMaxRateHz) r = std::floor(d.txMaxRateHz / kFs) * kFs;
    if (r < d.txMinRateHz) r = d.txMinRateHz;   // nothing on the 48 kHz raster fits: the fractional path
    return r;
}

// ---- the chain ----
SdrTxChain::SdrTxChain(double outRateHz, const SdrTxConfig& c) : outRate_(outRateHz), c_(c) {
    hr_.resize(kTaps);
    hi_.resize(kTaps);
    const double beta = 6.76, fc = kHalfHz / kFs;
    std::vector<double> lp(kTaps);
    double sum = 0;
    for (int k = 0; k < kTaps; k++) {
        const double n = k - kMid, r = n / kMid;
        const double sinc = n == 0 ? 2 * fc : std::sin(2 * kPi * fc * n) / (kPi * n);
        lp[k] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        sum += lp[k];
    }
    for (int k = 0; k < kTaps; k++) {   // stored reversed (newest sample last in the window), x2: a real cosine of amplitude A gives |z| = A
        const double n = k - kMid, ph = 2 * kPi * kCentreHz / kFs * n, g = 2 * lp[k] / sum;
        hr_[kTaps - 1 - k] = (float)(g * std::cos(ph));
        hi_[kTaps - 1 - k] = (float)(g * std::sin(ph));
    }
    limit_ = (float)(std::sqrt(std::clamp(c_.powerLimitPct, 0.0, 100.0) / 100.0) * 0.99);
    release_ = (float)std::pow(10.0, 10.0 / 20.0 / kFs);
    step_ = kFs / outRate_;
    const double ratio = outRate_ / kFs;
    if (std::fabs(ratio - std::round(ratio)) < 1e-9 && ratio >= 1) {
        ratio_ = (int)std::lround(ratio);
        w_.resize((size_t)ratio_ * 4);
        for (int p = 0; p < ratio_; p++) bspline((double)p / ratio_, &w_[(size_t)p * 4]);
    }
    reset();
}

void SdrTxChain::reset() {
    hist_.assign((size_t)kTaps * 2, 0.f);
    hpos_ = 0;
    for (auto& s : s_) s = cf32(0, 0);
    u_ = 0;
    gain_ = 1;
    in_ = 0;
    st_ = Stats();
    mon_.clear();
}

double SdrTxChain::latencySec() const { return (kMid + 2) / kFs; }

void SdrTxChain::push48(cf32 z, std::vector<cf32>& out) {
    z *= (float)c_.drive;
    const float m = std::abs(z);
    if (m * gain_ > limit_) { gain_ = limit_ / m; st_.alcSamples++; }
    else if (gain_ < 1) gain_ = std::min(1.f, gain_ * release_);
    z *= gain_;
    st_.alcDb = std::min(st_.alcDb, 20.f * std::log10(std::max(gain_, 1e-6f)));
    if (monitor) mon_.push_back(z.real());
    if (c_.lsb) z = std::conj(z);
    s_[0] = s_[1]; s_[1] = s_[2]; s_[2] = s_[3]; s_[3] = z;
    const size_t before = out.size();
    if (ratio_ > 0) {
        const float* w = w_.data();
        for (int p = 0; p < ratio_; p++, w += 4) {
            const cf32 y = w[0] * s_[0] + w[1] * s_[1] + w[2] * s_[2] + w[3] * s_[3];
            st_.outPeak = std::max(st_.outPeak, std::abs(y));
            out.push_back(y);
        }
    } else {
        float w[4];
        while (u_ < 1) {
            bspline(u_, w);
            const cf32 y = w[0] * s_[0] + w[1] * s_[1] + w[2] * s_[2] + w[3] * s_[3];
            st_.outPeak = std::max(st_.outPeak, std::abs(y));
            out.push_back(y);
            u_ += step_;
        }
        u_ -= 1;
    }
    st_.samplesOut += out.size() - before;
}

void SdrTxChain::process(const float* audio, size_t n, std::vector<cf32>& out) {
    out.reserve(out.size() + (size_t)((double)n * outRate_ / kFs) + 8);
    for (size_t i = 0; i < n; i++) {
        float x = std::isfinite(audio[i]) ? audio[i] : 0.f;
        st_.inPeak = std::max(st_.inPeak, std::fabs(x));
        if (in_ < (uint64_t)kRampIn) x *= (float)(0.5 - 0.5 * std::cos(kPi * (double)in_ / kRampIn));
        in_++;
        hist_[hpos_] = hist_[hpos_ + kTaps] = x;
        hpos_ = hpos_ + 1 == (size_t)kTaps ? 0 : hpos_ + 1;
        const float* win = &hist_[hpos_];   // oldest .. newest
        float r0 = 0, r1 = 0, r2 = 0, r3 = 0, q0 = 0, q1 = 0, q2 = 0, q3 = 0;
        int k = 0;
        for (; k + 4 <= kTaps; k += 4) {
            r0 += hr_[k] * win[k]; r1 += hr_[k + 1] * win[k + 1]; r2 += hr_[k + 2] * win[k + 2]; r3 += hr_[k + 3] * win[k + 3];
            q0 += hi_[k] * win[k]; q1 += hi_[k + 1] * win[k + 1]; q2 += hi_[k + 2] * win[k + 2]; q3 += hi_[k + 3] * win[k + 3];
        }
        for (; k < kTaps; k++) { r0 += hr_[k] * win[k]; q0 += hi_[k] * win[k]; }
        push48(cf32(r0 + r1 + r2 + r3, q0 + q1 + q2 + q3), out);
    }
    if (monitor && !mon_.empty()) { monitor(mon_.data(), mon_.size()); mon_.clear(); }
}

void SdrTxChain::flush(std::vector<cf32>& out) {
    std::vector<float> z((size_t)kTaps + 4, 0.f);
    const uint64_t keep = in_;
    in_ = std::max<uint64_t>(in_, kRampIn);   // the tail is silence: no ramp on it
    process(z.data(), z.size(), out);
    in_ = keep + z.size();
}

// ---- the HamRigIo ----
namespace {
class ClaimedSink : public TxSink {   // the radio stays claimed for transmitting as long as its sink lives
public:
    explicit ClaimedSink(std::unique_ptr<TxSink> s) : s_(std::move(s)) {}
    ~ClaimedSink() override { s_->close(); s_.reset(); txReleaseRadio(); }
    bool open(const TxTune& t, std::string& err) override { return s_->open(t, err); }
    bool write(const cf32* x, size_t n, std::string& err) override { return s_->write(x, n, err); }
    void close() override { s_->close(); }
    double sampleRate() const override { return s_->sampleRate(); }
    bool realtime() const override { return s_->realtime(); }
    bool fullDuplex() const override { return s_->fullDuplex(); }
    std::string name() const override { return s_->name(); }
private:
    std::unique_ptr<TxSink> s_;
};
}  // namespace

SdrHamIo::SinkFactory sdrSinkFactory(const DeviceInfo& d) {
    return [d](std::string& err) -> std::unique_ptr<TxSink> {
        if (!txClaimRadio(d, err)) return nullptr;
        auto s = makeTxSink(d);
        if (!s) {
            txReleaseRadio();
            err = txUnavailableReason(d).empty() ? d.name + ": this build has no transmit path for it" : txUnavailableReason(d);
            return nullptr;
        }
        return std::make_unique<ClaimedSink>(std::move(s));
    };
}

SdrHamIo::SdrHamIo(DeviceInfo d, SinkFactory make, SdrTxConfig c) : dev_(std::move(d)), make_(std::move(make)), cfg_(c) {}
SdrHamIo::~SdrHamIo() { closeSink(false); }

void SdrHamIo::setConfig(const SdrTxConfig& c) { std::lock_guard<std::mutex> l(cfgMu_); cfg_ = c; }
SdrTxConfig SdrHamIo::config() const { std::lock_guard<std::mutex> l(cfgMu_); return cfg_; }
void SdrHamIo::setMonitor(std::function<void(const float*, size_t)> m) { std::lock_guard<std::mutex> l(sinkMu_); monitor_ = std::move(m); }
SdrTxChain::Stats SdrHamIo::stats() const { std::lock_guard<std::mutex> l(sinkMu_); return chain_ ? chain_->stats() : SdrTxChain::Stats(); }
std::string SdrHamIo::lastError() const { std::lock_guard<std::mutex> l(sinkMu_); return err_; }
double SdrHamIo::latencySec() const { return (kMid + 2) / kFs + config().sinkLatencySec; }

bool SdrHamIo::prepare(const HamTxCheck& gate, std::string& err) {
    const SdrTxConfig c = config();
    std::lock_guard<std::mutex> l(sinkMu_);
    prepared_ = false;
    if (keyed_) { err = "the SDR is still transmitting"; return false; }
    std::string gw;
    if (!hamTxAllowed(gate, gw)) { err = gw; return false; }
    if (sdrTxRefused(dev_, c, gate.dialHz, gate.audioHz, gate.bwHz, err)) return false;
    const std::string limitWarn = sdrTxLimitWarnings(dev_, c, gate.dialHz, gate.audioHz, gate.bwHz);
    // the radio's centre is the suppressed carrier: its leak must stay inside the band too; on LSB the signal lies below it
    const double lo = c.lsb ? gate.dialHz - gate.audioHz - gate.bwHz : gate.dialHz;
    const double hi = c.lsb ? gate.dialHz : gate.dialHz + gate.audioHz + gate.bwHz;
    // not a refusal any more: a warning (kept in the audit log with the burst); the operator decides
    std::string warn;
    const bool bandWarned = gw.find("amateur band") != std::string::npos;   // the gate already said so
    if (bandWarned) {}
    else if (!hamSignalInBand(lo, 0, hi - lo)) warn = "The radio's centre frequency or the lower sideband is outside the amateur band. You are responsible for being allowed to transmit here.";
    else if (!gate.country.empty() && gw.empty()) { std::string w2; if (!hamSignalInCountryBand(gate.country, lo, 0, hi - lo, w2)) warn = w2 + ". You are responsible for being allowed to transmit here."; }
    if (!limitWarn.empty()) warn += std::string(warn.empty() ? "" : " ") + limitWarn;
    if (!c.bandFilterConfirmed) warn += std::string(warn.empty() ? "" : " ") + "No band filter is confirmed: an SDR's output carries strong harmonics, fit a low-pass or band filter for this band.";
    gate_ = gate;
    prepared_ = true;
    err_.clear();
    err = warn;   // true with text: warnings (HamSlotTx logs them)
    return true;
}

bool SdrHamIo::beginAudio(int rate, std::string& err) {
    if (rate != (int)kFs) { err = "the SDR path takes 48 kHz audio"; return false; }
    aborted_ = false;
    return true;
}

bool SdrHamIo::ptt(bool on, std::string& err) {
    if (!on) { closeSink(!aborted_); std::lock_guard<std::mutex> l(sinkMu_); prepared_ = false; return true; }
    const SdrTxConfig c = config();
    std::lock_guard<std::mutex> l(sinkMu_);
    if (keyed_) return true;
    if (!prepared_) { err = "the SDR transmits only through the ham transmitter (Arm), not directly"; return false; }
    std::string why;
    if (!hamTxAllowed(gate_, why)) { prepared_ = false; err = why; return false; }
    if (!make_) { err = "no radio"; return false; }
    sink_ = make_(err);
    if (!sink_) { prepared_ = false; if (err.empty()) err = "the radio could not be opened for transmitting"; return false; }
    TxTune t;
    t.centerHz = gate_.dialHz;
    t.sampleRate = sdrTxRate(dev_, c);
    t.gainDb = c.gainDb;
    t.ampOn = c.ampOn;
    if (!sink_->open(t, err)) { sink_.reset(); prepared_ = false; return false; }
    const double rate = sink_->sampleRate() > 0 ? sink_->sampleRate() : t.sampleRate;
    if (rate < 2 * kFs) { sink_->close(); sink_.reset(); prepared_ = false; err = "the radio's transmit rate is too low for the SSB path"; return false; }
    chain_ = std::make_unique<SdrTxChain>(rate, c);
    chain_->monitor = monitor_;
    keyed_ = true;
    return true;
}

int SdrHamIo::write(const float* mono, int frames) {
    if (frames <= 0) return 0;
    if (aborted_) return frames;   // the burst is being halted: HamSlotTx stops on its own flag
    if (!keyed_) { std::lock_guard<std::mutex> l(sinkMu_); err_ = "audio for the SDR while it is not keyed"; return -1; }
    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, 480);   // 10 ms blocks: a Halt waits at most one block
        std::lock_guard<std::mutex> l(sinkMu_);
        if (aborted_ || !sink_) return frames;
        buf_.clear();
        chain_->process(mono + done, (size_t)n, buf_);
        std::string e;
        if (!sink_->write(buf_.data(), buf_.size(), e)) { err_ = "the radio stopped: " + (e.empty() ? std::string("write failed") : e); return -1; }
        done += n;
    }
    return done;
}

void SdrHamIo::endAudio() {
    closeSink(false);
    std::lock_guard<std::mutex> l(sinkMu_);
    prepared_ = false;
}

void SdrHamIo::closeSink(bool tail) {
    std::lock_guard<std::mutex> l(sinkMu_);
    if (sink_) {
        if (tail && chain_ && keyed_) {   // the filter tail and 10 ms of silence, so the radio's buffer ends on zero
            buf_.clear();
            chain_->flush(buf_);
            buf_.resize(buf_.size() + (size_t)(chain_->outRate() * 0.01), cf32(0, 0));
            std::string e;
            sink_->write(buf_.data(), buf_.size(), e);
        }
        sink_->close();
        sink_.reset();
    }
    keyed_ = false;
}

}  // namespace dect2::ham
