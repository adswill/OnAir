// Voice segmentation: cuts a stream of voice audio into transmissions for the speech engine. A transmission starts when the level passes the
// gate (an absolute floor, or the noise floor the segmenter tracks plus a margin, whichever is higher), keeps a pre-roll in front of it and a
// hang behind it, and ends when the level has been under the gate for the hang time. Airband audio reaches it already gated by the squelch
// (silence between transmissions), other modes' sound has a noise floor: both work. The finished transmission is brought to 16 kHz mono and
// to a steady level, which is what the speech engine wants. Never blocks and never allocates a lot: a few vectors per transmission.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace dect2 {

struct VoiceSegment {
    std::vector<float> pcm16k;   // mono, 16 kHz, level-normalised, with the pre-roll in front and a short tail
    double durSec = 0;           // of the voiced part (pre-roll and hang not counted)
    float levelDb = -120;        // mean level of the voiced part before normalising, dBFS
    int64_t startMs = 0;         // wall clock (ms since the Unix epoch) of the start of the voiced part
    bool cut = false;            // ended because it reached the longest length, not because the speaker stopped
    bool music = false;          // judged to be music or a steady tone, not speech (only when SegmenterOptions::rejectMusic is on)
    float syllabic = 0, stdDb = 0;   // what the judgement was made of (see speechEnvelopeFeatures)
};

struct SegmenterOptions {
    double gateDb = -52;         // block level that always counts as quiet
    double marginDb = 9;         // above the tracked noise floor a block counts as voice
    double preRollSec = 0.35, hangSec = 0.7;
    double minSec = 0.5;         // shorter voiced parts are clicks, not speech
    double maxSec = 27;          // the speech engine reads 30 s at a time
    bool rejectMusic = false;    // mark transmissions whose level has no speech rhythm (music, tones, hum) so the caller can skip them
    double musicMinSec = 1.5;    // shorter parts are not judged
};

class VoiceSegmenter {
public:
    explicit VoiceSegmenter(int inRate = 8000, const SegmenterOptions& o = {});
    // x: mono samples at inRate; wallMs: the wall clock at the end of x. `out` gets every transmission that ended in this call.
    void push(const float* x, size_t n, int64_t wallMs, const std::function<void(VoiceSegment&&)>& out);
    void flush(int64_t wallMs, const std::function<void(VoiceSegment&&)>& out);   // the source went away: end what is going on
    bool active() const { return active_; }   // a transmission is going on now
    float levelDb() const { return lastDb_; }

private:
    void block(const float* x, size_t n, int64_t endMs, const std::function<void(VoiceSegment&&)>& out);
    void finish(int64_t endMs, bool cut, const std::function<void(VoiceSegment&&)>& out);
    int rate_;
    SegmenterOptions o_;
    size_t blk_;                          // samples per 20 ms block
    std::vector<float> part_;             // the block being filled
    std::vector<float> pre_;              // the last pre-roll samples while idle
    std::vector<float> seg_;              // the transmission so far
    bool active_ = false;
    size_t voicedBlocks_ = 0, quietRun_ = 0;
    double sumPow_ = 0;
    double floorDb_ = -80;
    std::deque<float> recent_;            // the last 100 block levels (dB), for the noise floor
    float lastDb_ = -120;
    int64_t startMs_ = 0;
    std::vector<float> env_;              // block levels (dB) of the transmission so far, for the music judgement
};

// Speech has a syllable rhythm: its level (20 ms blocks, dB) rises and falls 2 to 8 times a second by 10 dB or more. Music and tones do not.
// syllabic: share of the level's variation (0.5..25 Hz) that lies in 2..8 Hz; stdDb: spread of the level. blocks at 50 per second.
void speechEnvelopeFeatures(const std::vector<float>& envDb, float& syllabic, float& stdDb);
bool looksLikeSpeech(float syllabic, float stdDb);

// A linear-phase windowed-sinc resampler for a whole buffer (mono). Used by the segmenter; public for the tests.
std::vector<float> resampleMono(const float* x, size_t n, int inRate, int outRate);

}  // namespace dect2
