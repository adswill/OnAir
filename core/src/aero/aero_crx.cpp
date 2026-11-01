#include "dect2/aero/aero_crx.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

struct AeroCReceiver::Impl {
    AeroFront front;
    std::unique_ptr<AeroDemod> dem;
    AeroCFramer fr;
    std::function<void(const AeroCFrameEvent&)> cb;
    double fsIn = 0, offset = 0, search = 2500, fsCh = 0;
    int D = 1;
    uint64_t fed = 0, lastFrame = 0, nFrames = 0;
    std::vector<cf32> tmp;

    void build() {
        if (fsIn <= 0) return;
        D = std::max(1, (int)std::floor(fsIn / 48000.0));
        fsCh = fsIn / D;
        front.setup(fsIn, offset, D);
        dem = std::make_unique<AeroDemod>(kAeroCBitRate, fsCh, search);
        fr.reset();
        dem->setBitSink([this](float s) { fr.push(s); }, [this]() { return fr.synced(); });
        fr.setCallback([this](AeroCFrameEvent& e) {
            e.ebn0Db = dem ? dem->ebn0Db() : 0.f;
            e.freqHz = offset + (dem ? dem->freqHz() : 0.0);
            nFrames++;
            lastFrame = fed;
            if (cb) cb(e);
        });
    }
};

AeroCReceiver::AeroCReceiver() : p_(std::make_unique<Impl>()) {}
AeroCReceiver::~AeroCReceiver() = default;
void AeroCReceiver::configure(double fsIn, double offsetHz, double searchHz) {
    p_->fsIn = fsIn; p_->offset = offsetHz; p_->search = searchHz;
    p_->fed = 0; p_->lastFrame = 0; p_->nFrames = 0;
    p_->build();
}
void AeroCReceiver::reset() { p_->fed = 0; p_->lastFrame = 0; p_->build(); }
void AeroCReceiver::setFrameCallback(std::function<void(const AeroCFrameEvent&)> cb) { p_->cb = std::move(cb); }
void AeroCReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    if (!s.dem) return;
    constexpr size_t kChunk = 8192;
    for (size_t i = 0; i < n; i += kChunk) {
        const size_t k = std::min(kChunk, n - i);
        if (s.tmp.size() < k / (size_t)s.D + 2) s.tmp.resize(k / (size_t)s.D + 2);
        const size_t m = s.front.process(x + i, k, s.tmp.data());
        s.dem->feed(s.tmp.data(), m);
        s.fed += k;
    }
}
bool AeroCReceiver::acquired() const { return p_->dem && p_->dem->acquired(); }
bool AeroCReceiver::synced() const { return p_->dem && p_->fr.synced() && p_->dem->acquired(); }
bool AeroCReceiver::dataLock() const { return p_->dem && p_->dem->acquired() && p_->fr.dataLock(); }
double AeroCReceiver::carrierHz() const { return p_->offset + (p_->dem ? p_->dem->freqHz() : 0.0); }
float AeroCReceiver::ebn0Db() const { return p_->dem ? p_->dem->ebn0Db() : 0.f; }
uint64_t AeroCReceiver::frames() const { return p_->nFrames; }
uint64_t AeroCReceiver::samplesFed() const { return p_->fed; }
uint64_t AeroCReceiver::lastFrameSample() const { return p_->lastFrame; }

} // namespace dect2
