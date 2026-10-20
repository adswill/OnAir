#include "dect2/railway/railway_modem.h"
#include <algorithm>
#include <cmath>

namespace dect2::railway {
namespace {
constexpr double pi = 3.14159265358979323846;
std::array<double, 257> lowpass(double hz) {
    std::array<double, 257> taps{}; double sum = 0;
    for (int i = 0; i < 257; i++) {
        const int x = i - 128;
        taps[(size_t)i] = (x ? std::sin(2 * pi * hz * x / 48000) / (pi * x) : 2 * hz / 48000) * (0.54 - 0.46 * std::cos(2 * pi * i / 256));
        sum += taps[(size_t)i];
    }
    for (auto& t : taps) t /= sum;
    return taps;
}
} // namespace
AudioDecoder::AudioDecoder() {
    const auto lo = lowpass(300), hi = lowpass(4000);
    double sum = 0;
    for (size_t i = 0; i < taps_.size(); i++) { taps_[i] = hi[i] - lo[i]; sum += taps_[i]; }
    taps_[128] -= sum;  // exactly zero DC gain, including rounding
}
void AudioDecoder::feed(const float* audio, size_t n, std::vector<Message>& out) {
    if (!audio) return;
    for (size_t i = 0; i < n; i++, samples_++) {
        history_[head_] = std::isfinite(audio[i]) ? audio[i] : 0;
        double conditioned = 0;
        for (size_t j = 0; j < taps_.size(); j++) conditioned += taps_[j] * history_[(head_ + 257 - j) % 257];
        head_ = (head_ + 1) % 257;
        for (size_t tone = 0; tone < 2; tone++) {
            const double angle = -2 * pi * (tone ? 1800 : 1200) * double(samples_ % 80) / 48000;
            const auto v = conditioned * std::complex<double>(std::cos(angle), std::sin(angle));
            sums_[tone] += v - toneHistory_[tone][toneHead_]; toneHistory_[tone][toneHead_] = v;
        }
        toneHead_ = (toneHead_ + 1) % 40;
        const bool bit = std::norm(sums_[0]) > std::norm(sums_[1]);
        std::vector<Message> found;
        if (samples_ % 5 == 0) finders_[(samples_ % 40) / 5].feed(bit, found);
        if (samples_ % 4 == 0) {
            if (bit != previous_) {
                // Transitions belong halfway between sampling instants. Locking
                // them to phase zero samples an edge and loses a drifting clock.
                const double error = phase_ - 0.5;
                phase_ -= 0.1 * error; step_ = std::clamp(step_ - 0.00005 * error, 0.099, 0.101);
            }
            phase_ += step_;
            if (phase_ >= 1) { phase_ -= 1; finders_[8].feed(bit, found); }
            previous_ = bit;
        }
        const double now = timeSec();
        for (auto& m : found) {
            auto it = recent_.find(m.raw);
            if (it != recent_.end() && now - it->second < 0.15) continue;
            recent_[m.raw] = now; m.timeSec = now; out.push_back(std::move(m));
        }
        if (samples_ % 4800 == 0) for (auto it = recent_.begin(); it != recent_.end();) {
            if (now - it->second > 1) it = recent_.erase(it); else ++it;
        }
    }
}
uint64_t AudioDecoder::syncs() const { uint64_t n = 0; for (const auto& f : finders_) n += f.syncs; return n; }
uint64_t AudioDecoder::failures() const { uint64_t n = 0; for (const auto& f : finders_) n += f.crcFailures + f.rejected + f.ambiguous; return n; }
std::complex<double> Modem::Filter::run(std::complex<double> x) {
    const auto y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y;
}
void Modem::configure(double rate) { rate_ = rate; resampler_.configure(rate, 48000, 8); reset(); }
void Modem::reset() {
    resampler_.reset(); audio_ = AudioDecoder(); phase_ = power_ = meanDeviation_ = 0; previous_ = {};
    for (size_t i = 0; i < channel_.size(); i++) {
        // Leave room for the +/-8 kHz transmitter errors seen by the original
        // receiver; the audio bandpass removes discriminator DC afterwards.
        const double w = 2 * pi * 14000 / 48000, c = std::cos(w), a = std::sin(w) / (2 * (i ? 1.3066 : 0.5412));
        channel_[i] = Filter{(1 - c) / 2 / (1 + a), (1 - c) / (1 + a), (1 - c) / 2 / (1 + a), -2 * c / (1 + a), (1 - a) / (1 + a)};
    }
}
void Modem::feed(const cf32* iq, size_t n, std::vector<Message>& out) {
    if (!iq || rate_ < 48000) return;
    std::vector<cf32> mixed(n), baseband;
    const double step = -2 * pi * offset_ / rate_;
    for (size_t i = 0; i < n; i++) {
        mixed[i] = iq[i] * cf32(float(std::cos(phase_)), float(std::sin(phase_)));
        phase_ += step;
        if (phase_ > pi || phase_ < -pi) phase_ = std::remainder(phase_, 2 * pi);
    }
    resampler_.process(mixed.data(), n, baseband);
    std::vector<float> audio; audio.reserve(baseband.size());
    for (auto x : baseband) {
        std::complex<double> v{x.real(), x.imag()}; for (auto& f : channel_) v = f.run(v);
        const cf32 y(float(v.real()), float(v.imag()));
        const float deviation = std::arg(y * std::conj(previous_));
        audio.push_back(deviation); previous_ = y;
        meanDeviation_ += (deviation - meanDeviation_) / 4800;
        power_ += (std::norm(v) - power_) / 2400;
    }
    audio_.feed(audio.data(), audio.size(), out);
}
float Modem::levelDb() const { return float(10 * std::log10(std::max(power_, 1e-20))); }
} // namespace dect2::railway
