#include "dect2/iq_record.h"
#include "dect2/mode_tuning.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace dect2 {

std::string recordingModeId(int m) {
    static const char* const base[] = {"dvb", "dvb", "dvb", "atsc", "dab", "atsc3", "isdbt", "fm"};
    if (m >= 0 && m < 8) return base[m];
    if (const ModeTuning* mt = modeTuning(m)) if (*mt->id) return mt->id;
    return "iq";
}

std::string recordingFileName(const std::string& modeId, double centerHz, double rateHz, FileFormat fmt) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string id = modeId.empty() ? "iq" : modeId;
    for (auto& c : id) if (!isalnum((unsigned char)c)) c = '-';
    char b[160];
    snprintf(b, sizeof b, "onair_%s_%.3fMHz_%.9gMsps_%s.%s", id.c_str(), centerHz / 1e6, rateHz / 1e6, stamp, fmt == FileFormat::CF32 ? "cf32" : "cs8");
    return b;
}

bool IqRecorder::start(const std::string& path, FileFormat fmt, double rateHz, std::string& err) {
    stop();
    if (fmt == FileFormat::CU8) { err = "only 8-bit signed and float files can be recorded"; return false; }
    if (rateHz <= 0) { err = "the radio has no sample rate yet"; return false; }
    try {
        const std::filesystem::path p = std::filesystem::u8path(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
#ifdef _WIN32
        file_ = _wfopen(p.c_str(), L"wb");
#else
        file_ = fopen(p.c_str(), "wb");
#endif
    } catch (const std::exception& e) { err = std::string("cannot create the folder: ") + e.what(); return false; }
    if (!file_) { err = "cannot create " + path + ": " + strerror(errno); return false; }
    setvbuf((FILE*)file_, nullptr, _IOFBF, 1 << 20);
    fmt_ = fmt; rate_ = rateHz; path_ = path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear(); qBytes_ = 0; stopReq_ = false;
        samples_ = bytes_ = dropped_ = 0;
        error_.clear();
    }
    active_ = true;
    th_ = std::thread([this] { writerLoop(); });
    return true;
}

void IqRecorder::push(const cf32* x, size_t n) {
    if (!active_.load(std::memory_order_relaxed) || !n) return;
    const size_t bps = fmt_ == FileFormat::CF32 ? 8 : 2;
    std::vector<uint8_t> blk(n * bps);   // converted here: the queue holds the (smaller) file bytes
    if (fmt_ == FileFormat::CF32) memcpy(blk.data(), x, n * sizeof(cf32));
    else {
        auto q = [](float v) { return (uint8_t)(int8_t)std::max(-127L, std::min(127L, lrintf(v * 127.f))); };
        for (size_t i = 0; i < n; i++) { blk[2 * i] = q(x[i].real()); blk[2 * i + 1] = q(x[i].imag()); }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (qBytes_ + blk.size() > kQueueBytes) { dropped_ += n; return; }   // the disk is too slow: leave this block out
        qBytes_ += blk.size();
        bytes_ += blk.size();
        samples_ += n;
        q_.push_back(std::move(blk));
        if (bytes_ >= kMaxBytes) active_ = false;   // safety cap reached: the writer finishes what is queued
    }
    cv_.notify_one();
}

void IqRecorder::writerLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [this] { return !q_.empty() || stopReq_ || !active_.load(); });
        if (q_.empty()) break;   // stopping (or the cap): everything queued has been written
        std::vector<uint8_t> blk = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        const bool ok = fwrite(blk.data(), 1, blk.size(), (FILE*)file_) == blk.size();
        lk.lock();
        qBytes_ -= blk.size();
        if (!ok) {
            error_ = std::string("writing failed: ") + strerror(errno);
            active_ = false;
            q_.clear(); qBytes_ = 0;
            break;
        }
    }
    active_ = false;
}

void IqRecorder::stop() {
    if (!th_.joinable() && !file_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopReq_ = true;
    }
    active_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (file_) { fclose((FILE*)file_); file_ = nullptr; }
}

RecordingStats IqRecorder::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    RecordingStats s;
    s.active = active_.load();
    s.path = path_;
    s.seconds = rate_ > 0 ? samples_ / rate_ : 0;
    s.bytes = bytes_;
    s.droppedSamples = dropped_;
    s.error = error_;
    return s;
}

} // namespace dect2
