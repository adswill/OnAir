// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "audioout_impl.h"
#include "dect2/common/diag.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/audio/clip_ring.h"
#include "dect2/transcribe/audio_tap.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/platform.h"
#include "bitstream_out.h"
#include <thread>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <chrono>
#include <mutex>
#ifdef __linux__
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace dect2 {

// DECT2_MUTE=1: everything plays silence (the device still runs, so the timing that tests and the player rely on is unchanged). The tests
// set it (CMakeLists.txt): the engine tests of FM, DAB, DRM, ATSC and the others otherwise played their test signals out loud.
static const bool kForceMute = [] { const char* e = getenv("DECT2_MUTE"); return e && *e && *e != '0'; }();

static std::atomic<int64_t> gModeWriteMs{INT64_MIN / 4};
static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int AudioOut::msSinceModeWrite() { const int64_t d = nowMs() - gModeWriteMs.load(std::memory_order_relaxed); return d > 1000000000 ? 1000000000 : (int)d; }
std::vector<std::string> AudioOut::devices() { return audioListOutputs(); }
static std::mutex gDefDevMx;
static std::string gDefDev;
void AudioOut::setDefaultDevice(const std::string& name) { std::lock_guard<std::mutex> g(gDefDevMx); gDefDev = name; }
std::string AudioOut::defaultDevice() { std::lock_guard<std::mutex> g(gDefDevMx); return gDefDev; }

// ---- bitstream passthrough: a thread renders the PCM clock (the decoded sound is not played) and sends the IEC 61937 words of the same
// stretch of time to the bitstream device, whose blocking writes pace it. EXPERIMENTAL, untested on hardware.
namespace {
struct BitstreamState {
    std::thread th;
    std::atomic<bool> stop{false}, opened{false}, ok{false};
    std::string err;
    int pcmRate = 48000, linkRate = 48000, kind = 1;
    std::string device;
};
}

static void bitstreamThread(AudioOut::Impl* I, BitstreamState* b) {
    setThreadPriority(ThreadPriority::Realtime);
    std::unique_ptr<BitstreamDevice> dev = bitstreamOpen(b->device, b->linkRate, b->kind, b->err);
    b->ok = dev != nullptr; b->opened = true;
    if (!dev) return;
    const int ratio = std::max(1, b->linkRate / std::max(1, b->pcmRate));   // IEC frames per PCM frame: 1 (AC-3), 4 (E-AC-3)
    const int period = b->kind == 2 ? 6144 : 1536;                          // IEC frames per burst
    const uint32_t block = 256;                                             // PCM frames per step (5.3 ms at 48 kHz)
    std::vector<float> scratch((size_t)block * (size_t)I->channels);
    std::vector<int16_t> out((size_t)block * (size_t)ratio * 2);
    const size_t imask = I->iecFrames - 1;
    while (!b->stop) {
        if (I->iecFlush.exchange(false)) I->ir.store(I->iw.load());
        const uint64_t p0 = I->played.load();
        I->render(scratch.data(), block);
        const uint64_t consumed = I->played.load() - p0;
        // keep the bursts in step with the clock: the words queued should cover what the PCM ring holds; a whole burst is dropped or
        // waited for when they part by more than two and a half bursts (a decode error, a gap filled with PCM silence)
        const int64_t pcmQueued = (int64_t)(I->w.load() - I->r.load());
        uint64_t ird = I->ir.load(std::memory_order_relaxed);
        const uint64_t iwr = I->iw.load(std::memory_order_acquire);
        const int64_t iecQueued = (int64_t)(iwr - ird) / ratio;
        bool hold = false;
        const int64_t slack = (int64_t)period * 5 / (2 * ratio);   // 2.5 bursts: the E-AC-3 packer hands a burst over one frame late
        if (iecQueued - pcmQueued > slack && iwr - ird >= (uint64_t)period) { ird += (uint64_t)period; I->ir.store(ird); }
        else if (pcmQueued - iecQueued > slack) hold = true;
        const size_t want = (size_t)consumed * (size_t)ratio;
        size_t take = hold ? 0 : (size_t)std::min<uint64_t>(want, I->iw.load(std::memory_order_acquire) - ird);
        const bool mute = I->muted.load() || kForceMute;
        for (size_t i = 0; i < take; i++) {
            const size_t k = (size_t)((ird + i) & imask) * 2;
            out[2 * i] = mute ? 0 : I->iec[k]; out[2 * i + 1] = mute ? 0 : I->iec[k + 1];
        }
        I->ir.store(ird + take, std::memory_order_release);
        const size_t total = (size_t)block * (size_t)ratio;
        std::fill(out.begin() + (std::ptrdiff_t)(2 * take), out.begin() + (std::ptrdiff_t)(2 * total), (int16_t)0);   // no burst: zeros, the receiver mutes
        if (!dev->write(out.data(), (int)total)) { DIAG(638654, "The bitstream sound device stopped accepting data, so the compressed audio output ended."); b->err = "the bitstream device stopped"; break; }
    }
    dev.reset();   // closed on this thread (Windows COM)
}

void AudioOut::bitstreamStop(Impl* I) {
    auto* b = static_cast<BitstreamState*>(I->bitstream);
    if (!b) return;
    b->stop = true;
    if (b->th.joinable()) b->th.join();
    delete b;
    I->bitstream = nullptr;
}

bool AudioOut::startBitstream(int pcmRate, int linkRate, int kind, const std::string& device, std::string* err) {
    if (running_) stop();
    if (p_->channels != 2) { p_->channels = 2; p_->ring.assign(p_->frames * 2, 0.f); p_->r.store(p_->w.load()); }
    ch_ = 2;
    rate_ = pcmRate;
    if (p_->iec.empty()) { p_->iecFrames = 1 << 20; p_->iec.assign(p_->iecFrames * 2, 0); }   // 5.4 s at 192 kHz
    p_->ir.store(p_->iw.load());
    auto* b = new BitstreamState;
    b->pcmRate = pcmRate; b->linkRate = linkRate; b->kind = kind; b->device = device;
    b->th = dect2::namedThread("onair-iec", bitstreamThread, p_.get(), b);
    for (int i = 0; i < 500 && !b->opened; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!(b->opened && b->ok)) {
        DIAG(934315, "The bitstream sound device did not open (%s), so the compressed audio is not passed through.", b->opened ? b->err.c_str() : "it took longer than 5 seconds");
        if (err) *err = b->opened ? b->err : "the bitstream device did not open in time";
        b->stop = true;
        if (b->th.joinable()) b->th.join();
        delete b;
        return false;
    }
    p_->bitstream = b;
    running_ = true;
    bitstream_ = true;
    return true;
}

int AudioOut::writeIec(const int16_t* words, int frames) {
    if (p_->iec.empty()) return 0;
    const uint64_t w = p_->iw.load(std::memory_order_relaxed), r = p_->ir.load(std::memory_order_acquire);
    const int space = (int)(p_->iecFrames - (w - r));
    const int n = std::min(frames, space);
    const size_t mask = p_->iecFrames - 1;
    for (int i = 0; i < n; i++) { const size_t k = (size_t)((w + (uint64_t)i) & mask) * 2; p_->iec[k] = words[2 * i]; p_->iec[k + 1] = words[2 * i + 1]; }
    p_->iw.store(w + (uint64_t)n, std::memory_order_release);
    return n;
}

void AudioOut::Impl::render(float* out, uint32_t nFrames) {
    if (flushReq.exchange(false)) { r.store(w.load()); playing = false; leveler.reset(); }
    const uint64_t wr = w.load(std::memory_order_acquire), rd = r.load(std::memory_order_relaxed);
    const uint64_t avail = wr - rd;
    const int C = channels;
    if (!playing) {
        if ((int)avail >= startThreshold.load()) { playing = true; startThreshold = 38400; } // resume after an underrun once 0.8 s are buffered
    }
    uint32_t n = 0;
    if (playing) {
        n = (uint32_t)std::min<uint64_t>(avail, nFrames);
        const float vol = muted.load() || kForceMute ? 0.f : volumeTaper(volume.load());
        const size_t mask = frames - 1;
        for (uint32_t i = 0; i < n; i++) {
            const size_t k = (size_t)((rd + i) & mask) * (size_t)C;
            for (int c = 0; c < C; c++) out[(size_t)C * i + (size_t)c] = ring[k + (size_t)c];
        }
        if (C == 2) leveler.process(out, (int)n);   // the leveler is a stereo one; surround sound keeps the broadcaster's own levels
        for (uint32_t i = 0; i < (uint32_t)C * n; i++) out[i] *= vol;
        r.store(rd + n, std::memory_order_release);
        played.fetch_add(n);
        if (n < nFrames) { underruns++; playing = false; leveler.reset(); }
    }
    if (n < nFrames) memset(out + (size_t)C * n, 0, (size_t)(nFrames - n) * (size_t)C * sizeof(float));
}

AudioOut::AudioOut() : p_(new Impl) {
    p_->frames = 1 << 18; // 262144 frames ~ 5.5 s at 48 kHz
    p_->ring.assign(p_->frames * 2, 0.f);
}
AudioOut::~AudioOut() { stop(); }

bool AudioOut::start(int rate, int channels) {
    channels = channels == 6 || channels == 8 ? channels : 2;
    if (running_ && !bitstream_ && channels == ch_) return true;   // as before: a running output keeps its rate
    if (running_) stop();
    if (channels != p_->channels) { p_->channels = channels; p_->ring.assign(p_->frames * (size_t)channels, 0.f); p_->r.store(p_->w.load()); }
    ch_ = channels;
    rate_ = rate;
    // the device: the one set for this output, else the one chosen for the current mode (audio_control.h); one that has disappeared falls back to the default
    p_->device = actl::audioPickDevice(audioListOutputs(), device_.empty() ? defaultDevice() : device_);
#ifdef __linux__
    // run with sudo, the sound server of the user who ran sudo is not found (issue #67): point PulseAudio/PipeWire at it
    if (geteuid() == 0 && !getenv("PULSE_SERVER") && !getenv("XDG_RUNTIME_DIR")) {
        if (const char* uid = getenv("SUDO_UID"); uid && *uid) {
            const std::string dir = std::string("/run/user/") + uid, sock = dir + "/pulse/native";
            struct stat st{};
            if (stat(sock.c_str(), &st) == 0) {
                setenv("PULSE_SERVER", ("unix:" + sock).c_str(), 0);
                setenv("XDG_RUNTIME_DIR", dir.c_str(), 0);
                fprintf(stderr, "audio: running as root, using the sound server of user %s (%s)\n", uid, sock.c_str());
            }
        }
    }
#endif
    p_->cleanup.prepare(rate);
    p_->control.prepare(rate);
    { const int lat = AudioControlConfig::global().get().latencyMs; if (lat != 300) p_->startThreshold = std::max(480, rate / 1000 * lat); }   // the user's choice; modes that set their own threshold after start() keep it
    p_->vinv.prepare(rate);
    if (!audioBackendStart(p_.get(), rate)) return false;
    running_ = true;
    return true;
}

void AudioOut::stop() {
    if (!running_) return;
    if (bitstream_) bitstreamStop(p_.get());
    else audioBackendStop(p_.get());
    running_ = false;
    bitstream_ = false;
}

int AudioOut::deviceChannels(const std::string& name) { return audioDeviceChannels(name); }

int AudioOut::write(const float* s, int frames) {
    const uint64_t w = p_->w.load(std::memory_order_relaxed), r = p_->r.load(std::memory_order_acquire);
    const int space = (int)(p_->frames - (w - r));
    const int n = std::min(frames, space);
    const size_t mask = p_->frames - 1;
    if (p_->channels != 2) {   // surround: straight into the ring; the voice chain, recorder, clips and transcripts get a stereo fold-down
        const int C = p_->channels;
        float fold[2 * 512];
        for (int off = 0; off < n;) {
            const int m = std::min(n - off, 512);
            for (int i = 0; i < m; i++) {
                const float* f = s + (size_t)C * (size_t)(off + i);
                const size_t k = (size_t)((w + off + i) & mask) * (size_t)C;
                for (int c = 0; c < C; c++) p_->ring[k + (size_t)c] = f[c];
                // ITU-R BS.775 style: L + 0.707 C + 0.707 surrounds, the LFE left out
                float l = f[0] + 0.7071f * f[2], rr = f[1] + 0.7071f * f[2];
                for (int c = 4; c < C; c += 2) { l += 0.7071f * f[c]; rr += 0.7071f * f[c + 1]; }
                fold[2 * i] = l * 0.5f; fold[2 * i + 1] = rr * 0.5f;
            }
            AudioRecorder::offer(this, fold, m, rate_);
            AudioClipRing::offer(this, fold, m, rate_);
            AudioTap::offer(this, fold, m, rate_);
            off += m;
        }
        p_->w.store(w + n, std::memory_order_release);
        p_->written.fetch_add(n);
        if (!monitor_ && !p_->muted.load(std::memory_order_relaxed) && n > 0) gModeWriteMs.store(nowMs(), std::memory_order_relaxed);
        return n;
    }
    const AudioCleanupConfig& cc = AudioCleanupConfig::global();
    const bool clean = !monitor_ && running_;   // the chain returns the input itself while it is off (and fades itself out when switched off)
    for (int off = 0; off < n;) {
        const int m = std::min(n - off, (int)AudioCleanup::kMaxChunk);
        const float* raw = s + 2 * (size_t)off;
        const float* src = raw;
        bool active = false, invActive = false, recOpen = true;
        if (clean) {
            // voice inversion first (analog voice only, off by default), then the cleanup; decoders / clips / transcripts keep `raw`
            if (m <= VoiceInversion::kMaxChunk) src = p_->vinv.process(raw, m, p_->invBuf, &invActive);
            const float* cleaned = p_->cleanup.process(src, m, p_->cleanBuf, &active);
            src = cleaned;
            active = active || invActive;
            // audio control: filters, EQ, notch, AGC, squelch (off: the same pointer back)
            bool ctlActive = false, ctlOpen = true;
            src = p_->control.process(src, m, p_->ctlBuf, &ctlActive, &ctlOpen);
            active = active || ctlActive;
            recOpen = ctlOpen;
        }
        for (int i = 0; i < m; i++) {
            const size_t k = (size_t)((w + off + i) & mask) * 2;
            p_->ring[k] = src[2 * i]; p_->ring[k + 1] = src[2 * i + 1];
        }
        // "Record audio": the raw sound, or the cleaned one when asked; with "skip silence" nothing while the gate holds the sound off
        // with "record only when the squelch is open" nothing is recorded while it is closed
        const bool recGate = !(recOpen || !AudioControlConfig::global().recordOnlyOpen.load(std::memory_order_relaxed));
        if (recGate) {}
        else if (active && cc.recCleaned.load(std::memory_order_relaxed)) {
            if (!(cc.skipSilence.load(std::memory_order_relaxed) && !p_->cleanup.gateOpen())) AudioRecorder::offer(this, src, m, rate_);
        } else AudioRecorder::offer(this, raw, m, rate_);
        AudioClipRing::offer(this, raw, m, rate_);   // radio clips: the last seconds, kept in a ring
        AudioTap::offer(this, raw, m, rate_);        // live transcripts of the modes that have no channel audio of their own
        off += m;
    }
    p_->w.store(w + n, std::memory_order_release);
    p_->written.fetch_add(n);
    if (!monitor_ && !p_->muted.load(std::memory_order_relaxed) && n > 0) gModeWriteMs.store(nowMs(), std::memory_order_relaxed);   // the mode is playing: a monitor yields
    return n;
}

void AudioOut::flush() { p_->flushReq = true; p_->iecFlush = true; }
int AudioOut::bufferedFrames() const { return p_->flushReq.load() ? 0 : (int)(p_->w.load() - p_->r.load()); }   // audio about to be thrown away does not count
uint64_t AudioOut::writtenFrames() const { return p_->written.load(); }
uint64_t AudioOut::playedFrames() const { return p_->played.load(); }
void AudioOut::setVolume(float v) { p_->volume = v; }
void AudioOut::setMuted(bool m) { p_->muted = m; }
int AudioOut::underruns() const { return p_->underruns.load(); }
bool AudioOut::playing() const { return p_->playing.load(); }
void AudioOut::setStartThreshold(int f) { p_->startThreshold = f; }

} // namespace dect2
