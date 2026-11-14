// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/audio/flac_enc.h"
#include "dect2/audio/mp3_enc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "dect2/common/diag.h"

namespace dect2 {

namespace {
constexpr size_t kRingFrames = 1u << 19;   // 10 s at 48 kHz: rides over a disk that stalls for a while
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}  // namespace

struct AudioRecorder::Impl {
    AudioRecOptions o;
    std::string basePath;
    // audio thread -> writer thread
    std::vector<float> ring;
    std::atomic<uint64_t> w{0}, r{0};
    std::atomic<const void*> owner{nullptr};
    std::atomic<int64_t> lastMs{0};
    std::atomic<int> ringRate{0}, wantRate{0};
    std::atomic<uint64_t> dropped{0};
    // writer thread
    std::thread th;
    std::mutex cvMu;
    std::condition_variable cv;
    std::atomic<bool> stopReq{false}, failed{false};
    FILE* f = nullptr;
    int rate = 0, ch = 1, fileN = 0;
    bool stereoLatched = false;
    FlacEnc flac; Mp3Enc mp3;
    uint64_t fileFrames = 0, sinceSync = 0;
    std::vector<float> pend;
    std::vector<int16_t> pcm;
    std::deque<std::vector<float>> pre;
    bool gateOpen = false; double closedFor = 0;
    // stats (any thread)
    std::atomic<uint64_t> frames{0}, closedBytes{0}, liveBytes{0};
    std::atomic<int> files{0}, statRate{0}, statCh{0};
    std::atomic<bool> statGate{false};
    mutable std::mutex statMu;
    std::string path, error;
    std::atomic<bool>* activeFlag = nullptr;

    void fail(const std::string& e) { { std::lock_guard<std::mutex> lk(statMu); if (error.empty()) error = e; } failed = true; activeFlag->store(false); stopReq = true; }

    std::string nthPath(int n) const {
        if (n <= 1) return basePath;
        const size_t dot = basePath.find_last_of('.');
        const size_t sl = basePath.find_last_of("/\\");
        const std::string ins = "_" + std::to_string(n);
        if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return basePath + ins;
        return basePath.substr(0, dot) + ins + basePath.substr(dot);
    }
    void wavHeader() {
        uint8_t h[44] = {};
        const uint32_t data = (uint32_t)std::min<uint64_t>(fileFrames * 2 * (uint64_t)ch, 0xFFFFFFFFu - 40);
        memcpy(h, "RIFF", 4); le32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8); le32(h + 16, 16); le16(h + 20, 1); le16(h + 22, (uint32_t)ch);
        le32(h + 24, (uint32_t)rate); le32(h + 28, (uint32_t)rate * 2 * (uint32_t)ch); le16(h + 32, 2 * (uint32_t)ch); le16(h + 34, 16);
        memcpy(h + 36, "data", 4); le32(h + 40, data);
        fwrite(h, 1, 44, f);
    }
    void openFile() {   // with `pend` holding the first audio of the file
        bool st = stereoLatched;
        if (!st) for (size_t i = 0; i + 1 < pend.size(); i += 2) if (std::fabs(pend[i] - pend[i + 1]) > 1e-3f) { st = true; break; }
        ch = st ? 2 : 1;
        const std::string p = nthPath(++fileN);
        f = fopen(p.c_str(), "wb");
        if (!f) { DIAG(861412, "The audio recording file %s could not be created, so the recording stops.", p.c_str()); fail("cannot write " + p); return; }
        std::vector<std::string> tags;
        if (!o.tag.empty()) tags.push_back("COMMENT=" + o.tag);
        bool ok = true;
        if (o.format == AudioRecFormat::Flac) ok = flac.begin(f, (uint32_t)rate, tags, ch);
        else if (o.format == AudioRecFormat::Mp3) {
            ok = mp3.begin(f, (uint32_t)rate, o.mp3Kbps, tags, ch);
            if (!ok && ch == 2) { ch = 1; ok = mp3.begin(f, (uint32_t)rate, o.mp3Kbps, tags, 1); }   // no interleaved encoder: mono
        } else wavHeader();
        if (!ok) { DIAG(923855, "The audio encoder could not start for %s, so the recording stops.", p.c_str()); fclose(f); f = nullptr; fail("cannot start the encoder"); return; }
        if (ch == 2) stereoLatched = true;
        fileFrames = 0; sinceSync = 0;
        files.fetch_add(1); statCh = ch;
        std::lock_guard<std::mutex> lk(statMu); path = p;
    }
    uint64_t fileSize() { return f ? (uint64_t)std::max<long>(0, ftell(f)) : 0; }
    void sync() {   // keeps the header true and the data on disk, so a crash loses at most a second
        if (!f) return;
        if (o.format == AudioRecFormat::Wav) { fflush(f); fseek(f, 0, SEEK_SET); wavHeader(); fseek(f, 0, SEEK_END); }
        fflush(f);
        sinceSync = 0;
    }
    void closeFile() {
        if (!f && !pend.empty() && !failed.load()) openFile();
        if (!f) { pend.clear(); return; }
        flushPend();
        if (!f) return;
        if (o.format == AudioRecFormat::Flac) flac.finish();
        else if (o.format == AudioRecFormat::Mp3) mp3.finish();
        else { fflush(f); fseek(f, 0, SEEK_SET); wavHeader(); fseek(f, 0, SEEK_END); }
        fflush(f);
        closedBytes.fetch_add(fileSize()); liveBytes = 0;
        fclose(f); f = nullptr;
    }
    void flushPend() {
        if (!f || pend.empty()) return;
        const size_t n = pend.size() / 2;
        pcm.resize(n * (size_t)ch);
        for (size_t i = 0; i < n; i++) {
            auto cv16 = [](float x) { return (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, x)) * 32767.f); };
            if (ch == 2) { pcm[2 * i] = cv16(pend[2 * i]); pcm[2 * i + 1] = cv16(pend[2 * i + 1]); }
            else pcm[i] = cv16(0.5f * (pend[2 * i] + pend[2 * i + 1]));
        }
        pend.clear();
        if (o.format == AudioRecFormat::Flac) flac.write(pcm.data(), n);
        else if (o.format == AudioRecFormat::Mp3) mp3.write(pcm.data(), n);
        else if (fwrite(pcm.data(), 2 * (size_t)ch, n, f) != n) { DIAG(269737, "Writing audio data to the recording failed (the disk may be full), so the recording stops."); fail("the disk is full or cannot be written"); fclose(f); f = nullptr; return; }
        if (ferror(f)) { DIAG(562871, "The audio recording file reported a write error (the disk may be full), so the recording stops."); fail("the disk is full or cannot be written"); return; }
        fileFrames += n; sinceSync += n; frames.fetch_add(n);
        if (sinceSync >= (uint64_t)rate) { sync(); liveBytes = fileSize(); }
        // WAV ends at 4 GB: go on in a new file
        if (o.format == AudioRecFormat::Wav && fileFrames * 2 * (uint64_t)ch > 3500000000ull) { closeFileNoFlush(); }
    }
    void closeFileNoFlush() {
        if (!f) return;
        fflush(f); fseek(f, 0, SEEK_SET); wavHeader(); fseek(f, 0, SEEK_END); fflush(f);
        closedBytes.fetch_add(fileSize()); liveBytes = 0;
        fclose(f); f = nullptr;
    }
    void emit(const std::vector<float>& blk) {
        if (failed.load()) return;
        pend.insert(pend.end(), blk.begin(), blk.end());
        if (!f && pend.size() / 2 >= (size_t)rate / 4) openFile();
        if (f) flushPend();
    }
    void block(const std::vector<float>& blk) {
        const size_t n = blk.size() / 2;
        const double dur = (double)n / rate;
        if (!o.gated) { gateOpen = true; statGate = true; emit(blk); return; }
        double s = 0;
        for (float x : blk) s += (double)x * x;
        const double db = 10 * std::log10(s / (double)blk.size() + 1e-12);
        if (db >= o.gateDb) {
            if (!gateOpen) { gateOpen = true; statGate = true; for (auto& b : pre) emit(b); pre.clear(); }
            closedFor = 0;
            emit(blk);
            return;
        }
        if (gateOpen) {
            closedFor += dur;
            emit(blk);   // the hang
            if (closedFor > o.hangSec) { gateOpen = false; statGate = false; }
            return;
        }
        closedFor += dur;
        pre.push_back(blk);
        while ((double)pre.size() * dur > o.preRollSec) pre.pop_front();
        if (o.split && closedFor > o.splitAfterSec && (f || !pend.empty())) closeFile();
    }
    void run() {
        std::vector<float> blk;
        while (true) {
            const bool stopping = stopReq.load();
            const int want = wantRate.load();
            if (want > 0 && want != ringRate.load()) {   // the output changed its rate (another mode): the file ends here
                closeFile(); pre.clear(); gateOpen = false; closedFor = 0;
                r.store(w.load());
                rate = want; ringRate.store(want); statRate = want;
            }
            const int cur = ringRate.load();
            if (cur > 0) {
                rate = cur;
                const size_t bs = (size_t)std::max(1, rate / 50);
                const size_t mask = kRingFrames - 1;
                while (true) {
                    const uint64_t rd = r.load(), avail = w.load(std::memory_order_acquire) - rd;
                    const size_t n = avail >= bs ? bs : (stopping ? (size_t)avail : 0);
                    if (n == 0) break;
                    blk.resize(n * 2);
                    for (size_t i = 0; i < n; i++) { const size_t k = (size_t)((rd + i) & mask) * 2; blk[2 * i] = ring[k]; blk[2 * i + 1] = ring[k + 1]; }
                    r.store(rd + n, std::memory_order_release);
                    block(blk);
                }
            }
            if (stopping) break;
            std::unique_lock<std::mutex> lk(cvMu);
            cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return stopReq.load(); });
        }
        closeFile();
        activeFlag->store(false);
    }
};

AudioRecorder::AudioRecorder() : p_(new Impl) { p_->activeFlag = &active_; }
AudioRecorder::~AudioRecorder() { stop(); }
AudioRecorder& AudioRecorder::instance() { static AudioRecorder r; return r; }

const char* AudioRecorder::extension(AudioRecFormat f) { return f == AudioRecFormat::Flac ? "flac" : f == AudioRecFormat::Mp3 ? "mp3" : "wav"; }

std::string AudioRecorder::fileName(const std::string& modeId, double freqHz, AudioRecFormat f) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string id = modeId.empty() ? "audio" : modeId;
    for (auto& c : id) if (!isalnum((unsigned char)c)) c = '-';
    char b[200];
    if (freqHz > 0) snprintf(b, sizeof b, "onair_audio_%s_%.3fMHz_%s.%s", id.c_str(), freqHz / 1e6, stamp, extension(f));
    else snprintf(b, sizeof b, "onair_audio_%s_%s.%s", id.c_str(), stamp, extension(f));
    return b;
}

bool AudioRecorder::start(const std::string& path, const AudioRecOptions& o, std::string& err) {
    stop();   // joins a recording that ended by itself
    Impl& s = *p_;
    if (o.format == AudioRecFormat::Mp3 && !Mp3Enc::available()) { DIAG(238129, "MP3 recording was requested but the LAME library is not installed, so the recording did not start."); err = "MP3 needs the LAME library (libmp3lame), which is not installed"; return false; }
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) { DIAG(235054, "The recording folder %s could not be created (%s), so the recording did not start.", parent.string().c_str(), ec.message().c_str()); err = "cannot create " + parent.string() + ": " + ec.message(); return false; }
    s.o = o; s.basePath = path;
    if (s.ring.empty()) s.ring.assign(kRingFrames * 2, 0.f);
    s.w = 0; s.r = 0; s.owner = nullptr; s.lastMs = 0; s.ringRate = 0; s.wantRate = 0; s.dropped = 0;
    s.f = nullptr; s.rate = 0; s.ch = 1; s.fileN = 0; s.stereoLatched = false; s.pend.clear(); s.pre.clear(); s.gateOpen = false; s.closedFor = 0;
    s.failed = false; s.frames = 0; s.closedBytes = 0; s.liveBytes = 0; s.files = 0; s.statRate = 0; s.statCh = 0; s.statGate = !o.gated;
    { std::lock_guard<std::mutex> lk(s.statMu); s.path = path; s.error.clear(); }
    s.stopReq = false;
    active_.store(true, std::memory_order_release);
    s.th = dect2::namedThread("onair-rec", [&s] { s.run(); });
    return true;
}

void AudioRecorder::stop() {
    Impl& s = *p_;
    active_.store(false, std::memory_order_release);
    s.stopReq = true;
    s.cv.notify_all();
    if (s.th.joinable()) s.th.join();
}

AudioRecStats AudioRecorder::stats() const {
    const Impl& s = *p_;
    AudioRecStats st;
    st.active = active_.load();
    st.files = s.files.load(); st.rate = s.statRate.load(); st.channels = s.statCh.load();
    st.seconds = st.rate > 0 ? (double)s.frames.load() / st.rate : 0;
    st.bytes = s.closedBytes.load() + s.liveBytes.load();
    st.gateOpen = s.statGate.load(); st.dropped = s.dropped.load();
    std::lock_guard<std::mutex> lk(s.statMu);
    st.path = s.path; st.error = s.error;
    return st;
}

void AudioRecorder::push(const void* src, const float* stereo, int frames, int rate) {
    Impl& s = *p_;
    const int64_t now = nowMs();
    const void* o = s.owner.load(std::memory_order_relaxed);
    if (o != src) {
        if (o && now - s.lastMs.load(std::memory_order_relaxed) < 2000) return;   // another output is the source
        if (!s.owner.compare_exchange_strong(o, src)) return;
    }
    s.lastMs.store(now, std::memory_order_relaxed);
    int rr = s.ringRate.load(std::memory_order_acquire);
    if (rr == 0 && s.ringRate.compare_exchange_strong(rr, rate)) { s.statRate.store(rate); rr = rate; }   // the first audio sets the rate
    if (rate != rr) { s.wantRate.store(rate); return; }
    const uint64_t w = s.w.load(std::memory_order_relaxed), r = s.r.load(std::memory_order_acquire);
    const size_t space = kRingFrames - (size_t)(w - r);
    const size_t n = std::min<size_t>((size_t)frames, space);
    if (n < (size_t)frames) s.dropped.fetch_add((uint64_t)frames - n, std::memory_order_relaxed);
    const size_t mask = kRingFrames - 1;
    for (size_t i = 0; i < n; i++) { const size_t k = (size_t)((w + i) & mask) * 2; s.ring[k] = stereo[2 * i]; s.ring[k + 1] = stereo[2 * i + 1]; }
    s.w.store(w + n, std::memory_order_release);
}

}  // namespace dect2
