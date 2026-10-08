#include "audioout_impl.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace dect2 {

// DECT2_MUTE=1: everything plays silence (the device still runs, so the timing that tests and the player rely on is unchanged). The tests
// set it (CMakeLists.txt): the engine tests of FM, DAB, DRM, ATSC and the others otherwise played their test signals out loud.
static const bool kForceMute = [] { const char* e = getenv("DECT2_MUTE"); return e && *e && *e != '0'; }();

void AudioOut::Impl::render(float* out, uint32_t nFrames) {
    if (flushReq.exchange(false)) { r.store(w.load()); playing = false; }
    const uint64_t wr = w.load(std::memory_order_acquire), rd = r.load(std::memory_order_relaxed);
    const uint64_t avail = wr - rd;
    if (!playing) {
        if ((int)avail >= startThreshold.load()) { playing = true; startThreshold = 38400; } // resume after an underrun once 0.8 s are buffered
    }
    uint32_t n = 0;
    if (playing) {
        n = (uint32_t)std::min<uint64_t>(avail, nFrames);
        const float vol = muted.load() || kForceMute ? 0.f : volume.load();
        const size_t mask = frames - 1;
        for (uint32_t i = 0; i < n; i++) {
            const size_t k = (size_t)((rd + i) & mask) * 2;
            out[2 * i] = ring[k] * vol;
            out[2 * i + 1] = ring[k + 1] * vol;
        }
        r.store(rd + n, std::memory_order_release);
        played.fetch_add(n);
        if (n < nFrames) { underruns++; playing = false; }
    }
    if (n < nFrames) memset(out + 2 * n, 0, (size_t)(nFrames - n) * 2 * sizeof(float));
}

AudioOut::AudioOut() : p_(new Impl) {
    p_->frames = 1 << 18; // 262144 frames ~ 5.5 s at 48 kHz
    p_->ring.assign(p_->frames * 2, 0.f);
}
AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(int rate) {
    if (running_) return true;
    rate_ = rate;
    if (!audioBackendStart(p_.get(), rate)) return false;
    running_ = true;
    return true;
}

void AudioOut::stop() {
    if (!running_) return;
    audioBackendStop(p_.get());
    running_ = false;
}

int AudioOut::write(const float* s, int frames) {
    const uint64_t w = p_->w.load(std::memory_order_relaxed), r = p_->r.load(std::memory_order_acquire);
    const int space = (int)(p_->frames - (w - r));
    const int n = std::min(frames, space);
    const size_t mask = p_->frames - 1;
    for (int i = 0; i < n; i++) {
        const size_t k = (size_t)((w + i) & mask) * 2;
        p_->ring[k] = s[2 * i]; p_->ring[k + 1] = s[2 * i + 1];
    }
    p_->w.store(w + n, std::memory_order_release);
    p_->written.fetch_add(n);
    return n;
}

void AudioOut::flush() { p_->flushReq = true; }
int AudioOut::bufferedFrames() const { return p_->flushReq.load() ? 0 : (int)(p_->w.load() - p_->r.load()); }   // audio about to be thrown away does not count
uint64_t AudioOut::writtenFrames() const { return p_->written.load(); }
uint64_t AudioOut::playedFrames() const { return p_->played.load(); }
void AudioOut::setVolume(float v) { p_->volume = v; }
void AudioOut::setMuted(bool m) { p_->muted = m; }
int AudioOut::underruns() const { return p_->underruns.load(); }
bool AudioOut::playing() const { return p_->playing.load(); }
void AudioOut::setStartThreshold(int f) { p_->startThreshold = f; }

} // namespace dect2
