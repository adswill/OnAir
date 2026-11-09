// DRM back end: from equalised cells to messages. Reads the FAC of every transmission frame, the SDC of every super frame, and decodes the MSC
// (time deinterleaver, multilevel decoder, descrambler) into the logical frames of the streams. The OFDM front end (synchronisation, channel
// estimation) hands it one transmission frame of cells at a time.
#pragma once
#include "drm_afs.h"
#include "drm_defs.h"
#include "drm_fec.h"
#include "drm_msg.h"
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

// Equalised cells of one transmission frame: value = received / channel estimate (data cells then have unit average power), weight = |H|^2 / noise power
struct CellGrid {
    int ns = 0, kmin = 0, kmax = 0;
    std::vector<cf32> z;
    std::vector<float> w;
    int width() const { return kmax - kmin + 1; }
    void resize(int ns_, int kmin_, int kmax_) { ns = ns_; kmin = kmin_; kmax = kmax_; z.assign((size_t)ns * (size_t)width(), cf32(0, 0)); w.assign(z.size(), 0.f); }
    cf32 zAt(int s, int k) const { return z[(size_t)s * (size_t)width() + (size_t)(k - kmin)]; }
    float wAt(int s, int k) const { return w[(size_t)s * (size_t)width() + (size_t)(k - kmin)]; }
};

struct LogicalFrame {          // the data of one stream for 400 ms (mode E 100 ms): part A then part B
    int stream = 0;
    int lenA = 0, lenB = 0;
    const uint8_t* data = nullptr;
    uint64_t index = 0;        // multiplex frames decoded so far
    bool afterGap = false;     // frames were lost (or the signal was lost) since the previous logical frame: whatever the stream carries across frames is broken
    int position = 0;          // of the multiplex frame in its transmission super frame (0 .. frames - 1); mode E: an audio super frame starts at the even ones
};

class DrmBackend {
public:
    DrmBackend();
    void reset();                        // forget the signal (a retune); counters keep counting
    void setMode(int mode);              // robustness mode (known from the synchronisation); resets the configuration when it changes
    int mode() const { return mode_; }
    int occupancy() const { return occ_; }          // -1 until the FAC has been read
    // The layout the front end has to use for the next frame: nullptr until the occupancy is known (then use the part of the band that all occupancies share)
    std::shared_ptr<const Layout> layout() const;

    void frame(const CellGrid& g);       // the next transmission frame, in order; its carriers must be those of layout() when that is not null, else any grid that holds the FAC cells
    void frameLost();                    // frames were missed: the sequence of frames is broken
    int framePosition() const { return pos_; }       // position of the next frame in its super frame (0 first), -1 unknown

    std::function<void(const LogicalFrame&)> onLogical;
    std::function<void(const std::string&)> log;
    std::function<void(const FacInfo&)> onFac;

    // state
    bool facValid() const { return facValid_; }
    const FacInfo& fac() const { return fac_; }
    const SdcInfo& sdc() const { return sdc_; }
    const DrmAfs& afs() const { return afs_; }
    bool sdcValid() const { return sdcSeen_; }
    const FacService* facService(int shortId) const { return facSeen_[shortId & 3] ? &facSvc_[shortId & 3] : nullptr; }
    int mscQamBits() const;              // 4 / 16 / 64 cell constellation as bits per cell, 0 unknown
    bool hierarchical() const { return hierarchical_; }
    bool mscReady() const { return mscCode_ != nullptr; }
    uint64_t facOk = 0, facBad = 0, sdcOk = 0, sdcBad = 0, mscOk = 0, mscBad = 0;
    uint64_t mscSkipped = 0;             // multiplex frames not decoded because their cells carry no signal (a fade): counted in mscBad too
    uint64_t facRejected = 0;            // FAC blocks that passed their CRC but disagreed with the frame sequence or the configuration once (not taken)
    std::vector<cf32> facConst, sdcConst, mscConst;

    // A block is not decoded when the mean weight of its cells (their SNR, linear) is below kFadeRel times what it is in the blocks that decode, and
    // below kFadeMaxWeight: a deep fade or no signal. Decoding noise would only fail (or, once in 256 times, pass an 8 bit CRC with garbage). Relative,
    // because in a fast fading channel the noise estimate also holds the change of the channel and all weights are lower.
    static constexpr double kFadeRel = 0.03, kFadeMaxWeight = 2.0;

private:
    struct MuxFrame { std::vector<cf32> z; std::vector<float> w; };
    void decodeFac(const CellGrid& g);
    void decodeSdc();
    void buildMscCode();
    void decodeMux(int txPos);
    void dropData();
    bool fadedBlock(double meanW, double ref) const { return ref > 0 && meanW < kFadeRel * ref && meanW < kFadeMaxWeight; }
    static void learnWeight(double& ref, double meanW) { ref = ref > 0 ? ref + (meanW - ref) * 0.1 : meanW; }
    double wRefFac_ = 0, wRefSdc_ = 0, wRefMux_ = 0;   // mean weight of the blocks that decoded
    int muxDone_ = 0;                    // multiplex frames of the super frame being collected that have been decoded
    FacInfo pendFac_;                    // a changed configuration seen once (a reconfiguration is taken when two FAC blocks in a row agree on it)
    bool pendFacValid_ = false;
    int pendPos_ = -1;                   // a frame position that disagreed with the sequence once
    int announced_ = 0;                  // FAC blocks left in which a changed configuration is expected (the reconfiguration index was set)
    int mode_ = -1, occ_ = -1;
    std::shared_ptr<const Layout> layout_;
    bool facValid_ = false, sdcSeen_ = false, hierarchical_ = false;
    FacInfo fac_;
    FacService facSvc_[4];
    bool facSeen_[4] = {};
    SdcInfo sdc_;
    DrmAfs afs_;
    std::vector<cf32> sdcPrevZ_; std::vector<float> sdcPrevW_; int sdcPrevN_ = 0;   // the earlier blocks, for adding a repeated SDC
    int pos_ = -1;                       // expected position of the next frame in the super frame
    std::unique_ptr<MlcCode> facCode_, sdcCode_, mscCode_;
    int sdcCodeMode_ = -1, mscKey_ = -1;
    // one super frame being collected
    std::vector<cf32> sfZ_, sdcZ_;
    std::vector<float> sfW_, sdcW_;
    int mscFilled_ = 0, framesInSf_ = 0;
    bool sfValid_ = false;
    int mscQam_ = 0;
    std::deque<MuxFrame> hist_;
    uint64_t muxIndex_ = 0;
    int streamA_[4] = {}, streamB_[4] = {}, nStreams_ = 0;
    int depth_ = 1;
    std::vector<uint8_t> lf_;
    bool gap_ = true;
};

}} // namespace dect2::drm
