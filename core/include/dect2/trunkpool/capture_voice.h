// One wide capture as the trunk pool's radio: every call inside it is a slice decoded from the same samples, no retune. Analog calls go through an FM slice, P25
// (Motorola ASTRO / digital Type II) calls through a P25 Phase 1 receiver. IMBE needs the user-installed plug-in (P25 receiver); without it the digital call is heard as a carrier and logged, without sound.
// Plain logic on the analysis thread: the host calls feed() and the pool's tick() from the same thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/trunkpool/trunkpool.h"
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {
class P25Receiver;
namespace tpool {

class CaptureVoice final : public VoiceReceiver {
public:
    explicit CaptureVoice(int slices = 4);
    ~CaptureVoice() override;
    void configure(double rateHz);
    void setCentreHz(double hz) { centre_ = hz; }
    void feed(const cf32* x, size_t n);
    // Every block of decoded sound as it is made, also while the pool drains the slice for its recorder. rate: 48000 analog, 8000 P25.
    void setAudioSink(std::function<void(int slice, const float* x, size_t n, int rate)> cb) { sink_ = std::move(cb); }
    bool sliceOpen(int slice) const;
    bool sliceDigital(int slice) const;
    double sliceHz(int slice) const;
    const P25Receiver* sliceP25(int slice) const;

    std::string name() const override { return "capture"; }
    bool present() const override { return fs_ > 0; }
    double captureCentreHz() const override { return centre_; }
    double captureSpanHz() const override { return fs_ * 0.84; }
    int maxSlices() const override { return (int)s_.size(); }
    bool tune(const TuneRequest&) override { return false; }
    RxState state() const override { return RxState::Locked; }
    bool hearingVoice() const override { return false; }
    void release() override {}
    bool openSlice(const TuneRequest& r, int* slice) override;
    void closeSlice(int slice) override;
    bool sliceHearing(int slice) const override;
    int pullAudio(int slice, float* out, int maxFrames, int* rate) override;

private:
    struct Slice;
    std::vector<std::unique_ptr<Slice>> s_;
    double fs_ = 0, centre_ = 0;
    std::function<void(int, const float*, size_t, int)> sink_;
};

} // namespace tpool
} // namespace dect2
