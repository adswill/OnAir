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
        history_[head_] = history_[head_ + 257] = std::isfinite(audio[i]) ? audio[i] : 0;
        double conditioned = 0;
        const double* h = history_.data() + head_ + 257;   // h[-j] is the sample j back (the same values as the ring read modulo 257)
        for (size_t j = 0; j < taps_.size(); j++) conditioned += taps_[j] * h[-(ptrdiff_t)j];
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
    narrowAudio_ = AudioDecoder(); afcHz_ = afcPhase_ = widePow_ = wideFloor_ = 0; floorN_ = 0; r1_ = noiseR1_ = nr1_ = nNoiseR1_ = {}; narrowPow_ = narrowFloor_ = 0; narrowPrev_ = {}; recent_.clear();
    auto butter = [](double hz, size_t i) {   // stage i of a 4th-order Butterworth low-pass at 48 kHz
        const double w = 2 * pi * hz / 48000, c = std::cos(w), a = std::sin(w) / (2 * (i ? 1.3066 : 0.5412));
        return Filter{(1 - c) / 2 / (1 + a), (1 - c) / (1 + a), (1 - c) / 2 / (1 + a), -2 * c / (1 + a), (1 - a) / (1 + a)};
    };
    for (size_t i = 0; i < channel_.size(); i++) {
        // Leave room for the +/-8 kHz transmitter errors seen by the original
        // receiver; the audio bandpass removes discriminator DC afterwards.
        channel_[i] = butter(14000, i);
        // the narrow path: the signal is about +-4.3 kHz wide (2.5 kHz deviation, tones up to 1.8 kHz)
        narrow_[i] = butter(5500, i);
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
    std::vector<float> audio, narrowAudio; audio.reserve(baseband.size()); narrowAudio.reserve(baseband.size());
    for (auto x : baseband) {
        std::complex<double> v{x.real(), x.imag()}; for (auto& f : channel_) v = f.run(v);
        const cf32 y(float(v.real()), float(v.imag()));
        const cf32 lag = y * std::conj(previous_);
        const float deviation = std::arg(lag);
        audio.push_back(deviation); previous_ = y;
        meanDeviation_ += (deviation - meanDeviation_) / 4800;
        power_ += (std::norm(v) - power_) / 2400;
        // Carrier for the narrow path: the lag-1 product of the wide path averaged over 20 ms, less the same average on noise alone (the filtered
        // noise adds a real, positive term that would pull the estimate towards zero). Its angle is the carrier frequency; the FM of the tones
        // averages to a real factor and does not move it. Followed only while the wide path holds more than the noise (1 dB, over 20 ms).
        const double p = std::norm(v);
        if (floorN_ == 0) widePow_ = p;
        widePow_ += (p - widePow_) / 960;
        // the first 200 ms: the lowest 20 ms average after the first 20 ms (a mean would take in a burst that is on from the start)
        if (floorN_ < 9600) { ++floorN_; wideFloor_ = floorN_ <= 960 ? widePow_ : std::min(wideFloor_, widePow_); }
        else wideFloor_ += (widePow_ < wideFloor_ ? 1.0 / 2400 : widePow_ < 2 * wideFloor_ ? 1.0 / 9600 : 1.0 / 480000) * (widePow_ - wideFloor_);
        const std::complex<double> l{lag.real(), lag.imag()};
        r1_ += (l - r1_) / 960.0;
        const bool present = floorN_ >= 9600 && widePow_ > 1.25 * wideFloor_;
        if (!present) noiseR1_ += (l - noiseR1_) / 9600.0;
        afcPhase_ -= 2 * pi * afcHz_ / 48000;
        if (afcPhase_ > pi || afcPhase_ < -pi) afcPhase_ = std::remainder(afcPhase_, 2 * pi);
        std::complex<double> u = std::complex<double>{x.real(), x.imag()} * std::complex<double>(std::cos(afcPhase_), std::sin(afcPhase_));
        for (auto& f : narrow_) u = f.run(u);
        const cf32 z(float(u.real()), float(u.imag()));
        const cf32 nlag = z * std::conj(narrowPrev_);
        narrowAudio.push_back(std::arg(nlag)); narrowPrev_ = z;
        // the same on the narrow path: once the carrier is inside it, its residual is the finer (unbiased) estimate; the wide path's is pulled
        // towards the centre when the +-14 kHz filter clips one side of a signal far off, and only brings the carrier into the narrow path
        const double np = std::norm(std::complex<double>{z.real(), z.imag()});
        if (floorN_ <= 1) narrowPow_ = np;
        narrowPow_ += (np - narrowPow_) / 960;
        if (floorN_ < 9600) narrowFloor_ = floorN_ <= 960 ? narrowPow_ : std::min(narrowFloor_, narrowPow_);
        else narrowFloor_ += (narrowPow_ < narrowFloor_ ? 1.0 / 2400 : narrowPow_ < 2 * narrowFloor_ ? 1.0 / 9600 : 1.0 / 480000) * (narrowPow_ - narrowFloor_);
        const std::complex<double> nl{nlag.real(), nlag.imag()};
        nr1_ += (nl - nr1_) / 960.0;
        const bool nPresent = floorN_ >= 9600 && narrowPow_ > 1.5 * narrowFloor_;
        if (!present && !nPresent) nNoiseR1_ += (nl - nNoiseR1_) / 9600.0;
        if (present || nPresent) {
            double target = afcHz_;
            const std::complex<double> sn = nr1_ - nNoiseR1_, sw = r1_ - noiseR1_;
            if (nPresent && std::norm(sn) > 0) target = afcHz_ + std::arg(sn) * 48000 / (2 * pi);
            else if (std::norm(sw) > 0) target = std::arg(sw) * 48000 / (2 * pi);
            afcHz_ = std::clamp(afcHz_ + (target - afcHz_) / 240, -10000.0, 10000.0);
        }
    }
    std::vector<Message> found;
    audio_.feed(audio.data(), audio.size(), found);
    narrowAudio_.feed(narrowAudio.data(), narrowAudio.size(), found);
    const double now = audio_.timeSec();
    std::stable_sort(found.begin(), found.end(), [](const Message& a, const Message& b) { return a.timeSec < b.timeSec; });
    for (auto& m : found) {   // a burst both paths decode is one frame
        auto it = recent_.find(m.raw);
        if (it != recent_.end() && std::fabs(m.timeSec - it->second) < 0.25) continue;
        recent_[m.raw] = m.timeSec; out.push_back(std::move(m));
    }
    for (auto it = recent_.begin(); it != recent_.end();) { if (now - it->second > 1) it = recent_.erase(it); else ++it; }
}
float Modem::levelDb() const { return float(10 * std::log10(std::max(power_, 1e-20))); }
} // namespace dect2::railway
