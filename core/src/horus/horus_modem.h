// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus receiver internals (private to core/src/horus): the channel filter, the tone estimator, the non-coherent 4FSK
// demodulator with its symbol timing, the binary deframer (sync on preamble + unique word, layer 2 decode, CRC), the RTTY
// demodulator (two-tone correlators, asynchronous framers for 50 / 100 / 300 baud, 7 and 8 data bits, both polarities) and the
// wide-span detector that finds further balloons in the captured band. Written for OnAir.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/horus/horus_proto.h"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::horus {

// FIR low-pass with decimation (complex in, real taps), keeps its history between calls
class FirDecim {
public:
    void init(std::vector<float> taps, int decim);
    void reset();
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    int decimation() const { return d_; }
private:
    std::vector<float> h_;
    std::vector<cf32> buf_;
    size_t pos_ = 0;
    int d_ = 1;
};

// Two boxcars of D in a row, decimated by D (a second-order CIC; double integrators, no gain growth in the output)
class Cic2 {
public:
    void init(int d);
    void reset();
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    int decimation() const { return d_; }
private:
    int d_ = 1, pos_ = 0, cnt_ = 0;
    std::complex<double> integ_ = 0, acc_ = 0;
    std::vector<std::complex<double>> hist_;
};

// The channel: mixer at the input rate, a CIC-2 decimator, a Kaiser low-pass decimator to about 10 kHz
class ChannelFilter {
public:
    void init(double inRate);
    void reset();
    void setMixer(double hz);   // the input frequency brought to 0 Hz
    double mixer() const { return mixHz_; }
    double outRate() const { return outRate_; }
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
private:
    double inRate_ = 0, outRate_ = 0, mixHz_ = 0, ph_ = 0, dph_ = 0;
    Cic2 s1_;
    FirDecim s2_;
    std::vector<cf32> tmp_, mixed_;
};

// Averaged power spectrum of the channel (Hann, 50 % overlap, about 2 s memory) and the tone search on it
class ToneEstimator {
public:
    void init(double fs);
    void reset();
    void push(const cf32* x, size_t n);
    // the k strongest peaks at least minSepHz apart, at least 6 dB over the median, inside +-maxHz; ascending frequency
    int peaks(int k, double minSepHz, double maxHz, double* outHz, float* outDb) const;
    float floorDb() const;
    double binHz() const { return fs_ / n_; }
    int size() const { return n_; }
    const std::vector<float>& power() const { return avg_; }   // bins in frequency order, -fs/2 .. +fs/2
    bool primed() const { return frames_ >= 4; }
private:
    double fs_ = 1;
    int n_ = 1024, hop_ = 512, frames_ = 0;
    std::vector<cf32> buf_;
    std::vector<float> win_, avg_;
    struct FftHolder;
    std::shared_ptr<FftHolder> fft_;
};

// Sliding correlators: the energy of the last L samples at each of a few tone frequencies
class ToneBank {
public:
    void init(int len, int tones);
    void reset();
    void setFreqs(const double* cyclesPerSample);
    // one sample in; e receives the energies (one per tone)
    void push(cf32 x, float* e);
    int length() const { return L_; }
private:
    int L_ = 1, K_ = 1, pos_ = 0, sinceExact_ = 0;
    std::vector<cf32> raw_, mixed_;      // L entries; mixed_ is K rows of L
    std::vector<std::complex<double>> sum_;
    std::vector<double> w_, ph_;
};

// The 4FSK demodulator: four correlators one symbol long, the symbol clock from where the strongest tone energy peaks
class Fsk4Demod {
public:
    void init(double fs, double baud);
    void reset();
    void setTones(const double hz[4]);
    // symbols (0..3, lowest tone first) with the four energies of each
    void process(const cf32* x, size_t n, const std::function<void(int, const float*)>& onSymbol);
    bool hasTones() const { return have_; }
    const double* tones() const { return hz_; }
private:
    double fs_ = 1, baud_ = 100, hz_[4] = {};
    bool have_ = false;
    int L_ = 1, phase_ = 0, best_ = 0, sinceEmit_ = 0;
    ToneBank bank_;
    std::vector<float> timing_;
};

// Binary frames from the symbol stream: four mappings of tones to bits are watched for the preamble + unique word
class Deframer {
public:
    void reset();
    // returns true when a packet was decoded (out filled); energies are the four tone energies of the symbol
    void push(int sym, const float* e, const Tables& t, double baud, std::vector<Packet>& out);
    uint64_t syncs() const { return syncs_; }
    uint64_t failures() const { return fails_; }
private:
    struct Pend { int variant; int64_t start; bool triedV1 = false; };
    std::array<uint32_t, 4> reg_{};
    std::deque<uint8_t> syms_;
    std::deque<std::array<float, 2>> sig_;   // per symbol: max energy, mean of the other three
    int64_t base_ = 0, nbits_ = 0;           // symbol index of syms_[0]; bits pushed per variant
    std::vector<Pend> pend_;
    uint64_t syncs_ = 0, fails_ = 0;
    int bitOf(int variant, int64_t bit) const;
    bool tryDecode(const Pend& p, int bytes, const Tables& t, double baud, Packet& out);
};

// RTTY: two tones, framers for 50 / 100 / 300 baud, 7 or 8 data bits, both polarities; whole sentences with a good CRC
class RttyDemod {
public:
    void init(double fs);
    void reset();
    void setTones(double loHz, double hiHz);
    void process(const cf32* x, size_t n, const Tables& t, std::vector<Packet>& out);
    bool hasTones() const { return have_; }
private:
    struct Framer {
        int baud = 100, nbits = 7, pol = 1, state = 0, k = 0, ch = 0;
        double ts = 0, Lb = 100;
        float prev = 0;
        std::string line;
    };
    struct Rate { int baud; ToneBank bank; float d = 0, snrAcc = 0; double sAcc = 0, nAcc = 0; };
    double fs_ = 1, lo_ = 0, hi_ = 0;
    bool have_ = false;
    int64_t n_ = 0;
    std::vector<Rate> rates_;
    std::vector<Framer> framers_;
    void onChar(Framer& f, int c, const Tables& t, std::vector<Packet>& out, const Rate& r);
};

// One channel: filter, estimator, both demodulators. Packets carry the channel's frequency and SNR.
class Channel {
public:
    void init(double inRate, double mixerHz, bool dynamic);
    void reset();
    void setMixer(double hz) { filt_.setMixer(hz); }
    double mixer() const { return filt_.mixer(); }
    void feed(const cf32* x, size_t n, const Tables& t, double dialHz, std::vector<Packet>& out);
    bool dynamic() const { return dynamic_; }
    double outRate() const { return filt_.outRate(); }
    const ToneEstimator& estimator() const { return est_; }
    const Fsk4Demod& fsk4() const { return fsk_; }
    double lastDecodeSec = -1e9, createdSec = 0, lastSeenSec = 0;
    uint64_t packets = 0;
    int mode = 0;          // 0 nothing yet, 1 binary 4FSK, 2 RTTY (what decoded last)
    float snrDb = -99;     // of the last packet
    double timeSec() const { return t_; }
    uint64_t syncs() const { return def_.syncs(); }
    uint64_t failures() const { return def_.failures(); }
    double toneSpacing() const;
    float spectrumSnr(double loHz, double hiHz) const;   // SNR in 3 kHz of what lies between lo and hi (channel frequencies)
    double rttyTones[2] = {0, 0};
private:
    ChannelFilter filt_;
    ToneEstimator est_;
    Fsk4Demod fsk_;
    Deframer def_;
    RttyDemod rtty_;
    std::vector<cf32> y_;
    double t_ = 0, nextEst_ = 0.5;
    bool dynamic_ = false;
    void updateTones();
};

// Finds narrow signals (up to about 2.5 kHz wide) anywhere in the input: an averaged spectrum at the input rate
class WideDetector {
public:
    struct Hit { double hz; float db; };
    void init(double rate);
    void reset();
    void push(const cf32* x, size_t n);
    bool ready() const { return frames_ >= 6; }
    std::vector<Hit> hits(double excludeAroundHz, double excludeHalfWidth, double maxAbsHz) const;
private:
    double rate_ = 1;
    int n_ = 0, every_ = 0, gather_ = 0, frames_ = 0;
    int64_t since_ = 0;
    std::vector<cf32> buf_;
    std::vector<float> win_, avg_;
    struct FftHolder;
    std::shared_ptr<FftHolder> fft_;
};

} // namespace dect2::horus
