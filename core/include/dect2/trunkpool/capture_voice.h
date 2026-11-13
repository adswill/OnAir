// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One wide capture as the trunk pool's radio: every call inside it is a slice decoded from the same samples, no retune (single-radio trunking,
// docs/TRUNK1-3.9.md). Each slice cuts its channel out of the capture (SliceDdc: mix, filter, decimate to about 50 kHz) and runs the voice
// decoder of the call's protocol at that narrow rate:
//   analog FM          Motorola Type II, EDACS, MPT 1327, LTR and anything else not digital (noise squelch, 48 kHz sound)
//   P25 Phase 1        P25 and Motorola ASTRO calls (IMBE needs the user-installed plug-in)
//   P25 Phase 2        the logical channel of the grant (setP25p2System gives the descrambler seed of the site)
//   DMR                Tier III, Capacity Plus, Connect Plus (the time slot of the grant; AMBE+2 needs the user-installed plug-in)
//   NXDN               NXDN calls
// Without the codec a digital call is still heard (its voice frames are counted and keep it alive) and logged, without sound. Encrypted calls
// never reach a slice (the pool does not allocate them). Plain logic on the analysis thread: the host calls feed() and the pool's tick() from the
// same thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/trunkpool/trunkpool.h"
#include <functional>
#include <memory>
#include <string>
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
    // The number of voice slots (calls decoded at once), 1 to 16; open slices above the new count are closed (the pool ends their calls)
    void setSlots(int n);
    int slots() const { return (int)s_.size(); }
    void feed(const cf32* x, size_t n);
    // Every block of decoded sound as it is made, also while the pool drains the slice for its recorder. rate: 48000 analog and DMR, 8000 P25 / NXDN.
    void setAudioSink(std::function<void(int slice, const float* x, size_t n, int rate)> cb) { sink_ = std::move(cb); }
    // The P25 Phase 2 descrambler seed (WACN, system ID, NAC) from the control channel; applied to Phase 2 slices opened after it
    void setP25p2System(int wacn, int sysId, int nac) { p2Seed_[0] = wacn; p2Seed_[1] = sysId; p2Seed_[2] = nac; }
    // Each digital slice reports to its own voice monitor, voice::monitor(prefix + "1"), "2" ...: its call list, the conversation tracker and the
    // monitor's recording of each call. Empty (the default): no monitor, the pool's recorder only.
    void setMonitorPrefix(const std::string& p) { monPrefix_ = p; }
    // DC spike: 0 automatic (a notch on the capture while a slice is within 20 kHz of the centre), 1 never, 2 always
    void setDcMode(int m) { dcMode_ = m; }
    bool dcNotchOn() const { return dcOn_; }

    bool sliceOpen(int slice) const;
    bool sliceDigital(int slice) const;
    double sliceHz(int slice) const;
    Proto sliceProto(int slice) const;
    const P25Receiver* sliceP25(int slice) const;
    uint64_t sliceVoiceFrames(int slice) const;   // digital voice frames heard on it since it opened (analog: 10 ms blocks with a carrier)
    double sliceRateHz(int slice) const;          // the narrow rate its decoder runs at
    // Processing time of the slices against signal time (1 = one core at real time), measured over the samples fed; reset by configure()
    double cpuShare(int slice) const;
    double cpuShareAll() const;

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
    uint64_t fed_ = 0;                    // samples fed since configure (signal time)
    std::function<void(int, const float*, size_t, int)> sink_;
    int p2Seed_[3] = {0, 0, 0};
    std::string monPrefix_;
    int dcMode_ = 0;
    bool dcOn_ = false;
    struct Dc;
    std::unique_ptr<Dc> dc_;
    std::vector<cf32> work_;
    void resetSlice(Slice& x);
    double nowSec() const { return fs_ > 0 ? (double)fed_ / fs_ : 0; }
};

} // namespace tpool
} // namespace dect2
