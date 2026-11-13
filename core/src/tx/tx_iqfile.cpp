// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit into an IQ file: see tx_iqfile.h.
#include "dect2/tx/tx_iqfile.h"
#include "dect2/source/iq_record.h"
#include "dect2/source/bluefile.h"
#include "dect2/files/files_proto.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

namespace dect2 {

const char* iqFileFormatName(IqFileFormat f) {
    switch (f) {
    case IqFileFormat::SigmfCf32: return "SigMF cf32 (float)";
    case IqFileFormat::SigmfCi16: return "SigMF ci16 (16-bit)";
    case IqFileFormat::SigmfCs8: return "SigMF ci8 (8-bit)";
    case IqFileFormat::RawCf32: return "Raw .cf32";
    case IqFileFormat::WavIq16: return "WAV, 2 channels (I and Q)";
    case IqFileFormat::BlueCf32: return "X-Midas BLUE cf32 (float)";
    case IqFileFormat::BlueCi16: return "X-Midas BLUE ci16 (16-bit)";
    case IqFileFormat::BlueCs8: return "X-Midas BLUE ci8 (8-bit)";
    }
    return "";
}

const double* iqFileRates(int& count) {
    static const double kR[] = {12000, 48000, 96000, 192000, 250000, 500000, 1000000, 2000000, 2400000, 4000000, 8000000, 10000000, 20000000};
    count = (int)(sizeof kR / sizeof *kR);
    return kR;
}

std::string iqFileBaseName(const std::string& modeId, double centerHz, double rateHz) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char when[32];
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tm);
    char b[256];
    snprintf(b, sizeof b, "onair_tx_%s_%.3fMHz_%.0fsps_%s", modeId.c_str(), centerHz / 1e6, rateHz, when);
    return b;
}

namespace {
bool endsWith(const std::string& s, const char* e) {
    const size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
bool isBlue(IqFileFormat f) { return f == IqFileFormat::BlueCf32 || f == IqFileFormat::BlueCi16 || f == IqFileFormat::BlueCs8; }
bool isSigmf(IqFileFormat f) { return f == IqFileFormat::SigmfCf32 || f == IqFileFormat::SigmfCi16 || f == IqFileFormat::SigmfCs8; }
const char* extOf(IqFileFormat f) { return f == IqFileFormat::RawCf32 ? ".cf32" : f == IqFileFormat::WavIq16 ? ".wav" : isBlue(f) ? ".tmp" : ".sigmf-data"; }
}

std::string iqFilePath(const std::string& folderOrBase, const std::string& baseName, IqFileFormat f) {
    std::string p = folderOrBase;
    bool isDir = !p.empty() && (p.back() == '/' || p.back() == '\\');
    if (!isDir && !p.empty()) { std::error_code ec; isDir = std::filesystem::is_directory(dect2::u8file(p), ec); }
    if (isDir) {
        if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
        p += baseName;
    } else if (p.empty()) p = baseName;
    if (isSigmf(f) && endsWith(p, ".sigmf-meta")) p.resize(p.size() - strlen(".sigmf-meta"));
    if (!endsWith(p, extOf(f))) {
        if (isSigmf(f) && endsWith(p, ".sigmf")) p.resize(p.size() - strlen(".sigmf"));
        p += extOf(f);
    }
    return p;
}

IqFileTxSink::IqFileTxSink(const IqFileTxConfig& c) : cfg_(c) {}
IqFileTxSink::~IqFileTxSink() { close(); }

namespace {
void put32(unsigned char* b, uint32_t v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24); }
void put16(unsigned char* b, uint16_t v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); }
void wavHeader(unsigned char* h, uint32_t rate, uint64_t dataBytes) {
    const uint32_t d = (uint32_t)std::min<uint64_t>(dataBytes, 0xFFFFFFFFull - 36);
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + d); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate); put32(h + 28, rate * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, d);
}
}

void IqFileTxSink::writeMeta(bool final) {
    if (!isSigmf(cfg_.format)) return;
    std::string base = cfg_.path;
    if (endsWith(base, ".sigmf-data")) base.resize(base.size() - strlen(".sigmf-data"));
    const char* dt = cfg_.format == IqFileFormat::SigmfCi16 ? "ci16_le" : cfg_.format == IqFileFormat::SigmfCs8 ? "ci8_le" : "cf32_le";
    const std::string::size_type s = cfg_.path.find_last_of("/\\");
    const std::string dataName = cfg_.path.substr(s == std::string::npos ? 0 : s + 1);
    const std::string j = filesp::sigmfRecorderMeta(dt, rate_, cfg_.centerHz, when_, dataName, final ? samples_.load() : 0, cfg_.description);
    FILE* m = dect2::u8fopen(base + ".sigmf-meta", "wb");
    if (!m) return;
    fwrite(j.data(), 1, j.size(), m);
    fclose(m);
}

bool IqFileTxSink::open(const TxTune& t, std::string& err) {
    close();
    if (!(t.sampleRate > 0)) { err = "the IQ file needs a sample rate"; return false; }
    if (cfg_.path.empty()) { err = "no folder or file name was chosen for the IQ file"; return false; }
    rate_ = t.sampleRate;
    if (cfg_.centerHz == 0) cfg_.centerHz = txRadioHz(t);   // the radio's centre: a mode's signal sits its tuning offset away from it
    try {
        const std::filesystem::path p = dect2::u8file(cfg_.path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    } catch (const std::exception& e) { err = std::string("cannot create the folder: ") + e.what(); return false; }
    if (isBlue(cfg_.format)) {
        blue::WriteSpec ws;
        ws.complex = true; ws.sampleRate = rate_; ws.centerHz = cfg_.centerHz; ws.description = cfg_.description;
        ws.kind = cfg_.format == IqFileFormat::BlueCf32 ? 'F' : cfg_.format == IqFileFormat::BlueCi16 ? 'I' : 'B';
        auto* w = new blue::Writer;
        if (!w->open(cfg_.path, ws, err)) { delete w; return false; }
        blue_ = w;
        samples_ = 0; bytes_ = 512; started_ = false;
        if (cfg_.counters) { cfg_.counters->samples = 0; cfg_.counters->bytes = bytes_.load(); cfg_.counters->rateHz = rate_; }
        return true;
    }
    FILE* f = dect2::u8fopen(cfg_.path, "wb");
    if (!f) { err = "cannot create " + cfg_.path + ": " + strerror(errno); return false; }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    f_ = f;
    samples_ = 0; bytes_ = 0; started_ = false;
    when_ = sigmfUtcNow();
    if (cfg_.format == IqFileFormat::WavIq16) {
        unsigned char h[44];
        wavHeader(h, (uint32_t)std::llround(rate_), 0);
        if (fwrite(h, 1, 44, f) != 44) { err = "cannot write " + cfg_.path; fclose(f); f_ = nullptr; return false; }
        bytes_ = 44;
    }
    if (cfg_.counters) { cfg_.counters->samples = 0; cfg_.counters->bytes = bytes_.load(); cfg_.counters->rateHz = rate_; }
    writeMeta(false);
    return true;
}

bool IqFileTxSink::write(const cf32* x, size_t n, std::string& err) {
    if (blue_) {
        if (n == 0) return true;
        if (cfg_.paced && !started_) { t0_ = std::chrono::steady_clock::now(); started_ = true; }
        if (!static_cast<blue::Writer*>(blue_)->write(x, n, err)) return false;
        samples_ += n;
        bytes_ += n * (cfg_.format == IqFileFormat::BlueCf32 ? 8 : cfg_.format == IqFileFormat::BlueCi16 ? 4 : 2);
        if (cfg_.counters) { cfg_.counters->samples = samples_.load(); cfg_.counters->bytes = bytes_.load(); }
        if (cfg_.paced) std::this_thread::sleep_until(t0_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((double)samples_.load() / rate_)));
        return true;
    }
    FILE* f = (FILE*)f_;
    if (!f) { err = "the IQ file is closed"; return false; }
    if (n == 0) return true;
    if (cfg_.paced && !started_) { t0_ = std::chrono::steady_clock::now(); started_ = true; }
    std::vector<unsigned char> buf;
    const unsigned char* data = nullptr;
    size_t bytes = 0;
    switch (cfg_.format) {
    case IqFileFormat::SigmfCf32: case IqFileFormat::RawCf32:
        data = (const unsigned char*)x; bytes = n * sizeof(cf32);
        break;
    case IqFileFormat::SigmfCi16: case IqFileFormat::WavIq16: {
        buf.resize(n * 4);
        for (size_t i = 0; i < n; i++) {
            const int re = (int)std::lrintf(std::max(-1.f, std::min(1.f, x[i].real())) * 32767.f);
            const int im = (int)std::lrintf(std::max(-1.f, std::min(1.f, x[i].imag())) * 32767.f);
            put16(&buf[i * 4], (uint16_t)(int16_t)re); put16(&buf[i * 4 + 2], (uint16_t)(int16_t)im);
        }
        data = buf.data(); bytes = buf.size();
        break;
    }
    case IqFileFormat::SigmfCs8:
        buf.resize(n * 2);
        for (size_t i = 0; i < n; i++) {
            buf[i * 2] = (unsigned char)(int8_t)std::lrintf(std::max(-1.f, std::min(1.f, x[i].real())) * 127.f);
            buf[i * 2 + 1] = (unsigned char)(int8_t)std::lrintf(std::max(-1.f, std::min(1.f, x[i].imag())) * 127.f);
        }
        data = buf.data(); bytes = buf.size();
        break;
    }
    if (cfg_.format == IqFileFormat::WavIq16 && bytes_.load() + bytes > 0xFFFFFFFFull - 36) { err = "a WAV file cannot be larger than 4 GB: choose SigMF or raw for a longer transmission"; return false; }
    if (fwrite(data, 1, bytes, f) != bytes) { err = std::string("cannot write ") + cfg_.path + ": " + strerror(errno); return false; }
    bytes_ += bytes;
    samples_ += n;
    if (cfg_.counters) { cfg_.counters->samples = samples_.load(); cfg_.counters->bytes = bytes_.load(); }
    if (cfg_.paced) {   // the signal's own speed: the time of the samples written so far
        const auto due = t0_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((double)samples_.load() / rate_));
        std::this_thread::sleep_until(due);
    }
    return true;
}

void IqFileTxSink::close() {
    if (blue_) { auto* w = static_cast<blue::Writer*>(blue_); blue_ = nullptr; std::string e; w->close(e); delete w; return; }
    FILE* f = (FILE*)f_;
    if (!f) return;
    f_ = nullptr;
    if (cfg_.format == IqFileFormat::WavIq16) {
        unsigned char h[44];
        wavHeader(h, (uint32_t)std::llround(rate_), samples_.load() * 4);
        fflush(f);
        if (fseek(f, 0, SEEK_SET) == 0) fwrite(h, 1, 44, f);
    }
    fclose(f);
    writeMeta(true);
}

std::unique_ptr<IqFileTxSink> makeIqFileTxSink(const IqFileTxConfig& c) { return std::make_unique<IqFileTxSink>(c); }

} // namespace dect2
