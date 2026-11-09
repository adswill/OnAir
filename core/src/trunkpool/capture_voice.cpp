// See capture_voice.h.
#include "dect2/trunkpool/capture_voice.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/p25/p25_rx.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace dect2 {
namespace tpool {

namespace {
constexpr double kWork = 48000.0;
constexpr int kNoiseBlock = 480;            // 10 ms
constexpr double kNoiseRmsHz = 3500.0;      // above the speech band: wide FM noise from an empty channel is far above this
constexpr int kHangBlocks = 30;             // 300 ms
constexpr size_t kMaxQueued = 48000 * 5;
}

struct CaptureVoice::Slice {
    bool open = false, digital = false;
    double hz = 0;
    FmFront fm;
    FmBiquad lp, nlp;
    double dc = 0;
    std::vector<float> disc, block;
    std::deque<float> queue;
    int rate = 48000;
    double sum = 0;
    int count = 0, hang = 0;
    bool hearing = false;
    std::unique_ptr<P25Receiver> p25;
};

CaptureVoice::CaptureVoice(int slices) {
    for (int i = 0; i < std::max(1, slices); i++) s_.push_back(std::make_unique<Slice>());
}
CaptureVoice::~CaptureVoice() = default;

void CaptureVoice::configure(double rateHz) {
    fs_ = rateHz;
    for (auto& p : s_) {
        p->open = false; p->p25.reset(); p->queue.clear();
        p->fm.configure(rateHz, kWork, 7500.0);
        p->lp.lowpass(3400.0, kWork, 0.7071);
        p->nlp.lowpass(5000.0, kWork, 0.7071);
    }
}

bool CaptureVoice::openSlice(const TuneRequest& r, int* slice) {
    if (!(fs_ > 0) || !(centre_ > 0)) return false;
    for (size_t i = 0; i < s_.size(); i++) {
        Slice& x = *s_[i];
        if (x.open) continue;
        x.open = true; x.hz = r.freqHz; x.digital = r.proto == Proto::P25p1;
        x.fm.reset(); x.fm.setOffset(r.freqHz - centre_); x.lp.clear(); x.nlp.clear();
        x.dc = 0; x.sum = 0; x.count = 0; x.hang = 0; x.hearing = false; x.queue.clear();
        if (x.digital) {
            x.rate = 8000;
            x.p25 = std::make_unique<P25Receiver>();
            x.p25->setSilent(true);
            x.p25->configure(fs_);
            x.p25->setSignalOffset(r.freqHz - centre_);
            x.p25->reset();
            Slice* sp = &x;
            const int idx = (int)i;
            x.p25->setAudioTap([this, sp, idx](const float* a, size_t n) {
                sp->queue.insert(sp->queue.end(), a, a + n);
                while (sp->queue.size() > kMaxQueued) sp->queue.pop_front();
                if (sink_) sink_(idx, a, n, 8000);
            });
        } else x.rate = 48000;
        *slice = (int)i;
        return true;
    }
    return false;
}

void CaptureVoice::closeSlice(int slice) {
    if (slice < 0 || (size_t)slice >= s_.size()) return;
    Slice& x = *s_[(size_t)slice];
    x.open = false; x.p25.reset(); x.queue.clear(); x.hearing = false;
}

bool CaptureVoice::sliceOpen(int i) const { return i >= 0 && (size_t)i < s_.size() && s_[(size_t)i]->open; }
bool CaptureVoice::sliceDigital(int i) const { return sliceOpen(i) && s_[(size_t)i]->digital; }
double CaptureVoice::sliceHz(int i) const { return sliceOpen(i) ? s_[(size_t)i]->hz : 0; }
const P25Receiver* CaptureVoice::sliceP25(int i) const { return sliceOpen(i) ? s_[(size_t)i]->p25.get() : nullptr; }
bool CaptureVoice::sliceHearing(int i) const { return sliceOpen(i) && s_[(size_t)i]->hearing; }

int CaptureVoice::pullAudio(int i, float* out, int maxFrames, int* rate) {
    if (!sliceOpen(i) || maxFrames <= 0) return 0;
    Slice& x = *s_[(size_t)i];
    const int n = std::min(maxFrames, (int)x.queue.size());
    for (int k = 0; k < n; k++) { out[k] = x.queue.front(); x.queue.pop_front(); }
    if (rate) *rate = x.rate;
    return n;
}

void CaptureVoice::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    for (size_t i = 0; i < s_.size(); i++) {
        Slice& v = *s_[i];
        if (!v.open) continue;
        v.disc.clear();
        v.fm.feed(x, n, v.disc);
        v.block.clear();
        for (float d : v.disc) {
            // noise squelch: what is left of the discriminator above the speech band, over 10 ms. An empty channel gives wide FM noise, a carrier does not.
            const double e = (double)d - v.nlp.run((double)d);
            v.sum += e * e;
            if (++v.count == kNoiseBlock) {
                const double rms = std::sqrt(v.sum / kNoiseBlock);
                v.sum = 0; v.count = 0;
                if (rms < kNoiseRmsHz) { v.hang = kHangBlocks; v.hearing = true; }
                else if (v.hang > 0 && --v.hang == 0) v.hearing = false;
            }
            if (!v.digital) {
                const double a = (double)d / 5000.0;
                v.dc += 0.03 * (a - v.dc);                  // about 240 Hz: takes the subaudible signalling out of the speech
                v.block.push_back(v.hearing ? (float)(0.9 * v.lp.run(a - v.dc)) : 0.f);
            }
        }
        if (v.digital) { if (v.p25) v.p25->feed(x, n); }
        else if (!v.block.empty()) {
            v.queue.insert(v.queue.end(), v.block.begin(), v.block.end());
            while (v.queue.size() > kMaxQueued) v.queue.pop_front();
            if (sink_) sink_((int)i, v.block.data(), v.block.size(), 48000);
        }
    }
}

} // namespace tpool
} // namespace dect2
