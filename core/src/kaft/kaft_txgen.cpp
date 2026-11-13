// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT transmit generator: see kaft_txgen.h.
#include "dect2/kaft/kaft_txgen.h"
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_session.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iterator>
#include <numeric>

namespace dect2 {

namespace {

constexpr double kGapSec = 0.02;          // silence around a burst or a control frame
constexpr double kPassGapSec = 0.5;       // silence between two passes
constexpr int kOpenEvery = 8;             // bursts between two repeats of OPEN and START
constexpr float kLevel = 0.35f;           // output amplitude: keeps the peaks of the high QAM profiles clear of the limiter
constexpr size_t kMaxFile = 256ull << 20;

std::string baseOf(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

class KaftGen : public TxGenerator {
public:
    KaftGen(double rate, std::string name, std::vector<uint8_t> data, std::string call, int profile, kaft::Width w, int passes, int session)
        : rate_(rate), name_(std::move(name)), data_(std::move(data)), call_(std::move(call)), profile_(profile), width_(w), passes_(passes) {
        const kaft::WidthInfo& wi = kaft::widthInfo(w);
        const long long fsw = (long long)kaft::kSps * wi.symRate, r = std::llround(rate);
        const long long g = std::gcd(fsw, r);
        up_ = (int)(r / g);
        down_ = (int)(fsw / g);
        hash_ = kaft::hashBytes(0, data_.data(), data_.size());
        session_ = session > 0 ? (uint16_t)session : (uint16_t)(1 + (std::time(nullptr) * 2654435761u) % 65000u);
    }
    double sampleRate() const override { return rate_; }
    bool finished() const override { return done_ && pos_ >= cur_.size(); }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (pos_ >= cur_.size()) {
                if (done_) { std::fill(out + have, out + n, cf32(0.f, 0.f)); return; }
                nextSegment();
                continue;
            }
            const size_t k = std::min(n - have, cur_.size() - pos_);
            std::copy(cur_.begin() + (long)pos_, cur_.begin() + (long)(pos_ + k), out + have);
            pos_ += k;
            have += k;
        }
    }

private:
    void silence(double sec) {
        cur_.assign((size_t)std::max(1.0, sec * rate_), cf32(0.f, 0.f));
        pos_ = 0;
    }
    // a 4 sps waveform of the width to the output rate, with a gap of silence on both sides
    void emit(const std::vector<kaft::cf>& w4) {
        std::vector<kaft::cf> iq = (up_ == 1 && down_ == 1) ? w4 : kaft::resample(w4, up_, down_);
        const size_t gap = (size_t)(kGapSec * rate_);
        cur_.assign(gap + iq.size() + gap, cf32(0.f, 0.f));
        for (size_t i = 0; i < iq.size(); i++) cur_[gap + i] = iq[i] * kLevel;
        pos_ = 0;
    }
    void ctrl(const std::vector<uint8_t>& msg) {
        const kaft::FrameSpec fs = kaft::frameForCtrl(msg, false, session_, ctrlNo_++, width_, call_);
        emit(kaft::buildFrame(fs));
    }
    kaft::OpenMsg openMsg() const {
        kaft::OpenMsg o;
        o.session = session_;
        o.callTx = call_;
        o.callRx = "CQ";
        o.width = (uint8_t)width_;
        o.fileName = name_;
        o.payloadSize = data_.size();
        o.origSize = data_.size();
        o.hashCode = 0;
        o.hash = hash_;
        o.compression = 0;
        o.startProfile = (uint8_t)profile_;
        o.blockSize = (uint16_t)kaft::dataBytesPerBlock(profile_);
        return o;
    }
    void nextSegment() {
        // the control sequence at the start of a pass and before every kOpenEvery-th burst: OPEN, then START
        if (stage_ == 0) { ctrl(kaft::encode(openMsg())); stage_ = 1; return; }
        if (stage_ == 1) { kaft::StartMsg s; s.session = session_; s.profile = (uint8_t)profile_; ctrl(kaft::encode(s)); stage_ = 2; return; }
        if (stage_ == 2) {
            if (offset_ >= data_.size()) { stage_ = 3; nextSegment(); return; }
            if (burstsSincePass_ > 0 && burstsSincePass_ % kOpenEvery == 0 && !reopened_) { reopened_ = true; stage_ = 0; nextSegment(); return; }
            reopened_ = false;
            dataBurst();
            burstsSincePass_++;
            return;
        }
        if (stage_ == 3) {
            kaft::FinishMsg f;
            f.session = session_;
            f.payloadSize = data_.size();
            f.hashCode = 0;
            f.hash = hash_;
            f.origHash = hash_;
            ctrl(kaft::encode(f));
            stage_ = 4;
            return;
        }
        // the pass is over
        pass_++;
        offset_ = 0; stage_ = 0; burstsSincePass_ = 0; reopened_ = false;
        if (pass_ >= passes_) done_ = true;
        silence(kPassGapSec);
    }
    void dataBurst() {
        const size_t cap = (size_t)kaft::dataBytesPerBlock(profile_);
        const int nb = kaft::maxBlocksPerFrame(profile_, width_);
        std::vector<kaft::FrameSpec> frames;
        for (int f = 0; f < kaft::kFramesPerBurst && offset_ < data_.size(); f++) {
            kaft::FrameSpec fs;
            fs.header.width = width_;
            fs.header.profile = profile_;
            fs.header.type = kaft::FrameType::Data;
            fs.header.session = session_;
            fs.header.frameNo = (uint16_t)(((burstNo_ & 0x0FFF) << 4) | (f & 15));
            fs.header.frameInBurst = (uint8_t)(f & 15);
            fs.header.callsign = call_;
            for (int k = 0; k < nb && offset_ < data_.size(); k++) {
                const size_t take = std::min(cap, data_.size() - offset_);
                fs.blocks.push_back(kaft::packDataBlock((uint32_t)offset_, data_.data() + offset_, take));
                offset_ += take;
            }
            frames.push_back(std::move(fs));
        }
        if (!frames.empty()) frames.back().header.lastInBurst = true;
        burstNo_ = (uint16_t)((burstNo_ + 1) & 0x0FFF);
        emit(kaft::buildBurst(frames));
    }

    double rate_;
    std::string name_;
    std::vector<uint8_t> data_;
    std::string call_;
    int profile_;
    kaft::Width width_;
    int passes_;
    int up_ = 1, down_ = 1;
    kaft::Hash32 hash_{};
    uint16_t session_ = 1, ctrlNo_ = 0, burstNo_ = 0;
    int stage_ = 0, pass_ = 0, burstsSincePass_ = 0;
    bool reopened_ = false;
    size_t offset_ = 0;
    bool done_ = false;
    std::vector<cf32> cur_;
    size_t pos_ = 0;
};

} // namespace

double kaftMinRateHz(int width) { return width == 0 ? 1.25e6 : width == 1 ? 0.625e6 : 0.25e6; }

bool kaftFromRequest(const TxRequest& r, std::string& fileName, std::vector<uint8_t>& data, std::string& callsign, int& profile, int& width, int& passes, std::string& err) {
    callsign.clear();
    for (char c : r.callsign) callsign += (char)std::toupper((unsigned char)c);
    while (!callsign.empty() && callsign.back() == ' ') callsign.pop_back();
    if (callsign.empty()) callsign = "ONAIR";
    if (callsign.size() > 6) { err = "KAFT: the callsign is at most 6 characters"; return false; }
    for (char c : callsign) {
        if (!(c == '/' || c == '-' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) { err = "KAFT: the callsign may hold only letters, digits and / - ."; return false; }
    }
    profile = (int)std::lround(r.o("kaft.profile", 2));
    if (profile < 0 || profile >= kaft::kNumProfiles) { err = "KAFT: the profile is 0 to 9"; return false; }
    width = (int)std::lround(r.o("kaft.width", 2));
    if (width < 0 || width >= kaft::kNumWidths) { err = "KAFT: the width is 1 MHz, 500 kHz or 200 kHz"; return false; }
    passes = (int)std::lround(r.o("kaft.repeat", 2));
    if (passes < 1 || passes > 9) { err = "KAFT: the number of passes is 1 to 9"; return false; }
    data.clear();
    if (!r.file.empty()) {
        std::ifstream in(r.file, std::ios::binary);
        if (!in) { err = "KAFT: the file cannot be opened: " + r.file; return false; }
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (data.empty()) { err = "KAFT: the file is empty"; return false; }
        if (data.size() > kMaxFile) { err = "KAFT: a file is at most 256 MiB"; return false; }
        fileName = baseOf(r.file);
    } else {
        const std::string t = "OnAir KAFT demo file.\nA file sent one-way in frames of LDPC blocks, each with its own CRC-32.\n";
        data.assign(t.begin(), t.end());
        fileName = "onair-kaft-demo.txt";
    }
    if (fileName.size() > 90) fileName = fileName.substr(fileName.size() - 90);
    return true;
}

double kaftAirtimeSeconds(size_t fileBytes, int profile, int width, int passes) {
    if (fileBytes == 0 || profile < 0 || profile >= kaft::kNumProfiles || width < 0 || width >= kaft::kNumWidths) return 0;
    const kaft::Width w = (kaft::Width)width;
    const size_t cap = (size_t)kaft::dataBytesPerBlock(profile);
    const size_t nBlocks = (fileBytes + cap - 1) / cap;
    const size_t perFrame = (size_t)std::max(1, kaft::maxBlocksPerFrame(profile, w));
    double data = 0;
    size_t frames = 0;
    for (size_t left = nBlocks; left > 0;) {
        const size_t b = std::min(left, perFrame);
        data += kaft::frameSeconds(w, profile, (int)b);
        left -= b;
        frames++;
    }
    const size_t bursts = (frames + kaft::kFramesPerBurst - 1) / kaft::kFramesPerBurst;
    const double ctl = kaft::ctrlFrameSeconds(w, 150) + 2 * kGapSec;
    const double one = data + (double)bursts * 2 * kGapSec + ctl * (3.0 + 2.0 * (double)(bursts / kOpenEvery + 1)) + kPassGapSec;
    return one * std::max(1, passes);
}

std::unique_ptr<TxGenerator> makeKaftTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    std::string name, call;
    std::vector<uint8_t> data;
    int profile = 2, width = 2, passes = 2;
    if (!kaftFromRequest(r, name, data, call, profile, width, passes, err)) return nullptr;
    if (rateHz < kaftMinRateHz(width) - 1) {
        err = std::string("KAFT at ") + kaft::widthInfo((kaft::Width)width).name + " needs a transmit rate of at least " + std::to_string((int)(kaftMinRateHz(width) / 1000)) + " ksps";
        return nullptr;
    }
    return std::make_unique<KaftGen>(rateHz, name, std::move(data), call, profile, (kaft::Width)width, passes, (int)std::lround(std::max(0.0, std::min(65000.0, r.o("kaft.session", 0)))));
}

} // namespace dect2
