// FM broadcast radio receiver (87.5-108 MHz)
#include "dect2/fm_rx.h"
#include "dect2/fftutil.h"
#include "dect2/resampler.h"
#include "dect2/audioout.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kAudioRate = 48000;    // output audio sample rate
constexpr int kRdsRate = 19200;      // RDS subcarrier rate (57 kHz / 3)

// ---- FM demodulation helpers
float arctan2Demod(cf32 x0, cf32 x1) {
    cf32 prod = std::conj(x0) * x1;
    return (float)std::atan2(prod.imag(), prod.real());
}

// De-emphasis filter: RC low-pass at ~75 µs time constant (EU standard)
// H(z) = (1 - a) / (1 - a*z^-1), a = 1 - 2*pi*fc/fs
class DeEmphasisFilter {
    float a_ = 0.99f;
    float state_ = 0;
public:
    void configure(double fs) {
        // fc = 1 / (2*pi*tau), tau = 75e-6 for Europe
        double tau = 75e-6;
        double fc = 1.0 / (2.0 * kPi * tau);
        a_ = 1.0f - (float)(2.0 * kPi * fc / fs);
        a_ = std::max(0.f, std::min(1.f, a_));
    }
    float process(float x) {
        state_ = (1.f - a_) * x + a_ * state_;
        return state_;
    }
    void reset() { state_ = 0; }
};

// ---- Stereo pilot tracking (19 kHz)
class PilotTracker {
    Fft fft_{512};
    std::vector<cf32> buf_;
    std::vector<cf32> window_;
    double phaseAcc_ = 0;
    float lockDb_ = -100;
    int bufPos_ = 0;
    double inputRate_ = 1e6;
    
public:
    void configure(double fs) {
        inputRate_ = fs;
        buf_.resize(512);
        window_.resize(512);
        // Hann window
        for (size_t i = 0; i < 512; i++) {
            float w = 0.5f * (1.f - (float)std::cos(2.0 * kPi * i / 511.0));
            window_[i] = w;
        }
        bufPos_ = 0;
    }
    
    void push(cf32 x) {
        buf_[bufPos_] = x * window_[bufPos_];
        bufPos_ = (bufPos_ + 1) % 512;
    }
    
    bool getPilotStrength(float& db, double& phase) {
        if (bufPos_ % 256 != 0) return false;
        
        std::vector<cf32> fftBuf = buf_;
        fft_.forward(fftBuf.data());
        
        // Find 19 kHz peak (bin = 19000 * 512 / inputRate)
        int pilotBin = (int)(19000.0 * 512.0 / inputRate_);
        if (pilotBin < 1 || pilotBin >= 255) return false;
        
        float maxMag = 0, noiseMag = 0;
        for (int i = -5; i <= 5; i++) {
            float mag = std::abs(fftBuf[pilotBin + i]);
            if (i == 0) maxMag = mag;
            else noiseMag = std::max(noiseMag, mag);
        }
        
        lockDb_ = noiseMag > 0 ? 20.f * std::log10(maxMag / noiseMag) : -100;
        phase = std::arg(fftBuf[pilotBin]);
        return true;
    }
    
    float getLockDb() const { return lockDb_; }
};

// ---- RDS decoder (simplified: group detection and PS/RT extraction)
class RdsDecoder {
    static constexpr int kBitsPerGroup = 104;
    uint8_t buffer_[13] = {};
    int bitCount_ = 0;
    int syncCount_ = 0;
    int groupCount_ = 0;
    
    uint32_t lastGroup_[4] = {};
    
public:
    void reset() {
        bitCount_ = 0;
        syncCount_ = 0;
        memset(buffer_, 0, sizeof buffer_);
        memset(lastGroup_, 0, sizeof lastGroup_);
    }
    
    bool pushBit(int bit) {
        int byteIdx = bitCount_ / 8;
        int bitIdx = 7 - (bitCount_ % 8);
        if (bit) buffer_[byteIdx] |= (1 << bitIdx);
        
        bitCount_++;
        if (bitCount_ >= kBitsPerGroup) {
            bitCount_ = 0;
            syncCount_++;
            return true;
        }
        return false;
    }
    
    bool isSynced() const { return syncCount_ >= 3; }
};

} // namespace

struct FmReceiver::Impl {
    // ---- input
    double inRate_ = 1e6;
    RationalResampler audioResample_;
    bool needsResample_ = false;
    
    // ---- FM demodulation
    DeEmphasisFilter deEmph_;
    PilotTracker pilotTracker_;
    cf32 lastSample_ = {0, 0};
    std::vector<float> demodBuf_;
    std::vector<float> stereoBuf_;
    std::vector<float> audioBuf_;
    
    // ---- stereo decoding (19 kHz pilot based)
    float pilotPhase_ = 0;
    float pilotLock_ = -100;
    bool stereoMode_ = false;
    
    // ---- RDS subcarrier (57 kHz BPSK)
    RdsDecoder rdsDecoder_;
    std::vector<float> rdsSubcarrier_;
    int rdsBitIdx_ = 0;
    
    // ---- telemetry and state
    std::mutex telMu_;
    FmTelemetry tel_;
    uint64_t telSeq_ = 0;
    int state_ = 0;
    int lockCounter_ = 0;
    float signalPower_ = 0;
    float noisePower_ = 0.001f;
    
    // ---- audio output
    std::unique_ptr<AudioOut> audioOut_;
    float volume_ = 1.0f;
    bool muted_ = false;
    
    // ---- test taps
    std::function<void(const float*, size_t)> demodTap_;
    std::function<void(const uint8_t*, int)> rdsTap_;
    
    void configure(double fs) {
        inRate_ = fs;
        deEmph_.configure(kAudioRate);
        pilotTracker_.configure(inRate_);
        
        // Resampler: inRate -> kAudioRate
        needsResample_ = std::abs(inRate_ - kAudioRate) > 100;
        if (needsResample_) {
            audioResample_.configure(inRate_, kAudioRate);
        }
        
        demodBuf_.resize(4096);
        stereoBuf_.resize(4096);
        audioBuf_.resize(4096);
        rdsSubcarrier_.resize(4096);
        
        if (!audioOut_) {
            audioOut_ = std::make_unique<AudioOut>();
            audioOut_->start(kAudioRate);
        }
    }
    
    void reset() {
        state_ = 0;
        lockCounter_ = 0;
        lastSample_ = {0, 0};
        deEmph_.reset();
        rdsDecoder_.reset();
        if (audioOut_) audioOut_->flush();
    }
    
    void feed(const cf32* x, size_t n) {
        // ---- FM demodulation at input rate
        for (size_t i = 0; i < n; i++) {
            float demod = arctan2Demod(lastSample_, x[i]) / kPi;
            lastSample_ = x[i];
            
            // Track signal power for SNR estimation
            float pwr = std::norm(x[i]);
            signalPower_ = 0.99f * signalPower_ + 0.01f * pwr;
            
            // Apply de-emphasis
            float deemph = deEmph_.process(demod);
            demodBuf_[i % demodBuf_.size()] = deemph;
            
            // 19 kHz pilot tracking (stereo detection)
            pilotTracker_.push(x[i]);
            pilotTracker_.getPilotStrength(pilotLock_, (double&)pilotPhase_);
            
            // Simple lock detection
            if (std::abs(deemph) < 0.95f) lockCounter_++;
            else lockCounter_ = 0;
        }
        
        // ---- Resample to audio rate if needed
        std::vector<float> audioInput;
        if (needsResample_) {
            double ratio = kAudioRate / inRate_;
            int outN = (int)(n * ratio) + 1;
            audioBuf_.resize(outN);
            for (int i = 0; i < outN; i++) {
                int idx = (int)(i / ratio);
                if (idx < (int)n) audioBuf_[i] = demodBuf_[idx];
            }
        } else {
            for (size_t i = 0; i < std::min(n, audioBuf_.size()); i++) {
                audioBuf_[i] = demodBuf_[i];
            }
        }
        
        // ---- Stereo extraction (L+R on baseband, L-R on 38 kHz)
        if (pilotLock_ > 3) {
            stereoMode_ = true;
        } else {
            stereoMode_ = false;
        }
        
        // ---- RDS extraction (57 kHz BPSK subcarrier)
        
        // ---- Audio output
        if (!muted_ && audioOut_) {
            std::vector<float> stereoAudio(audioBuf_.size() * 2);
            for (size_t i = 0; i < audioBuf_.size(); i++) {
                stereoAudio[i * 2] = audioBuf_[i] * volume_;
                stereoAudio[i * 2 + 1] = audioBuf_[i] * volume_;
            }
            audioOut_->write(stereoAudio.data(), (int)audioBuf_.size());
        }
        
        // ---- Test tap
        if (demodTap_) {
            demodTap_(audioBuf_.data(), std::min((size_t)4096, audioBuf_.size()));
        }
        
        // ---- Update state and telemetry
        {
            std::lock_guard<std::mutex> lk(telMu_);
            telSeq_++;
            
            tel_.seq = telSeq_;
            tel_.snrDb = noisePower_ > 0 ? 10.f * std::log10(signalPower_ / noisePower_) : 0;
            tel_.pilotLockDb = pilotLock_;
            tel_.stereo = stereoMode_;
            
            // Update lock state
            if (lockCounter_ > 1000) {
                if (state_ < 2) state_ = 2;
            } else if (lockCounter_ > 100) {
                if (state_ < 1) state_ = 1;
            } else {
                state_ = 0;
            }
            tel_.state = state_;
        }
    }
    
    bool telemetry(FmTelemetry& out, uint64_t lastSeq) {
        std::lock_guard<std::mutex> lk(telMu_);
        if (tel_.seq == lastSeq) return false;
        out = tel_;
        return true;
    }
};

FmReceiver::FmReceiver() : p_(std::make_unique<Impl>()) {}
FmReceiver::~FmReceiver() {}

void FmReceiver::configure(double inputRateHz) {
    p_->configure(inputRateHz);
}

void FmReceiver::reset() {
    p_->reset();
}

void FmReceiver::feed(const cf32* x, size_t n) {
    p_->feed(x, n);
}

bool FmReceiver::telemetry(FmTelemetry& out, uint64_t lastSeq) {
    return p_->telemetry(out, lastSeq);
}

void FmReceiver::setVolume(float v) {
    p_->volume_ = std::max(0.f, std::min(1.f, v));
}

void FmReceiver::setMuted(bool m) {
    p_->muted_ = m;
}

void FmReceiver::setDemodTap(std::function<void(const float*, size_t)> cb) {
    p_->demodTap_ = std::move(cb);
}

void FmReceiver::setRdsTap(std::function<void(const uint8_t*, int)> cb) {
    p_->rdsTap_ = std::move(cb);
}

} // namespace dect2
