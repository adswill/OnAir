// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/record.h"
#include "dect2/common/threadpool.h"
#include "dect2/common/u8file.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/source/iq_record.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace dect2 {
namespace fastscan {

namespace {
double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
int ilog2(int n) { int l = 0; while ((1 << l) < n) l++; return l; }
double nowSteady() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

// ---------------------------------------------------------------- filterbank
bool WolaBank::configure(double rateHz, int m, int p) {
    if (m < 8 || (m & (m - 1)) || m > 4096 || rateHz <= 0) return false;
    rate_ = rateHz; M_ = m; P_ = std::max(4, p); log2m_ = ilog2(m);
    const int L = P_ * M_;
    w_.assign(L, 0.f);
    const double mid = (L - 1) / 2.0, beta = 5.65, i0b = bessel0(beta);
    double sum = 0;
    for (int i = 0; i < L; i++) {
        const double t = 2.0 * (i - mid) / M_;
        const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
        const double r = 2.0 * i / (L - 1) - 1.0;
        const double kai = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        w_[i] = (float)(sinc * kai);
        sum += w_[i];
    }
    for (auto& v : w_) v = (float)(v / sum);
    re_.assign(M_, 0.f); im_.assign(M_, 0.f); fr_.assign(M_, 0.f); fi_.assign(M_, 0.f);
    reset();
    return true;
}

void WolaBank::analyse(const cf32* x) {
    const int M = M_, P = P_;
    for (int r = 0; r < M; r++) { fr_[r] = 0; fi_[r] = 0; }
    for (int p = 0; p < P; p++) {
        const float* w = &w_[(size_t)p * M];
        const cf32* s = x + (size_t)p * M;
        for (int r = 0; r < M; r++) { fr_[r] += w[r] * s[r].real(); fi_[r] += w[r] * s[r].imag(); }
    }
    fftSplit(fr_.data(), fi_.data(), log2m_, false);
    // X[k] * exp(-j 2 pi k n / 4): the block starts nD into the stream, the channel is referred to the stream's time
    const unsigned n4 = (unsigned)(frame_ & 3u);
    for (int k = 0; k < M; k++) {
        const unsigned q = ((unsigned)k * n4) & 3u;
        const float a = fr_[k], b = fi_[k];
        switch (q) {
        case 0: re_[k] = a; im_[k] = b; break;
        case 1: re_[k] = b; im_[k] = -a; break;     // x -j
        case 2: re_[k] = -a; im_[k] = -b; break;
        default: re_[k] = -b; im_[k] = a; break;    // x +j
        }
    }
}

int wolaChannelsFor(double rateHz, double bw) {
    int m = 8;
    while (m * 2 <= 4096 && rateHz / (m * 2) >= 2.0 * std::max(1.0, bw)) m *= 2;
    return m;
}

Demod demodFor(const std::string& k) { return (k == "am" || k.rfind("am_", 0) == 0 || k == "airband") ? Demod::AM : Demod::FM; }

// ---------------------------------------------------------------- tap
void ChannelTap::configure(int bin, double res, double inRate, double bw, Demod d) {
    bin_ = bin; res_ = res; inRate_ = inRate; d_ = d;
    dec_ = std::max(1, (int)std::min(std::floor(inRate / 32000.0), std::floor(inRate / (1.4 * std::max(1.0, bw)))));
    int n = (int)std::lround(3.3 * inRate / std::max(1.0, bw * 0.5));
    n = std::min(255, std::max(15, n)) | 1;
    taps_.assign(n, 0.f);
    const double fc = 0.55 * bw / inRate, mid = (n - 1) / 2.0;
    double s = 0;
    for (int i = 0; i < n; i++) {
        const double t = i - mid;
        const double sinc = std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        taps_[i] = (float)(sinc * (0.54 - 0.46 * std::cos(2 * M_PI * i / (n - 1))));
        s += taps_[i];
    }
    for (auto& v : taps_) v = (float)(v / s);
    step_ = -2.0 * M_PI * res / inRate;
    ph_ = 0; phase_ = 0; hist_.clear(); prev_ = cf32(1, 0); dcAvg_ = 0; level_ = 0;
    const double dev = std::max(1000.0, bw * 0.2);
    fmScale_ = (float)(outRate() / (2.0 * M_PI * dev));
}

void ChannelTap::process(const cf32* z, size_t n, std::vector<int16_t>& audio, std::vector<cf32>& iq) {
    const size_t base = hist_.size();
    hist_.resize(base + n);
    for (size_t i = 0; i < n; i++) {
        const cf32 m(std::cos(ph_), std::sin(ph_));
        hist_[base + i] = z[i] * m;
        ph_ += step_;
        if (ph_ < -M_PI) ph_ += 2 * M_PI;
    }
    const size_t N = taps_.size();
    size_t i = phase_;   // the part of the decimation stride carried over from the last block
    for (; i + N <= hist_.size(); i += (size_t)dec_) {
        float re = 0, im = 0;
        const cf32* h = &hist_[i];
        for (size_t j = 0; j < N; j++) { re += taps_[j] * h[j].real(); im += taps_[j] * h[j].imag(); }
        const cf32 y(re, im);
        if (d_ == Demod::IQ) { iq.push_back(y); continue; }
        float s;
        if (d_ == Demod::FM) {
            const cf32 c = y * std::conj(prev_);
            prev_ = y;
            s = std::atan2(c.imag(), c.real()) * fmScale_;
            dcAvg_ += 0.0005f * (s - dcAvg_);
            s -= dcAvg_;
        } else {
            const float a = std::abs(y);
            dcAvg_ += 0.001f * (a - dcAvg_);
            s = (a - dcAvg_) / std::max(dcAvg_, 1e-6f);
        }
        s = std::max(-1.f, std::min(1.f, s * 0.8f));
        level_ += 0.01f * (std::fabs(s) - level_);
        audio.push_back((int16_t)std::lround(s * 32000.f));
    }
    const size_t used = std::min(i, hist_.size());
    hist_.erase(hist_.begin(), hist_.begin() + (long)used);
    phase_ = i - used;
}

// ---------------------------------------------------------------- guard
void CpuGuard::update(double busy, double span, int active) {
    if (span <= 0) return;
    load_ = 0.8 * load_ + 0.2 * (busy / span);
    if (load_ > maxLoad_) { cap_ = std::max(1, (int)(std::min(cap_, std::max(1, active)) * 0.8)); grow_ = 0; }
    else if (load_ < 0.7 * maxLoad_) { grow_ += span; if (grow_ >= 1.0) { cap_ = std::min(maxCh_, cap_ + 1); grow_ = 0; } }
}

// ---------------------------------------------------------------- files
std::string scanRecName(double freqHz, std::time_t when, const std::string& ext) {
    char b[96];
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &when);
#else
    localtime_r(&when, &tmv);
#endif
    char t[32];
    strftime(t, sizeof t, "%Y%m%d-%H%M%S", &tmv);
    snprintf(b, sizeof b, "scan_%.3fMHz_%s", freqHz / 1e6, t);
    return std::string(b) + (ext.empty() || ext[0] == '.' ? "" : ".") + ext;
}

std::vector<size_t> pickDeletions(const std::vector<RecFileInfo>& f, uint64_t budget, size_t keepNewest) {
    std::vector<size_t> idx(f.size()), out;
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return f[a].mtime != f[b].mtime ? f[a].mtime < f[b].mtime : f[a].path < f[b].path; });
    uint64_t total = 0;
    for (const auto& x : f) total += x.bytes;
    const size_t last = idx.size() > keepNewest ? idx.size() - keepNewest : 0;
    for (size_t k = 0; k < last && total > budget; k++) { out.push_back(idx[k]); total -= f[idx[k]].bytes; }
    return out;
}

uint64_t trimRecordingFolder(const std::string& dir, uint64_t budget) {
    namespace fs = std::filesystem;
    std::vector<RecFileInfo> files;
    std::error_code ec;
    for (fs::directory_iterator it(u8file(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("scan_", 0) != 0 || !it->is_regular_file(ec)) continue;
        RecFileInfo r;
        r.path = it->path().string(); r.bytes = (uint64_t)it->file_size(ec);
        const auto ft = it->last_write_time(ec);
        r.mtime = (std::time_t)std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
        files.push_back(r);
    }
    uint64_t total = 0;
    for (const auto& x : files) total += x.bytes;
    for (size_t i : pickDeletions(files, budget)) { std::error_code e2; if (fs::remove(files[i].path, e2)) total -= files[i].bytes; }
    return total;
}

// ---------------------------------------------------------------- recorder bank
namespace {
void putLe(unsigned char* p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (unsigned char)(v >> (8 * i)); }
void wavHeader(FILE* f, uint32_t rate, uint32_t dataBytes) {
    unsigned char h[44];
    memcpy(h, "RIFF", 4); putLe(h + 4, 36 + dataBytes, 4); memcpy(h + 8, "WAVEfmt ", 8);
    putLe(h + 16, 16, 4); putLe(h + 20, 1, 2); putLe(h + 22, 1, 2); putLe(h + 24, rate, 4); putLe(h + 28, rate * 2, 4); putLe(h + 32, 2, 2); putLe(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); putLe(h + 40, dataBytes, 4);
    fwrite(h, 1, 44, f);
}
}  // namespace

bool RecorderBank::configure(double rateHz, double centerHz, const RecSettings& s) {
    std::lock_guard<std::mutex> g(m_);
    for (auto& r : recs_) finish(*r);
    recs_.clear();
    s_ = s; s_.maxChannels = std::max(1, std::min(100, s.maxChannels));
    rate_ = 0;
    if (!bank_.configure(rateHz, wolaChannelsFor(rateHz, s.chanBwHz))) return false;
    rate_ = rateHz; center_ = centerHz; elapsed_ = 0;
    guard_.configure(s_.maxLoad, s_.maxChannels);
    std::error_code ec;
    if (!s_.dir.empty()) std::filesystem::create_directories(u8file(s_.dir), ec);
    return true;
}

bool RecorderBank::retarget(double centerHz) {
    std::lock_guard<std::mutex> g(m_);
    if (!recs_.empty() || rate_ <= 0) return false;
    center_ = centerHz;
    bank_.reset();
    return true;
}

bool RecorderBank::recording(uint64_t key) const {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& r : recs_) if (r->key == key) return true;
    return false;
}

bool RecorderBank::start(uint64_t key, double freqHz, const std::string& modeKey, std::time_t now) {
    std::lock_guard<std::mutex> g(m_);
    if (rate_ <= 0 || s_.dir.empty()) return false;
    for (const auto& r : recs_) if (r->key == key) return true;
    const double df = freqHz - center_;
    if (std::fabs(df) > rate_ * 0.45 || (int)recs_.size() >= std::min(guard_.cap(), s_.maxChannels)) { refused_++; return false; }
    auto r = std::make_unique<Rec>();
    r->key = key; r->freqHz = freqHz; r->iqMode = s_.iqSlice; r->startedSec = elapsed_;
    const long k = std::lround(df / bank_.binHz());
    const int kk = (int)(((k % bank_.channels()) + bank_.channels()) % bank_.channels());
    r->tap.configure(kk, df - (double)k * bank_.binHz(), bank_.outRate(), s_.chanBwHz, s_.iqSlice ? Demod::IQ : demodFor(modeKey));
    trimRecordingFolder(s_.dir, s_.budgetBytes > (64ull << 20) ? s_.budgetBytes - (64ull << 20) : 0);   // room for what this one will write
    std::string name = scanRecName(freqHz, now, s_.iqSlice ? "cf32" : "wav");
    std::filesystem::path p = u8file(s_.dir) / u8file(name);
    for (int n = 2; std::filesystem::exists(p) && n < 100; n++) p = u8file(s_.dir) / u8file(scanRecName(freqHz, now, "") + "_" + std::to_string(n) + (s_.iqSlice ? ".cf32" : ".wav"));
    r->path = p.string();
    r->f = u8fopen(r->path, "wb");
    if (!r->f) { refused_++; return false; }
    setvbuf(r->f, nullptr, _IOFBF, 1 << 16);
    if (!s_.iqSlice) wavHeader(r->f, (uint32_t)std::lround(r->tap.outRate()), 0);
    started_++;
    recs_.push_back(std::move(r));
    return true;
}

void RecorderBank::finish(Rec& r) {
    if (!r.f) return;
    if (!r.iqMode) {
        fseek(r.f, 0, SEEK_SET);
        wavHeader(r.f, (uint32_t)std::lround(r.tap.outRate()), (uint32_t)std::min<uint64_t>(r.bytes, 0xffffffffu - 64));
    }
    fclose(r.f);
    r.f = nullptr;
    bytesDone_ += r.bytes;
    if (r.iqMode) {
        SigmfInfo i;
        i.fmt = FileFormat::CF32; i.rateHz = r.tap.outRate(); i.centerHz = r.freqHz; i.sampleCount = r.samples;
        const std::string::size_type sl = r.path.find_last_of("/\\");
        i.dataFile = r.path.substr(sl == std::string::npos ? 0 : sl + 1);
        i.datetimeUtc = sigmfUtcNow();
        if (FILE* m = u8fopen(r.path + ".sigmf-meta", "wb")) { const std::string j = sigmfMetaJson(i); fwrite(j.data(), 1, j.size(), m); fclose(m); }
    }
}

void RecorderBank::stop(uint64_t key) {
    std::lock_guard<std::mutex> g(m_);
    for (size_t i = 0; i < recs_.size(); i++) if (recs_[i]->key == key) { finish(*recs_[i]); recs_.erase(recs_.begin() + (long)i); return; }
}

void RecorderBank::closeAll() {
    std::lock_guard<std::mutex> g(m_);
    for (auto& r : recs_) finish(*r);
    recs_.clear();
}

void RecorderBank::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> g(m_);
    if (rate_ <= 0 || n == 0) return;
    if (recs_.empty()) { bank_.reset(); elapsed_ += (double)n / rate_; return; }
    const double t0 = nowSteady();
    bank_.process(x, n, [&](uint64_t, const float* re, const float* im) {
        for (auto& r : recs_) r->in.push_back(cf32(re[r->tap.bin()], im[r->tap.bin()]));
    });
    auto job = [&](size_t i) {
        Rec& r = *recs_[i];
        r.audio.clear(); r.iq.clear();
        r.tap.process(r.in.data(), r.in.size(), r.audio, r.iq);
        r.in.clear();
        if (r.iqMode) { fwrite(r.iq.data(), sizeof(cf32), r.iq.size(), r.f); r.samples += r.iq.size(); r.bytes += r.iq.size() * sizeof(cf32); }
        else { fwrite(r.audio.data(), 2, r.audio.size(), r.f); r.samples += r.audio.size(); r.bytes += r.audio.size() * 2; }
    };
    if (recs_.size() >= 8) ThreadPool::shared().parallelFor(recs_.size(), job);
    else for (size_t i = 0; i < recs_.size(); i++) job(i);
    elapsed_ += (double)n / rate_;
    guard_.update(nowSteady() - t0, (double)n / rate_, (int)recs_.size());
    // over the cap (the guard backed off) or past the length limit: the newest go first / the long ones end
    for (size_t i = recs_.size(); i-- > 0;) {
        const bool tooLong = elapsed_ - recs_[i]->startedSec >= s_.maxSecPerRecording;
        if (tooLong || (int)recs_.size() > guard_.cap()) {
            if (!tooLong) refused_++;
            finish(*recs_[i]); recs_.erase(recs_.begin() + (long)i);
        }
    }
}

RecStats RecorderBank::stats() const {
    std::lock_guard<std::mutex> g(m_);
    RecStats s;
    s.active = (int)recs_.size(); s.cap = std::min(guard_.cap(), s_.maxChannels); s.started = started_; s.refused = refused_;
    s.bytes = bytesDone_;
    for (const auto& r : recs_) s.bytes += r->bytes;
    s.load = guard_.load(); s.throttled = guard_.throttled();
    return s;
}

}  // namespace fastscan
}  // namespace dect2
