// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "dect2/common/u8file.h"
#include "dect2/source/iq_record.h"
#include "dect2/source/bluefile.h"
#include "dect2/files/files_proto.h"
#include "dect2/engine/mode_tuning.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace dect2 {

std::string recordingModeId(int m) {
    static const char* const base[] = {"dvb", "dvb", "dvb", "atsc", "dab", "atsc3", "isdbt", "fm"};
    if (m >= 0 && m < 8) return base[m];
    if (const ModeTuning* mt = modeTuning(m)) if (*mt->id) return mt->id;
    return "iq";
}

namespace {
std::mutex gPatMu;
std::string gPattern;
}
void setRecordingNamePattern(const std::string& pattern) { std::lock_guard<std::mutex> lk(gPatMu); gPattern = pattern; }
std::string recordingNamePattern() { std::lock_guard<std::mutex> lk(gPatMu); return gPattern; }

std::string expandRecordingPattern(const std::string& pattern, const std::string& modeId, double centerHz, double rateHz, const std::tm& local, const std::tm& utc) {
    std::string id = modeId.empty() ? "iq" : modeId;
    for (auto& c : id) if (!isalnum((unsigned char)c)) c = '-';
    char b[64];
    std::string o;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] == '{') {
            const size_t e = pattern.find('}', i);
            if (e != std::string::npos) {
                const std::string tok = pattern.substr(i + 1, e - i - 1);
                bool known = true;
                if (tok == "mode") o += id;
                else if (tok == "freq") { snprintf(b, sizeof b, "%.3f", centerHz / 1e6); o += b; }
                else if (tok == "khz") { snprintf(b, sizeof b, "%.0f", centerHz / 1e3); o += b; }
                else if (tok == "rate") { snprintf(b, sizeof b, "%.9g", rateHz / 1e6); o += b; }
                else if (tok == "date") { strftime(b, sizeof b, "%Y%m%d", &local); o += b; }
                else if (tok == "time") { strftime(b, sizeof b, "%H%M%S", &local); o += b; }
                else if (tok == "utcdate") { strftime(b, sizeof b, "%Y%m%d", &utc); o += b; }
                else if (tok == "utctime") { strftime(b, sizeof b, "%H%M%SZ", &utc); o += b; }
                else known = false;
                if (known) { i = e; continue; }
            }
        }
        o += pattern[i];
    }
    for (auto& c : o) if ((unsigned char)c < 32 || strchr("/\\:*?\"<>|", c)) c = '_';
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    if (o.size() > 180) o.resize(180);
    return o;
}

std::string recordingFileName(const std::string& modeId, double centerHz, double rateHz, FileFormat fmt) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{}, ut{};
#ifdef _WIN32
    localtime_s(&tm, &t);
    gmtime_s(&ut, &t);
#else
    localtime_r(&t, &tm);
    gmtime_r(&t, &ut);
#endif
    const char* ext = fmt == FileFormat::CF32 ? "cf32" : fmt == FileFormat::CI16 ? "cs16" : "cs8";
    const std::string pat = recordingNamePattern();
    if (!pat.empty()) {
        const std::string n = expandRecordingPattern(pat, modeId, centerHz, rateHz, tm, ut);
        if (!n.empty()) return n + "." + ext;
    }
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string id = modeId.empty() ? "iq" : modeId;
    for (auto& c : id) if (!isalnum((unsigned char)c)) c = '-';
    char b[160];
    snprintf(b, sizeof b, "onair_%s_%.3fMHz_%.9gMsps_%s.%s", id.c_str(), centerHz / 1e6, rateHz / 1e6, stamp, ext);
    return b;
}

std::string blueRecordingName(const std::string& name) {
    const size_t d = name.find_last_of('.');
    return (d == std::string::npos ? name : name.substr(0, d)) + ".tmp";
}

const char* sigmfDatatype(FileFormat fmt) { return fmt == FileFormat::CF32 ? "cf32_le" : fmt == FileFormat::CI16 ? "ci16_le" : fmt == FileFormat::CU8 ? "cu8" : "ci8_le"; }

int recordingStdMode(const std::string& id) {
    if (id.empty() || id == "iq" || id == "dvb") return -1;   // "dvb" is DVB-T and T2 both: the mode already chosen stays
    static const struct { const char* id; int std; } base[] = {{"atsc", 3}, {"dab", 4}, {"atsc3", 5}, {"isdbt", 6}, {"fm", 7}};
    for (auto& b : base) if (id == b.id) return b.std;
    if (const ModeTuning* mt = modeTuningById(id)) return mt->stdMode;
    return -1;
}

std::string sigmfUtcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string sigmfMetaJson(const SigmfInfo& i) {   // the text: core/src/files/files_proto.cpp (C++) or the Rust crate onair_files
    return filesp::sigmfRecorderMeta(sigmfDatatype(i.fmt), i.rateHz, i.centerHz, i.datetimeUtc, i.dataFile, i.sampleCount, i.description);
}

std::string sigmfAddExtra(const std::string& json, const std::string& hw, bool hasGeo, double lat, double lon) {
    if (hw.empty() && !hasGeo) return json;
    const size_t g = json.find("\"global\"");
    const size_t b = g == std::string::npos ? g : json.find('{', g);
    if (b == std::string::npos) return json;
    std::string add;
    if (!hw.empty()) {
        std::string e;
        for (char ch : hw) { if (ch == '"' || ch == '\\') e += '\\'; if ((unsigned char)ch >= 0x20) e += ch; }
        add += "\"core:hw\":\"" + e + "\"";
    }
    if (hasGeo) {
        char buf[96];
        snprintf(buf, sizeof buf, "\"core:geolocation\":{\"type\":\"Point\",\"coordinates\":[%.6f,%.6f]}", lon, lat);
        if (!add.empty()) add += ",";
        add += buf;
    }
    // a following member needs a comma; an empty object does not
    size_t k = b + 1;
    while (k < json.size() && isspace((unsigned char)json[k])) k++;
    const bool empty = k < json.size() && json[k] == '}';
    return json.substr(0, b + 1) + add + (empty ? "" : ",") + json.substr(b + 1);
}

void IqRecorder::writeSigmf(bool final) {
    if (sigmfPath_.empty()) return;
    SigmfInfo i;
    i.fmt = fmt_; i.rateHz = rate_; i.centerHz = centerHz_; i.datetimeUtc = sigmfWhen_; i.description = sigmfDesc_;
    const std::string::size_type s = path_.find_last_of("/\\");
    i.dataFile = path_.substr(s == std::string::npos ? 0 : s + 1);
    if (final) { std::lock_guard<std::mutex> lk(mu_); i.sampleCount = samples_; }
    FILE* f = nullptr;
    try { f = fopen(dect2::u8file(sigmfPath_).string().c_str(), "wb"); } catch (...) {}
    if (!f) return;
    const std::string j = sigmfAddExtra(sigmfMetaJson(i), sigmfHw_, sigmfGeo_, geoLat_, geoLon_);
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
}

bool IqRecorder::start(const std::string& path, FileFormat fmt, double rateHz, std::string& err) {
    stop();
    if (fmt != FileFormat::CS8 && fmt != FileFormat::CF32 && fmt != FileFormat::CI16) { DIAG_ERROR(198861, "The IQ recording format is not 8-bit, 16-bit or float, so the recording does not start."); err = "only 8-bit, 16-bit and float files can be recorded"; return false; }
    if (rateHz <= 0) { DIAG_ERROR(590239, "The radio has no sample rate yet, so the IQ recording does not start."); err = "the radio has no sample rate yet"; return false; }
    const bool asBlue = blue::isBlueExtension(path);
    if (asBlue) {
        blue::WriteSpec ws;
        ws.complex = true; ws.kind = fmt == FileFormat::CF32 ? 'F' : fmt == FileFormat::CI16 ? 'I' : 'B'; ws.sampleRate = rateHz; ws.centerHz = centerHz_; ws.description = sigmfDesc_;
        auto* w = new blue::Writer;
        if (!w->open(path, ws, err)) { delete w; return false; }
        blue_ = w;
        fmt_ = fmt; rate_ = rateHz; path_ = path;
        { std::lock_guard<std::mutex> lk(mu_); q_.clear(); qBytes_ = 0; stopReq_ = false; samples_ = bytes_ = dropped_ = 0; error_.clear(); }
        sigmfPath_.clear();
        active_ = true;
        th_ = dect2::namedThread("onair-iqrec", [this] { writerLoop(); });
        return true;
    }
    try {
        const std::filesystem::path p = dect2::u8file(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
#ifdef _WIN32
        file_ = _wfopen(p.c_str(), L"wb");
#else
        file_ = fopen(p.c_str(), "wb");
#endif
    } catch (const std::exception& e) { DIAG_ERROR(577367, "The folder for the IQ recording could not be created (%s), so the recording does not start.", e.what()); err = std::string("cannot create the folder: ") + e.what(); return false; }
    if (!file_) { DIAG_ERROR(728619, "The IQ recording file %s could not be created (%s), so the recording does not start.", path.c_str(), strerror(errno)); err = "cannot create " + path + ": " + strerror(errno); return false; }
    setvbuf((FILE*)file_, nullptr, _IOFBF, 1 << 20);
    fmt_ = fmt; rate_ = rateHz; path_ = path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear(); qBytes_ = 0; stopReq_ = false;
        samples_ = bytes_ = dropped_ = 0;
        error_.clear();
    }
    capHit_ = false;
    sigmfPath_.clear();
    if (sigmf_) {
        try {
            const std::filesystem::path p = dect2::u8file(path);
            std::filesystem::path base = p; base.replace_extension("");
            sigmfPath_ = base.string() + ".sigmf-meta";
            sigmfWhen_ = sigmfUtcNow();
            std::error_code ec;   // the same data under the name IQEngine looks for: a hard link, no second copy
            std::filesystem::remove(base.string() + ".sigmf-data", ec);
            std::filesystem::create_hard_link(p, base.string() + ".sigmf-data", ec);
        } catch (...) { sigmfPath_.clear(); }
        writeSigmf(false);
    }
    active_ = true;
    th_ = dect2::namedThread("onair-iqrec", [this] { writerLoop(); });
    return true;
}

void IqRecorder::push(const cf32* x, size_t n) {
    if (!active_.load(std::memory_order_relaxed) || !n) return;
    const size_t bps = fmt_ == FileFormat::CF32 ? 8 : fmt_ == FileFormat::CI16 ? 4 : 2;
    std::vector<uint8_t> blk(n * bps);   // converted here: the queue holds the (smaller) file bytes
    if (fmt_ == FileFormat::CF32) memcpy(blk.data(), x, n * sizeof(cf32));
    else if (fmt_ == FileFormat::CI16) {
        auto q = [](float v) { return (int16_t)std::max(-32767L, std::min(32767L, lrintf(v * 32767.f))); };
        for (size_t i = 0; i < n; i++) { const int16_t a = q(x[i].real()), b = q(x[i].imag()); memcpy(&blk[4 * i], &a, 2); memcpy(&blk[4 * i + 2], &b, 2); }   // little-endian hosts
    } else {
        auto q = [](float v) { return (uint8_t)(int8_t)std::max(-127L, std::min(127L, lrintf(v * 127.f))); };
        for (size_t i = 0; i < n; i++) { blk[2 * i] = q(x[i].real()); blk[2 * i + 1] = q(x[i].imag()); }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopReq_ || !active_.load()) return;   // stop() came while this block was converted: the writer is gone, it would be counted but never written
        if (qBytes_ + blk.size() > kQueueBytes) { dropped_ += n; return; }   // the disk is too slow: leave this block out
        qBytes_ += blk.size();
        bytes_ += blk.size();
        samples_ += n;
        q_.push_back(std::move(blk));
        if (bytes_ >= maxBytes_) { capHit_ = true; active_ = false; }   // safety cap reached: the writer finishes what is queued
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
        std::string werr;
        const bool ok = blue_ ? static_cast<blue::Writer*>(blue_)->writeBytes(blk.data(), blk.size(), werr) : fwrite(blk.data(), 1, blk.size(), (FILE*)file_) == blk.size();
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
    if (!th_.joinable() && !file_ && !blue_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopReq_ = true;
    }
    active_ = false;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (file_) { fclose((FILE*)file_); file_ = nullptr; writeSigmf(true); }
    if (blue_) { auto* w = static_cast<blue::Writer*>(blue_); std::string e; if (!w->close(e)) { std::lock_guard<std::mutex> lk(mu_); error_ = e; } delete w; blue_ = nullptr; }
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
