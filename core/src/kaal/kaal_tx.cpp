// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice transmitter, streaming port of kaal_tx.py.
#include "dect2/kaal/kaal_tx.h"
#include "dect2/kaal/kaal_codec.h"
#include "dect2/kaal/kaal_codec3.h"
#include "dect2/kaal/kaal_fec.h"
#include "dect2/kaal/kaal_phy.h"
#include "dect2/kaal/kaal_sweepid.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>

namespace dect2 {
namespace kaal {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kChunk = 320;                 // codec samples per frame
constexpr int kHistSyms = kRrcSpan + 1;        // symbols that overlap one output sample (49 taps / 6)

// Streaming windowed-sinc resampler for complex samples (48 kHz in, outRate out).
class Resampler {
public:
    void configure(double outRate, double inRate = 48000.0) {
        ratio_ = inRate / outRate;
        fc_ = 0.97 * std::min(1.0, outRate / inRate);
        // the kernel at kPhases fractional positions (nearest phase: a timing error of at most 1 / 2048 of an input sample, below -70 dB for this signal)
        tab_.assign((size_t)(kPhases + 1) * 2 * kH, 0.f);
        for (int p = 0; p <= kPhases; p++) {
            const double f = (double)p / kPhases;
            for (int j = 0; j < 2 * kH; j++) {
                const double x = f - (double)(j - kH + 1);   // pos - i with i = i0 - kH + 1 + j and pos = i0 + f
                const double a = fc_ * x;
                const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                const double w = 0.5 + 0.5 * std::cos(kPi * x / kH);
                tab_[(size_t)p * 2 * kH + (size_t)j] = (float)(fc_ * sinc * w);
            }
        }
        reset();
    }
    void reset() { buf_.assign(kH, cf(0, 0)); base_ = -kH; pos_ = 0; }
    void process(const cf* in, size_t n, std::vector<cf>& out) {
        buf_.insert(buf_.end(), in, in + n);
        while (true) {
            const int64_t i0 = (int64_t)std::floor(pos_);
            int p = (int)std::lround((pos_ - (double)i0) * kPhases);
            int64_t ii = i0;
            if (p >= kPhases) { p = 0; ii++; }
            // the kernel reads buf_ up to ii + kH: checked after the phase rounded up to the next sample (it read one past the end, ASan)
            if (ii + kH >= base_ + (int64_t)buf_.size()) break;
            const float* k = &tab_[(size_t)p * 2 * kH];
            const cf* v = &buf_[(size_t)(ii - kH + 1 - base_)];
            float ar = 0, ai = 0;
            for (int j = 0; j < 2 * kH; j++) { ar += k[j] * v[j].real(); ai += k[j] * v[j].imag(); }
            out.emplace_back(ar, ai);
            pos_ += ratio_;
        }
        const int64_t keep = (int64_t)std::floor(pos_) - kH;
        if (keep - base_ > 4096) {
            buf_.erase(buf_.begin(), buf_.begin() + (keep - base_));
            base_ = keep;
        }
    }
    void finish(std::vector<cf>& out) {
        const std::vector<cf> z(2 * kH, cf(0, 0));
        process(z.data(), z.size(), out);
        reset();
    }
private:
    static constexpr int kH = 16;        // kernel half width in input samples
    static constexpr int kPhases = 2048;
    double ratio_ = 1, fc_ = 0.97, pos_ = 0;
    int64_t base_ = -kH;
    std::vector<cf> buf_;
    std::vector<float> tab_;
};
}  // namespace

double kaalAirTimeRatio(int kbit) {
    if (kbit == 12 || kbit == 16) return 1.0;
    if (kbit == 24 || kbit == 32) return 1.0;   // two frames per codec frame, but at 16 ksym/s: 10 ms each
    return (double)kFrameSymbols / (double)kRtFrameSymbols;   // legacy: 202 / 160 symbols per 20 ms
}

struct VoiceTx::Impl {
    int bitrate = 0;                        // 0 = legacy v0.1, else codec v3 at that kbit/s
    KaalCodec3 codec3;
    std::vector<std::vector<uint8_t>> words;   // v3: coded 480-bit words waiting for a 4-word block
    std::array<uint8_t, kIdCycle> idSyms{};    // v3: the sweep variant of every period of the ID cycle (all 0 = plain up sweep)
    double outRate = 48000;
    // 24 and 32 kbit/s go out at 16 ksym/s: the stream made here (48 kHz, 6 samples per symbol) is the same one played at twice the speed, so its
    // notional rate is 96 kHz and the output is about 20 kHz wide
    double inRate = 48000;
    void applyRate() {
        inRate = bitrate >= 24 ? 96000.0 : 48000.0;
        resample = std::fabs(outRate - inRate) > 1e-6;
        if (resample) rs.configure(outRate, inRate);
    }
    bool resample = false;
    Resampler rs;
    KaalCodec codec;
    std::vector<double> rrc;

    // audio -> frames
    std::vector<int16_t> pcmQ;              // not yet encoded, < 320 after every push
    std::vector<int16_t> prev;              // the previous 320 samples (zeros at the start)
    std::vector<std::vector<uint8_t>> frames;   // info frames of the current 4-frame block
    bool started = false;                   // at least one codec frame was made in this stream

    // symbols -> samples
    cf hist[kHistSyms];                     // hist[0] newest
    uint64_t sampleIdx = 0;                 // output sample index at 48 kHz within the stream
    double sweepAcc = 0;

    // beacon
    bool beaconOn = false;
    BeaconFields bf;
    std::string btext;
    std::vector<cf> sliceFull, sliceHdr, cur;
    size_t curPos = 0;

    std::vector<cf> out;                    // ready samples (at the output rate)
    size_t head = 0;

    Impl() { rrc = rrcTaps(); reset(); makeSlices(); }

    void reset() {
        pcmQ.clear();
        prev.assign(kChunk, 0);
        frames.clear();
        words.clear();
        started = false;
        for (cf& h : hist) h = cf(0, 0);
        sampleIdx = 0;
        sweepAcc = 0;
        cur.clear();
        curPos = 0;
    }

    void makeSlices() {
        sliceFull = beaconSliceModulate(bf, btext, kFs);
        sliceHdr = beaconSliceModulate(bf, std::string(), kFs);
        if (btext.empty()) sliceFull = sliceHdr;
    }

    void encodeWindow(const int16_t* cur320) {   // frame for samples [prev, cur320)
        int16_t w[2 * kChunk];
        std::memcpy(w, prev.data(), kChunk * sizeof(int16_t));
        std::memcpy(w + kChunk, cur320, kChunk * sizeof(int16_t));
        std::memcpy(prev.data(), cur320, kChunk * sizeof(int16_t));
        started = true;
        if (bitrate) {
            const std::vector<uint8_t> bits = codec3.encode(w, 2 * kChunk);   // 3 frames; the middle one covers exactly [prev, cur320)
            const size_t fb = codec3.frameBits();
            const int k = (bitrate == 12 || bitrate == 24) ? 240 : 320;
            for (size_t o = 0; o < fb; o += (size_t)k) {   // one 160-symbol frame per k information bits
                std::vector<uint8_t> word(kRtCodedBits);
                ldpcEncodeRm(&bits[fb + o], k, word.data());
                words.push_back(std::move(word));
            }
            while (words.size() >= (size_t)kRtBlockFrames) encodeBlockRt();
            return;
        }
        const std::vector<uint8_t> bits = codec.encode(w, 2 * kChunk);   // 3 frames; the middle one covers exactly [prev, cur320)
        frames.emplace_back(bits.begin() + KaalCodec::kBits, bits.begin() + 2 * KaalCodec::kBits);
        if (frames.size() == (size_t)4) encodeBlock();
    }

    // v3: 4 coded words -> interleave over 1920 bits -> 4 frames of 160 symbols
    void encodeBlockRt() {
        std::vector<uint8_t> coded;
        coded.reserve((size_t)kRtBlockFrames * kRtCodedBits);
        for (int f = 0; f < kRtBlockFrames; f++) coded.insert(coded.end(), words[(size_t)f].begin(), words[(size_t)f].end());
        words.erase(words.begin(), words.begin() + kRtBlockFrames);
        const std::vector<uint8_t> il = interleave(coded);
        const cf pil = pilotSymbol();
        const std::vector<cf> pre = preamble();
        for (int f = 0; f < kRtBlockFrames; f++) {
            const std::vector<cf> data = qam16Map(&il[(size_t)f * kRtCodedBits], kRtCodedBits);
            for (const cf& sy : pre) emitSymbol(sy);
            for (size_t j = 0; j < data.size(); j += kPilotEvery - 1) {
                const size_t m = std::min<size_t>(kPilotEvery - 1, data.size() - j);
                for (size_t q = 0; q < m; q++) emitSymbol(data[j + q]);
                if (m == (size_t)(kPilotEvery - 1)) emitSymbol(pil);
            }
        }
    }

    void encodeBlock() {
        std::vector<uint8_t> coded(4 * kCodedBits);
        for (int f = 0; f < 4; f++) ldpcEncode(frames[f].data(), &coded[f * kCodedBits]);
        frames.clear();
        const std::vector<uint8_t> il = interleave(coded);
        const cf pil = pilotSymbol();
        const std::vector<cf> pre = preamble();
        for (int f = 0; f < 4; f++) {
            const std::vector<cf> data = qam16Map(&il[f * kCodedBits], kCodedBits);
            for (const cf& s : pre) emitSymbol(s);
            for (size_t j = 0; j < data.size(); j += kPilotEvery - 1) {
                const size_t m = std::min<size_t>(kPilotEvery - 1, data.size() - j);
                for (size_t k = 0; k < m; k++) emitSymbol(data[j + k]);
                if (m == (size_t)(kPilotEvery - 1)) emitSymbol(pil);
            }
        }
    }

    void startBeaconIfDue() {
        if (!beaconOn || curPos < cur.size()) return;
        const uint64_t full = (uint64_t)(kBeaconFullPeriodS * kFs), hdr = (uint64_t)(kBeaconHeaderPeriodS * kFs);
        if (sampleIdx % full == 0) { cur = sliceFull; curPos = 0; }
        else if (sampleIdx % hdr == 0) { cur = sliceHdr; curPos = 0; }
    }

    void emitSymbol(const cf& s) {
        for (int m = kHistSyms - 1; m > 0; m--) hist[m] = hist[m - 1];
        hist[0] = s;
        const float beaconAmp = (float)std::sqrt((bitrate ? kTxDataPowerRt : kTxDataPower) * std::pow(10.0, kTxBeaconDb / 10.0));
        const double dp = bitrate ? kTxDataPowerRt : kTxDataPower;
        const float sweepAmp = (float)std::sqrt(dp * std::pow(10.0, kSweepPowerDb / 10.0));
        cf blk[kSps];
        for (int q = 0; q < kSps; q++) {
            double ar = 0, ai = 0;
            for (int m = 0; m < kHistSyms; m++) {
                const int ti = m * kSps + q;
                if (ti >= (int)rrc.size()) break;
                ar += hist[m].real() * rrc[ti];
                ai += hist[m].imag() * rrc[ti];
            }
            // sweep signature: phase-continuous chirp from the start of the stream, as kaal_phy.sweep_template
            double sr, si;
            if (bitrate) {   // v3: one of four variants per period of 24240 samples, the phase restarts at every period
                const uint64_t per = sampleIdx / (uint64_t)kSwL;
                const std::complex<double> z = sweepSampleAt(idSyms[(size_t)(per % kIdCycle)], (int)(sampleIdx % (uint64_t)kSwL));
                sr = ar + sweepAmp * z.real();
                si = ai + sweepAmp * z.imag();
            } else {
                const double t = (double)sampleIdx / kFs;
                const double ph = std::fmod(t, kSweepPeriodS);
                sweepAcc += kSweepLowHz + (kSweepHighHz - kSweepLowHz) * (ph / kSweepPeriodS);
                if (sweepAcc > 1e12) sweepAcc = std::fmod(sweepAcc, (double)kFs);
                const double a = 2 * kPi * sweepAcc / kFs;
                sr = ar + sweepAmp * std::cos(a);
                si = ai + sweepAmp * std::sin(a);
            }
            startBeaconIfDue();
            if (curPos < cur.size()) {
                sr += beaconAmp * cur[curPos].real();
                si += beaconAmp * cur[curPos].imag();
                curPos++;
            }
            blk[q] = cf((float)(sr * kTxGain), (float)(si * kTxGain));
            sampleIdx++;
        }
        if (resample) rs.process(blk, kSps, out);
        else out.insert(out.end(), blk, blk + kSps);
    }

    void pushAudio(const int16_t* pcm, size_t n) {
        pcmQ.insert(pcmQ.end(), pcm, pcm + n);
        size_t used = 0;
        while (pcmQ.size() - used >= kChunk) { encodeWindow(&pcmQ[used]); used += kChunk; }
        pcmQ.erase(pcmQ.begin(), pcmQ.begin() + used);
    }

    void flush() {
        if (!pcmQ.empty()) {
            pcmQ.resize(kChunk, 0);
            encodeWindow(pcmQ.data());
            pcmQ.clear();
        }
        if (started) {
            const std::vector<int16_t> z(kChunk, 0);
            encodeWindow(z.data());   // the last half-overlapped frame, as the whole-signal encoder adds
            if (bitrate) {
                for (int guard = 0; !words.empty() && guard < 8; guard++) encodeWindow(z.data());   // fill the block with silent frames
                words.clear();
            }
            while (!frames.empty()) {   // fill the 4-frame block with all-zero info frames
                frames.emplace_back(KaalCodec::kBits, (uint8_t)0);
                if (frames.size() == (size_t)4) encodeBlock();
            }
            for (int i = 0; i < kRrcSpan; i++) emitSymbol(cf(0, 0));   // the filter tail
            if (resample) rs.finish(out);
        }
        reset();
    }
};

VoiceTx::VoiceTx() : p_(new Impl) {}
VoiceTx::~VoiceTx() { delete p_; }

void VoiceTx::configure(double sampleRate) {
    p_->outRate = sampleRate > 0 ? sampleRate : 48000.0;
    p_->applyRate();
    p_->reset();
    p_->out.clear();
    p_->head = 0;
}
void VoiceTx::pushAudio(const int16_t* pcm, size_t n) { p_->pushAudio(pcm, n); }
void VoiceTx::flush() { p_->flush(); }
size_t VoiceTx::available() const { return p_->out.size() - p_->head; }
size_t VoiceTx::popIq(std::complex<float>* out, size_t maxSamples) {
    const size_t n = std::min(maxSamples, p_->out.size() - p_->head);
    if (n) std::memcpy(out, &p_->out[p_->head], n * sizeof(cf));
    p_->head += n;
    if (p_->head == p_->out.size()) { p_->out.clear(); p_->head = 0; }
    else if (p_->head > 65536) { p_->out.erase(p_->out.begin(), p_->out.begin() + p_->head); p_->head = 0; }
    return n;
}
bool VoiceTx::setBitrate(int kbit) {
    if (kbit != 0 && !KaalCodec3::validBitrate(kbit)) return false;
    p_->bitrate = kbit;
    if (kbit) p_->codec3 = KaalCodec3(kbit);
    p_->applyRate();
    p_->reset();
    p_->out.clear();
    p_->head = 0;
    return true;
}
int VoiceTx::bitrate() const { return p_->bitrate; }
bool VoiceTx::setSweepCallsign(const std::string& callsign) {
    std::string base;
    int sfx = 0;
    p_->idSyms.fill(0);
    if (!splitCallsign(callsign, base, sfx)) return false;
    p_->idSyms = sweepIdSymbols(base, sfx);
    return true;
}
void VoiceTx::setBeaconMix(bool on) { p_->beaconOn = on; }
void VoiceTx::setBeacon(const BeaconFields& f, const std::string& text) {
    p_->bf = f;
    p_->btext = text;
    p_->makeSlices();
}

}  // namespace kaal
}  // namespace dect2
