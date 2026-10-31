// Inmarsat Aero C channel (8400 bit/s circuit-mode voice): the coding layer and the demodulator, bit-exact round trips.
//   format facts (aerol.cpp setSettings case 8400, DecodeC): 52-bit word per arm, 4096 channel bits, 16 blocks of 64 x 4, rate 3/4 puncturing
//   frame bits <-> three SUs + 25 voice frames (aeroCPack / aeroCUnpack), encoder -> decoder with and without inverted arms
//   modulator -> offset, clock error, noise -> AeroDemod(8400): the frames come back exact
// With DECT2_CCHANNEL_FILE set, the demodulator also runs on a real capture (cf32 or cs8 I/Q at DECT2_CCHANNEL_RATE, the carrier at
// DECT2_CCHANNEL_OFFSET Hz; DECT2_CCHANNEL_CONJ=1 mirrors the spectrum, which OnAir's own Airspy recordings need) and prints what it
// decodes: the reference values come from JAERO run on the same capture (the tools in docs/INMARSAT.md).
#include "dect2/aero/aero_demod.h"
#include "dect2/aero/aero_phy.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

namespace {

void randomSus(std::mt19937& g, uint8_t* su36) {
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 10; i++) su36[12 * k + i] = (uint8_t)g();
        su36[12 * k] = 0x30;                                    // "Call progress" like the real channel
        aeroSuSetCrc(&su36[12 * k]);
    }
}
void randomVoice(std::mt19937& g, uint8_t* v300) { for (int i = 0; i < kAeroCVoiceBytes; i++) v300[i] = (uint8_t)g(); }

struct Sent { std::vector<uint8_t> su, voice; };

// modulator -> carrier offset, clock error, noise -> demodulator
struct Result { int frames = 0, exactSu = 0, exactVoice = 0, sent = 0, susOk = 0, susBad = 0; double freq = 0; float ebn0 = 0; };
Result run(double fs, double secs, double ebn0Db, double cfo, double ppm, uint32_t seed, bool mirror = false) {
    std::mt19937 g(seed);
    AeroFrameEncoder enc(8400);
    AeroModulator mod(8400, fs, ppm);
    AeroDemod dem(8400, fs, 1500);
    Result r;
    std::vector<Sent> sent;
    dem.setCallback([&](AeroFrameEvent& e) {
        r.frames++; r.susOk += e.susOk; r.susBad += e.susBad; r.freq = e.freqHz; r.ebn0 = e.ebn0Db;
        for (const auto& s : sent) if (s.su == e.bytes) { r.exactSu++; if (s.voice == e.voice) r.exactVoice++; break; }
    });
    const AeroFrameFormat* f = aeroFrameFormat(8400);
    const double sigma = std::sqrt(fs / (2.0 * 8400 * std::pow(10.0, ebn0Db / 10)));
    genutil::NoiseSource noise(seed + 1);
    const size_t total = (size_t)(secs * fs);
    std::vector<cf32> buf(4096);
    double ph = 0;
    for (size_t done = 0; done < total;) {
        while (mod.queuedBits() < 2u * f->totalBits()) {
            Sent s; s.su.resize(kAeroCSuBytes); s.voice.resize(kAeroCVoiceBytes);
            randomSus(g, s.su.data()); randomVoice(g, s.voice.data());
            const auto bits = enc.cFrame(s.su.data(), s.voice.data());
            sent.push_back(std::move(s));
            mod.pushBits(bits.data(), bits.size());
        }
        const size_t n = std::min<size_t>(buf.size(), total - done);
        mod.generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            if (mirror) buf[i] = std::conj(buf[i]);
            buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += 2 * M_PI * cfo / fs;
            if (ph > M_PI) ph -= 2 * M_PI;
        }
        noise.add(buf.data(), n, (float)sigma);
        dem.feed(buf.data(), n);
        done += n;
    }
    r.sent = (int)sent.size();
    return r;
}

// ---- the real capture (optional) ----
class Decim {                      // windowed-sinc low-pass, one output every D inputs
public:
    Decim(int D, int len) : D_(D), h_(len), line_(2 * len, cf32(0, 0)) {
        const double c = (len - 1) / 2.0, cut = 0.4 / D;
        double sum = 0;
        for (int n = 0; n < len; n++) {
            const double t = n - c;
            const double s = t == 0 ? 2 * cut : std::sin(2 * M_PI * cut * t) / (M_PI * t);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * n / (len - 1)) + 0.08 * std::cos(4 * M_PI * n / (len - 1));
            h_[n] = (float)(s * w); sum += s * w;
        }
        for (auto& v : h_) v = (float)(v / sum);
    }
    bool push(cf32 x, cf32& out) {
        const size_t L = h_.size();
        line_[pos_] = x; line_[pos_ + L] = x;
        pos_ = pos_ + 1 == L ? 0 : pos_ + 1;
        if (++cnt_ < D_) return false;
        cnt_ = 0;
        const cf32* d = &line_[pos_];
        float re = 0, im = 0;
        for (size_t k = 0; k < L; k++) { re += h_[L - 1 - k] * d[k].real(); im += h_[L - 1 - k] * d[k].imag(); }
        out = cf32(re, im);
        return true;
    }
private:
    int D_, cnt_ = 0; std::vector<float> h_; std::vector<cf32> line_; size_t pos_ = 0;
};

void realCapture(const char* path) {
    const double rate = getenv("DECT2_CCHANNEL_RATE") ? atof(getenv("DECT2_CCHANNEL_RATE")) : 10e6;
    const double offset = getenv("DECT2_CCHANNEL_OFFSET") ? atof(getenv("DECT2_CCHANNEL_OFFSET")) : 0;
    const bool conj = getenv("DECT2_CCHANNEL_CONJ") && atoi(getenv("DECT2_CCHANNEL_CONJ")) != 0;
    const double secs = getenv("DECT2_CCHANNEL_SECONDS") ? atof(getenv("DECT2_CCHANNEL_SECONDS")) : 40;
    const std::string p(path);
    const bool cs8 = p.size() > 4 && p.compare(p.size() - 4, 4, ".cs8") == 0;
    const bool wav = p.size() > 4 && p.compare(p.size() - 4, 4, ".wav") == 0;     // 16-bit mono PCM, what JAERO is fed: real samples, the carrier at OFFSET Hz
    const bool verbose = getenv("DECT2_CCHANNEL_VERBOSE") != nullptr;
    FILE* f = fopen(path, "rb");
    if (!f) { printf("real capture: cannot open %s\n", path); return; }
    double rateIn = rate;
    if (wav) {
        uint8_t hdr[12]; if (fread(hdr, 1, 12, f) != 12) { fclose(f); return; }
        for (;;) {
            uint8_t ch[8]; if (fread(ch, 1, 8, f) != 8) { fclose(f); return; }
            const uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
            if (!std::memcmp(ch, "fmt ", 4)) {
                std::vector<uint8_t> fm(sz); if (fread(fm.data(), 1, sz, f) != sz) { fclose(f); return; }
                rateIn = (double)((uint32_t)fm[4] | ((uint32_t)fm[5] << 8) | ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24));
            } else if (!std::memcmp(ch, "data", 4)) break;
            else fseek(f, (long)sz + (sz & 1), SEEK_CUR);
        }
    }
    // 10 Msps -> 400 kHz -> 50 kHz (two stages); other rates: the stages are chosen to land near 50 kHz; a WAV is used as it is
    const double rateUse = wav ? rateIn : rate;
    int d1 = std::max(1, (int)std::lround(rateUse / 400e3)), d2 = std::max(1, (int)std::lround(rateUse / d1 / 50e3));
    if (wav) d1 = d2 = 1;
    const double fs2 = rateUse / d1 / d2;
    Decim s1(d1, 8 * d1 + 1), s2(d2, 8 * d2 + 1);
    AeroDemod dem(8400, fs2, 2000);
    int frames = 0, susOk = 0, susBad = 0, voiceFrames = 0, calls = 0;
    float ebn0 = 0;
    dem.setCallback([&](AeroFrameEvent& e) {
        frames++; susOk += e.susOk; susBad += e.susBad; ebn0 = e.ebn0Db;
        if (e.susOk) voiceFrames += kAeroCVoiceFrames;
        if (verbose) printf("  frame %3d at bit %8llu: uw errors %2d, SUs %d ok %d bad, BER %.3f, Eb/N0 %.1f dB, carrier %.1f Hz\n",
                            frames, (unsigned long long)e.bitIndex, e.uwErrors, e.susOk, e.susBad, e.channelBer, e.ebn0Db, e.freqHz);
        for (size_t k = 0; k + 12 <= e.bytes.size(); k += 12) {
            const uint8_t* su = &e.bytes[k];
            if (!aeroSuCrcOk(su)) continue;
            bool zero = true; for (int i = 0; i < 10; i++) zero &= su[i] == 0;
            if (zero) continue;
            if (su[0] == 0x30) calls++;
            printf("  frame %d (uw errors %d, Eb/N0 %.1f dB, BER %.3f): SU type %02X AES %02X%02X%02X GES %02X  [%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]\n",
                   frames, e.uwErrors, e.ebn0Db, e.channelBer, su[0], su[1], su[2], su[3], su[4], su[0], su[1], su[2], su[3], su[4], su[5], su[6], su[7], su[8], su[9]);
        }
    });
    const double offEff = conj ? -offset : offset;
    const double inc = -2 * M_PI * offEff / rateUse;
    double ph = 0;
    std::vector<cf32> out; out.reserve(4096);
    const size_t chunk = 1 << 18;
    std::vector<cf32> in(chunk);
    std::vector<int8_t> in8(2 * chunk);
    std::vector<int16_t> in16(chunk);
    size_t total = (size_t)(secs * rateUse), done = 0;
    printf("real capture %s: %.4f Msps, carrier at %+.1f kHz%s, %.0f s, demodulator at %.0f Hz (%d x %d)\n", path, rateUse / 1e6, offset / 1e3,
           conj ? ", spectrum mirrored" : "", secs, fs2, d1, d2);
    while (done < total) {
        size_t n = std::min(chunk, total - done), got;
        if (cs8) {
            got = fread(in8.data(), 2, n, f);
            for (size_t i = 0; i < got; i++) in[i] = cf32(in8[2 * i] / 128.f, in8[2 * i + 1] / 128.f);
        } else if (wav) {
            got = fread(in16.data(), 2, n, f);
            for (size_t i = 0; i < got; i++) in[i] = cf32(in16[i] / 32768.f, 0.f);
        } else got = fread(in.data(), sizeof(cf32), n, f);
        if (!got) break;
        out.clear();
        for (size_t i = 0; i < got; i++) {
            cf32 x = conj ? std::conj(in[i]) : in[i];
            x *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += inc; if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            cf32 y, z;
            if (s1.push(x, y) && s2.push(y, z)) out.push_back(z);
        }
        if (!out.empty()) dem.feed(out.data(), out.size());
        done += got;
    }
    fclose(f);
    printf("real capture: %d frames, SUs %d ok / %d bad, %d Call progress SUs, %d voice frames carried, last Eb/N0 %.1f dB, carrier %.1f Hz, acquired %d synced %d\n",
           frames, susOk, susBad, calls, voiceFrames, ebn0, dem.freqHz(), (int)dem.acquired(), (int)dem.synced());
    printf("real capture: framer %llu frames, %llu word misses, %llu word bit errors, %llu false syncs; %.1f frames possible in %.0f s\n",
           (unsigned long long)dem.framer().frames(), (unsigned long long)dem.framer().uwMisses(), (unsigned long long)dem.framer().uwBitErrors(),
           (unsigned long long)dem.framer().falseSyncs(), secs * 8400 / 4200.0, secs);
}

} // namespace

int main() {
    // 1. format facts
    {
        const AeroFrameFormat* f = aeroFrameFormat(8400);
        CHECK(f != nullptr, "8400 format");
        CHECK(f && f->totalBits() == 4200, "C frame is 4200 channel bits (0.5 s)");
        CHECK(f && f->oqpsk() && f->cChannel() && f->blocks * f->blockBits() == kAeroCFrameBits, "16 blocks of 256");
        CHECK(f && f->infoBytes() == kAeroCSuBytes, "three SUs");
        CHECK(kAeroCSubFrames * kAeroCSubFrameBits + 1 + kAeroCVoiceBits <= kAeroCInfoBits, "25th voice frame fits");
        // the word on the arms, most significant bit first: Q on even channel bits (the first), I on odd ones
        AeroFrameEncoder e(8400);
        std::vector<uint8_t> su(kAeroCSuBytes, 0), v(kAeroCVoiceBytes, 0);
        const auto bits = e.cFrame(su.data(), v.data());
        CHECK(bits.size() == 4200, "frame bits %zu", bits.size());
        uint64_t wq = 0, wi = 0;
        for (int k = 0; k < kAeroCUwBits; k++) { wq = (wq << 1) | bits[2 * k]; wi = (wi << 1) | bits[2 * k + 1]; }
        CHECK(wq == kAeroCUwQ && wi == kAeroCUwI, "unique words %llx %llx", (unsigned long long)wq, (unsigned long long)wi);
    }
    // 2. pack / unpack
    {
        std::mt19937 g(1);
        uint8_t su[kAeroCSuBytes], v[kAeroCVoiceBytes], su2[kAeroCSuBytes], v2[kAeroCVoiceBytes], bits[kAeroCInfoBits];
        randomSus(g, su); randomVoice(g, v);
        aeroCPack(su, v, bits);
        aeroCUnpack(bits, su2, v2);
        CHECK(std::memcmp(su, su2, kAeroCSuBytes) == 0 && std::memcmp(v, v2, kAeroCVoiceBytes) == 0, "pack / unpack round trip");
        // the layout: voice frame 0 bit 0 is frame bit 1, signalling of sub-frame 0 is bits 97..108, voice frame 24 starts at 2617
        CHECK(bits[1] == (v[0] & 1) && bits[97] == (su[0] & 1) && bits[2617] == (v[24 * 12] & 1), "sub-frame layout");
    }
    // 3. encoder -> decoder at the bit level, three frames in a row (the code and the Viterbi run on), straight and with the I arm inverted
    for (int inverted = 0; inverted < 2; inverted++) {
        std::mt19937 g(7 + inverted);
        AeroFrameEncoder enc(8400);
        AeroFramer fr(8400);
        std::vector<Sent> sent;
        int got = 0, exact = 0, susOk = 0;
        fr.setCallback([&](AeroFrameEvent& e) {
            got++; susOk += e.susOk;
            if (got <= (int)sent.size() && e.bytes == sent[got - 1].su && e.voice == sent[got - 1].voice) exact++;
        });
        for (int n = 0; n < 3; n++) {
            Sent s; s.su.resize(kAeroCSuBytes); s.voice.resize(kAeroCVoiceBytes);
            randomSus(g, s.su.data()); randomVoice(g, s.voice.data());
            const auto bits = enc.cFrame(s.su.data(), s.voice.data());
            sent.push_back(std::move(s));
            for (size_t i = 0; i < bits.size(); i++) {
                float soft = bits[i] ? 1.f : -1.f;
                if (inverted && (i & 1)) soft = -soft;
                fr.push(soft);
            }
        }
        for (int i = 0; i < 200; i++) fr.push(0.01f);
        CHECK(got == 3 && exact == 3 && susOk == 9, "bit-level round trip (inverted %d): %d frames, %d exact, %d SUs ok", inverted, got, exact, susOk);
    }
    // 4. modulator -> demodulator: 48 kHz, 200 Hz off, 30 ppm clock error, Eb/N0 10 dB and 6 dB, and a mirrored spectrum (I/Q swapped)
    {
        const Result a = run(48000, 12, 10, 200, 30, 11);
        printf("8400 at 10 dB: %d sent, %d frames, %d exact SU sets, %d exact voice, SUs %d/%d, carrier %.1f Hz, Eb/N0 %.1f dB\n",
               a.sent, a.frames, a.exactSu, a.exactVoice, a.susOk, a.susBad, a.freq, a.ebn0);
        CHECK(a.frames >= 18 && a.exactVoice >= 16 && a.susBad <= 3, "clean channel: %d frames, %d exact voice, %d bad SUs", a.frames, a.exactVoice, a.susBad);
        CHECK(std::fabs(a.freq - 200) < 15, "carrier estimate %.1f Hz", a.freq);
        const Result b = run(48000, 12, 6, -300, -30, 12);
        printf("8400 at 6 dB: %d frames, %d exact voice, SUs %d/%d\n", b.frames, b.exactVoice, b.susOk, b.susBad);
        CHECK(b.frames >= 14 && b.exactVoice >= 8, "noisy channel: %d frames, %d exact voice", b.frames, b.exactVoice);
        const Result c = run(48000, 12, 10, 200, 0, 13, true);
        printf("8400 mirrored: %d frames, %d exact voice, SUs %d/%d\n", c.frames, c.exactVoice, c.susOk, c.susBad);
        CHECK(c.frames >= 18 && c.exactVoice >= 16, "mirrored spectrum: %d frames, %d exact voice", c.frames, c.exactVoice);
    }
    // 5. a real capture when the user names one
    if (const char* path = getenv("DECT2_CCHANNEL_FILE")) realCapture(path);
    return testSummary("aero_cchannel");
}
