// Signal-processing cost table: CPU seconds per second of signal for the DSP kernels and the receivers that take most of the CPU, measured
// without the Engine, the GUI or the radio (those are other people's tables). Not part of ctest; build and run with tools/bench/bench_dsp.sh.
//
//   bench_dsp [--list] [--only name[,name..]] [--seconds S] [--repeat N] [--best N] [--md] [--digest]
//
// Every row: the signal is made first (not timed), then fed in blocks of 16384 samples (receivers) or the kernel runs over it; the figure is the
// process CPU time (user + system, all threads: getrusage) spent per second of signal, so a busy machine does not change it much. A receiver
// with its own threads is destroyed inside the timed part, so the work it still had queued is counted. "items" is a sanity count (log lines,
// packets, outputs): a row whose items drop to 0 after a change has broken something.
#include "dect2/common/ring.h"
#include "dect2/dsp/carrierfind.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/engine/channelizer.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/adsb/adsb_gen.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/vdl4/vdl4_gen.h"
#include "dect2/vdl4/vdl4_rx.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dab/dab.h"
#include "dect2/drm/drm_rx.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/hdr/hdr_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/atsc/atsc_gen.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/t2/t2fec.h"
#include "dect2/dsp/ldpc.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <sys/resource.h>

using namespace dect2;

namespace {

double cpuNow() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
}

// --digest: every receiver row also hashes its output (FNV-1a) and prints the hash as "items", so a speed change can be shown to leave the output as it was
bool g_dg = false;
uint64_t g_h = 1469598103934665603ull;
void dgAdd(const void* p, size_t n) { auto* b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { g_h ^= b[i]; g_h *= 1099511628211ull; } }
void dgStr(const std::string& s) { dgAdd(s.data(), s.size()); }

struct Row { std::string name, what; double rate = 0, sigSec = 0, cpu = 0; long long items = 0; };

struct Bench {
    std::string name, what;
    double rate;
    std::function<Row(double secs)> run;
};

// a signal from a mode's built-in test generator
std::vector<cf32> synth(std::unique_ptr<ModeSynth> s, double secs) {
    std::vector<cf32> v;
    if (!s) return v;
    v.resize((size_t)(secs * s->sampleRate()));
    for (size_t i = 0; i < v.size(); i += 65536) s->generate(v.data() + i, std::min<size_t>(65536, v.size() - i));
    return v;
}

SynthConfig cfgFor(int mode) { SynthConfig c; c.mode = mode; c.snrDb = 30; return c; }

std::vector<cf32> noiseSig(double rate, double secs, unsigned seed = 3) {
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0.f, 0.2f);
    std::vector<cf32> v((size_t)(rate * secs));
    for (auto& x : v) x = cf32(nd(rng), nd(rng));
    return v;
}

// feeds a receiver in 16384-sample blocks; `make` builds and configures it (not timed), the destructor is timed
template <class Rx>
Row feedRx(const char* name, const char* what, double rate, const std::vector<cf32>& sig, const std::function<void(Rx&, long long&)>& setup) {
    Row r; r.name = name; r.what = what; r.rate = rate; r.sigSec = sig.size() / rate;
    auto rx = std::make_unique<Rx>();
    g_h = 1469598103934665603ull;
    long long items = 0;
    setup(*rx, items);
    const double c0 = cpuNow();
    for (size_t i = 0; i < sig.size(); i += 16384) rx->feed(sig.data() + i, std::min<size_t>(16384, sig.size() - i));
    rx.reset();
    r.cpu = cpuNow() - c0;
    r.items = g_dg ? (long long)(g_h & 0x3fffffffffffffffull) : items;
    return r;
}

template <class Rx>
void logCount(Rx& rx, long long& items) { rx.setLogCallback([&items](const std::string&) { items++; }); }

std::vector<Bench> benches() {
    std::vector<Bench> b;
    // ---------------------------------------------------------------- kernels
    b.push_back({"fft32k", "fftSplit 32768 points, back to back at 10 Msps", 10e6, [](double secs) {
        Row r; r.name = "fft32k"; r.rate = 10e6; r.sigSec = secs;
        std::vector<float> re(32768), im(32768);
        for (int i = 0; i < 32768; i++) { re[i] = std::sin(0.01f * i); im[i] = std::cos(0.013f * i); }
        const long n = (long)(secs * 10e6 / 32768);
        const double c0 = cpuNow();
        for (long k = 0; k < n; k++) { fftSplit(re.data(), im.data(), 15, (k & 1) != 0); re[0] *= 1.f / 32768; }
        r.cpu = cpuNow() - c0; r.items = n;
        return r;
    }});
    b.push_back({"spectrum", "SpectrumAnalyzer 32768 bins at 10 Msps, a frame every 40 ms", 10e6, [](double secs) {
        Row r; r.name = "spectrum"; r.rate = 10e6;
        auto sig = noiseSig(10e6, secs); r.sigSec = secs;
        SpectrumAnalyzer sa(32768);
        SpectrumFrame fr;
        const double c0 = cpuNow();
        const size_t per = 400000;   // 40 ms
        for (size_t i = 0; i < sig.size(); i += 16384) {
            const size_t n = std::min<size_t>(16384, sig.size() - i);
            sa.feed(sig.data() + i, n);
            if ((i + n) / per != i / per && sa.takeFrame(fr)) r.items++;
        }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"rational_rs", "RationalResampler 10 -> 9.142857 Msps (DVB-T2 native)", 10e6, [](double secs) {
        Row r; r.name = "rational_rs"; r.rate = 10e6;
        auto sig = noiseSig(10e6, secs); r.sigSec = secs;
        RationalResampler rs; rs.configure(10e6, 64e6 / 7);
        std::vector<cf32> out; out.reserve(1 << 15);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) { out.clear(); rs.process(sig.data() + i, std::min<size_t>(16384, sig.size() - i), out); r.items += out.size(); }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"rational_dec", "RationalResampler 2.4 Msps -> 48 kHz, 12.5 kHz occupied (wide-bank carrier)", 2.4e6, [](double secs) {
        Row r; r.name = "rational_dec"; r.rate = 2.4e6;
        auto sig = noiseSig(2.4e6, secs); r.sigSec = secs;
        RationalResampler rs; rs.configure(2.4e6, 48e3, nullptr, 12500);
        std::vector<cf32> out; out.reserve(1 << 15);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) { out.clear(); rs.process(sig.data() + i, std::min<size_t>(16384, sig.size() - i), out); r.items += out.size(); }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"exact_rs", "ExactResampler 10 -> 9.142857 Msps, 16 lobes", 10e6, [](double secs) {
        Row r; r.name = "exact_rs"; r.rate = 10e6;
        auto sig = noiseSig(10e6, secs); r.sigSec = secs;
        ExactResampler rs; rs.configure(10e6, 64e6 / 7);
        std::vector<cf32> out; out.reserve(1 << 15);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) { out.clear(); rs.process(sig.data() + i, std::min<size_t>(16384, sig.size() - i), out); r.items += out.size(); }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"tv_channel", "ChannelFilter (multi-TV channeliser) 20 Msps -> 10 Msps, 4 MHz off", 20e6, [](double secs) {
        Row r; r.name = "tv_channel"; r.rate = 20e6;
        auto sig = noiseSig(20e6, secs); r.sigSec = secs;
        ChannelFilter cf; cf.configure(20e6, 4e6, 10e6);
        std::vector<cf32> out; out.reserve(1 << 15);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) { out.clear(); cf.process(sig.data() + i, std::min<size_t>(16384, sig.size() - i), out); r.items += out.size(); }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"wide_bank", "CarrierFinder + 8 Channelizers 2.4 Msps -> 48 kHz (wide-bank carriers, receivers not included)", 2.4e6, [](double secs) {
        Row r; r.name = "wide_bank"; r.rate = 2.4e6;
        auto sig = noiseSig(2.4e6, secs); r.sigSec = secs;
        CarrierFinder f; f.configure(2.4e6, CarrierCfg{});
        std::vector<Channelizer> ch(8);
        for (int k = 0; k < 8; k++) ch[k].configure(2.4e6, 48e3, -1e6 + 250e3 * k + 12500, 12500);
        std::vector<cf32> out; out.reserve(1 << 12);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) {
            const size_t n = std::min<size_t>(16384, sig.size() - i);
            f.feed(sig.data() + i, n);
            for (auto& c : ch) { out.clear(); c.process(sig.data() + i, n, out); r.items += out.size(); }
        }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"fm_front", "FmFront (LTR / EDACS control channel) 2.4 Msps -> 48 kHz, 100 kHz off", 2.4e6, [](double secs) {
        Row r; r.name = "fm_front"; r.rate = 2.4e6;
        auto sig = noiseSig(2.4e6, secs); r.sigSec = secs;
        FmFront fe; fe.configure(2.4e6, 48e3, 6000); fe.setOffset(100e3);
        std::vector<float> out; out.reserve(1 << 12);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) { out.clear(); fe.feed(sig.data() + i, std::min<size_t>(16384, sig.size() - i), out); r.items += out.size(); }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"t2_ldpc", "DVB-T2 LDPC 64800 rate 3/5 256-QAM at 18 dB, 624 blocks a second (8-bit layered min-sum)", 0, [](double secs) {
        Row r; r.name = "t2_ldpc"; r.rate = 0;
        PlpFec f; f.rate = 2; f.mod = 3; f.rotation = true;
        FecDims d = fecDims(f);
        const LdpcCode& ldpc = ldpcFor(f); const BchCode& bch = bchFor(f);
        std::mt19937 rng(9); dect2::normal_distribution<float> nd(0.f, 1.f);
        const int blocks = 16;
        std::vector<std::vector<float>> llrs(blocks);
        const float sigma = 0.55f;   // BPSK-equivalent noise on the LLRs: a few iterations, like a working mux
        for (int k = 0; k < blocks; k++) {
            std::vector<uint8_t> bits(d.kBch); for (auto& v : bits) v = rng() & 1;
            bch.encode(bits, d.kBch); ldpc.encode(bits);
            llrs[k].resize(bits.size());
            for (size_t i = 0; i < bits.size(); i++) { const float s = bits[i] ? -1.f : 1.f; llrs[k][i] = 2 * (s + sigma * nd(rng)) / (sigma * sigma); }
        }
        const long n = (long)(secs * 624);
        std::vector<uint8_t> hard; int it = 0;
        const double c0 = cpuNow();
        for (long k = 0; k < n; k++) { if (ldpc.decodeFast8(llrs[k % blocks], 50, hard, &it)) r.items++; }
        r.cpu = cpuNow() - c0; r.sigSec = secs;
        return r;
    }});
    // ---------------------------------------------------------------- receivers on their own test signals
    b.push_back({"fm", "FM broadcast receiver (stereo, RDS) at 2 Msps", 2e6, [](double secs) {
        FmGenConfig g; g.rate = 2e6; FmGenerator gen(g);
        std::vector<cf32> sig; gen.generate((size_t)(secs * 2e6), sig);
        return feedRx<FmReceiver>("fm", "", 2e6, sig, [](FmReceiver& rx, long long&) { rx.configure(2e6); rx.setSilent(true); if (g_dg) rx.setAudioTap([](const float* l, const float* r, size_t n) { dgAdd(l, n * 4); dgAdd(r, n * 4); }); });
    }});
    auto general = [&](const char* name, int demod, double rate) {
        b.push_back({name, "general receiver", rate, [=](double secs) {
            SynthConfig c = cfgFor(0); c.modeOpt[0] = demod; c.modeVal[0] = 50e3;
            auto sig = synth(makeGeneralSynth(c, rate), secs);
            return feedRx<GeneralReceiver>(name, "", rate, sig, [=](GeneralReceiver& rx, long long& it) {
                rx.configure(rate); rx.setDemod(demod); rx.setChannelOffsetHz(50e3); logCount(rx, it); rx.setSilent(true); if (g_dg) rx.setAudioTap([](const float* x, size_t n) { dgAdd(x, n * 4); }); });
        }});
    };
    general("nfm", 0, 2.4e6);
    general("am", 2, 2.4e6);
    general("usb", 3, 2.4e6);
    b.push_back({"adsb", "ADS-B 1090 receiver at 2.4 Msps", 2.4e6, [](double secs) {
        auto sig = synth(makeAdsbSynth(cfgFor(0), 2.4e6), secs);
        return feedRx<AdsbReceiver>("adsb", "", 2.4e6, sig, [](AdsbReceiver& rx, long long& it) { rx.configure(2.4e6); logCount(rx, it); });
    }});
    b.push_back({"ais", "AIS receiver (both channels) at 2 Msps", 2e6, [](double secs) {
        auto sig = synth(makeAisSynth(cfgFor(0), 2e6), secs);
        return feedRx<AisReceiver>("ais", "", 2e6, sig, [](AisReceiver& rx, long long& it) { rx.configure(2e6); logCount(rx, it); });
    }});
    b.push_back({"vdl4", "VDL Mode 4 receiver (six channels) at 2 Msps", 2e6, [](double secs) {
        auto sig = synth(makeVdl4Synth(cfgFor(0), 2e6), secs);
        return feedRx<Vdl4Receiver>("vdl4", "", 2e6, sig, [](Vdl4Receiver& rx, long long& it) { rx.configure(2e6); logCount(rx, it); });
    }});
    b.push_back({"dmr", "DMR receiver at 2.4 Msps", 2.4e6, [](double secs) {
        auto sig = synth(makeDmrSynth(cfgFor(0), 2.4e6), secs);
        return feedRx<DmrReceiver>("dmr", "", 2.4e6, sig, [](DmrReceiver& rx, long long& it) { rx.configure(2.4e6); logCount(rx, it); rx.setSilent(true); });
    }});
    b.push_back({"p25", "P25 receiver at 2.4 Msps", 2.4e6, [](double secs) {
        auto sig = synth(makeP25Synth(cfgFor(0), 2.4e6), secs);
        return feedRx<P25Receiver>("p25", "", 2.4e6, sig, [](P25Receiver& rx, long long& it) { rx.configure(2.4e6); logCount(rx, it); rx.setSilent(true); });
    }});
    b.push_back({"tetra", "TETRA receiver at 2.4 Msps", 2.4e6, [](double secs) {
        auto sig = synth(makeTetraSynth(cfgFor(0), 2.4e6), secs);
        return feedRx<TetraReceiver>("tetra", "", 2.4e6, sig, [](TetraReceiver& rx, long long& it) { rx.configure(2.4e6); logCount(rx, it); });
    }});
    b.push_back({"dvbt", "DVB-T 8K 64-QAM 3/4 GI 1/8 at the native 9.142857 Msps", 64e6 / 7, [](double secs) {
        dvbt::Params p; p.mode = dvbt::k8K; p.mod = dvbt::k64Qam; p.guard = dvbt::kGi8; p.crHp = p.crLp = dvbt::kR34;
        uint32_t counter = 0;
        dvbt::Generator gen(p, [&](uint8_t* pkt) { pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0; pkt[3] = 0x10; memcpy(pkt + 4, &counter, 4); for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7); counter++; });
        const double fn = 64e6 / 7;
        std::mt19937 rng(5); dect2::normal_distribution<float> nd(0, 1);
        const float sigma = (float)(std::pow(10.0, -30 / 20.0) / std::sqrt(2.0));
        std::vector<cf32> sig, sym;
        while (sig.size() < secs * fn) { gen.nextSymbol(sym); for (auto v : sym) sig.push_back(v + cf32{nd(rng), nd(rng)} * sigma); }
        return feedRx<DvbtReceiver>("dvbt", "", fn, sig, [&](DvbtReceiver& rx, long long& it) {
            rx.configure(fn, 8); rx.setPacketCallback([&it](const uint8_t*, size_t n, double) { it += (long long)n; }); });
    }});
    b.push_back({"dtmb", "DTMB receiver at 10 Msps", 10e6, [](double secs) {
        auto sig = synth(makeDtmbSynth(cfgFor(0), 10e6), secs);
        return feedRx<DtmbReceiver>("dtmb", "", 10e6, sig, [](DtmbReceiver& rx, long long& it) {
            rx.configure(10e6); rx.setPacketCallback([&it](const uint8_t*, size_t n, double) { it += (long long)n; }); });
    }});
    b.push_back({"dab", "DAB receiver at 2.048 Msps", 2.048e6, [](double secs) {
        auto sig = synth(makeDabSynth(cfgFor(0), 2.048e6), secs);
        return feedRx<DabReceiver>("dab", "", 2.048e6, sig, [](DabReceiver& rx, long long& it) { rx.configure(2.048e6); rx.audio().setSilent(true); rx.select(1); if (g_dg) rx.audio().setPcmTap([](const float* x, int n) { dgAdd(x, (size_t)n * 8); }); rx.setPacketCallback([&it](const uint8_t*, size_t n, double) { it += (long long)n; }); });
    }});
    b.push_back({"atsc", "ATSC 1.0 receiver at 8 Msps", 8e6, [](double secs) {
        uint32_t counter = 0;
        atsc::Generator gen([&](uint8_t* pkt) { pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0; pkt[3] = 0x10; memcpy(pkt + 4, &counter, 4); for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7); counter++; },
                            atsc::ChannelConfig{}, 8e6);
        std::vector<cf32> sig; gen.generate((size_t)(secs * 8e6), sig);
        return feedRx<AtscReceiver>("atsc", "", 8e6, sig, [](AtscReceiver& rx, long long& it) {
            rx.configure(8e6); rx.setPacketCallback([&it](const uint8_t*, size_t n, double) { it += (long long)n; }); });
    }});
    // ---------------------------------------------------------------- the same receivers on noise only (nothing to lock to: the search cost)
    b.push_back({"dvbt_idle", "DVB-T receiver on noise at 9.142857 Msps", 64e6 / 7, [](double secs) {
        auto sig = noiseSig(64e6 / 7, secs);
        return feedRx<DvbtReceiver>("dvbt_idle", "", 64e6 / 7, sig, [](DvbtReceiver& rx, long long&) { rx.configure(64e6 / 7, 8); });
    }});
    b.push_back({"dtmb_idle", "DTMB receiver on noise at 10 Msps", 10e6, [](double secs) {
        auto sig = noiseSig(10e6, secs);
        return feedRx<DtmbReceiver>("dtmb_idle", "", 10e6, sig, [](DtmbReceiver& rx, long long&) { rx.configure(10e6); });
    }});
    b.push_back({"atsc_idle", "ATSC 1.0 receiver on noise at 8 Msps", 8e6, [](double secs) {
        auto sig = noiseSig(8e6, secs);
        return feedRx<AtscReceiver>("atsc_idle", "", 8e6, sig, [](AtscReceiver& rx, long long&) { rx.configure(8e6); });
    }});
    b.push_back({"dab_idle", "DAB receiver on noise at 2.048 Msps", 2.048e6, [](double secs) {
        auto sig = noiseSig(2.048e6, secs);
        return feedRx<DabReceiver>("dab_idle", "", 2.048e6, sig, [](DabReceiver& rx, long long&) { rx.configure(2.048e6); rx.audio().setSilent(true); });
    }});
    b.push_back({"fm_idle", "FM broadcast receiver on noise at 2 Msps", 2e6, [](double secs) {
        auto sig = noiseSig(2e6, secs);
        return feedRx<FmReceiver>("fm_idle", "", 2e6, sig, [](FmReceiver& rx, long long&) { rx.configure(2e6); rx.setSilent(true); });
    }});
    b.push_back({"drm", "DRM30 receiver at 48 kHz (mode B, 64-QAM, AAC)", 48e3, [](double secs) {
        auto sig = synth(makeDrmSynth(cfgFor(12), 48e3), secs);
        return feedRx<DrmReceiver>("drm", "", 48e3, sig, [](DrmReceiver& rx, long long&) { rx.configure(48e3); rx.setSilent(true); if (g_dg) rx.setAudioTap([](const float* l, const float* r, size_t n) { dgAdd(l, n * 4); dgAdd(r, n * 4); }); });
    }});
    b.push_back({"drm_idle", "DRM30 receiver on noise at 48 kHz", 48e3, [](double secs) {
        auto sig = noiseSig(48e3, secs);
        return feedRx<DrmReceiver>("drm_idle", "", 48e3, sig, [](DrmReceiver& rx, long long&) { rx.configure(48e3); rx.setSilent(true); });
    }});
    b.push_back({"hdr_fm", "HD Radio FM hybrid receiver at 2 Msps", 2e6, [](double secs) {
        SynthConfig c = cfgFor(0); c.modeOpt[0] = 0;
        auto sig = synth(makeHdrSynth(c, 2e6), secs);
        return feedRx<HdrReceiver>("hdr_fm", "", 2e6, sig, [](HdrReceiver& rx, long long& it) { rx.configure(2e6); rx.setSilent(true); logCount(rx, it); if (g_dg) { rx.setAudioTap([](const float* x, size_t n) { dgAdd(x, n * 8); }); rx.setLogCallback([](const std::string& s) { dgStr(s); }); } });
    }});
    b.push_back({"hdr_am", "HD Radio AM hybrid receiver at 1 Msps", 1e6, [](double secs) {
        SynthConfig c = cfgFor(0); c.modeOpt[0] = 1;
        auto sig = synth(makeHdrSynth(c, 1e6), secs);
        return feedRx<HdrReceiver>("hdr_am", "", 1e6, sig, [](HdrReceiver& rx, long long& it) { rx.configure(1e6); rx.setSilent(true); logCount(rx, it); });
    }});
    b.push_back({"hdr_idle", "HD Radio FM receiver on noise at 2 Msps", 2e6, [](double secs) {
        auto sig = noiseSig(2e6, secs);
        return feedRx<HdrReceiver>("hdr_idle", "", 2e6, sig, [](HdrReceiver& rx, long long&) { rx.configure(2e6); rx.setSilent(true); });
    }});
    b.push_back({"airband", "airband receiver, test layout (5 channels) at 1 Msps", 1e6, [](double secs) {
        AirbandGenConfig g; g.chans = airbandTestLayout(); g.snrDb = 25; g.rate = 1e6; g.dialOffsetHz = -airbandTuning().tuneOffsetHz;
        AirbandGenerator gen(g);
        std::vector<cf32> sig((size_t)(secs * 1e6));
        for (size_t o = 0; o < sig.size(); o += 65536) gen.generate(sig.data() + o, std::min<size_t>(65536, sig.size() - o));
        const double dial = 118.700e6;
        const auto list = airbandChannelsFor(g.chans, dial);
        return feedRx<AirbandReceiver>("airband", "", 1e6, sig, [&](AirbandReceiver& rx, long long&) {
            rx.setSilent(true); rx.configure(1e6); rx.setCenterHz(dial + airbandTuning().tuneOffsetHz); rx.setChannels(list); if (g_dg) rx.setMixTap([](const float* x, size_t n) { dgAdd(x, n * 4); }); });
    }});
    b.push_back({"fm_chan20", "20 ChannelFilters 20 Msps -> 500 kHz (many-station FM front end, receivers not included)", 20e6, [](double secs) {
        Row r; r.name = "fm_chan20"; r.rate = 20e6;
        auto sig = noiseSig(20e6, std::min(secs, 2.0)); r.sigSec = sig.size() / 20e6;
        std::vector<ChannelFilter> ch(20);
        for (int k = 0; k < 20; k++) ch[k].configure(20e6, -9.5e6 + 1e6 * k, 500e3);
        std::vector<cf32> out; out.reserve(1 << 15);
        const double c0 = cpuNow();
        for (size_t i = 0; i < sig.size(); i += 16384) {
            const size_t n = std::min<size_t>(16384, sig.size() - i);
            for (auto& c : ch) { out.clear(); c.process(sig.data() + i, n, out); r.items += out.size(); }
        }
        r.cpu = cpuNow() - c0;
        return r;
    }});
    b.push_back({"fm500", "FM broadcast receiver (stereo, RDS) at 500 ksps (one station of a channelised bank)", 500e3, [](double secs) {
        FmGenConfig g; g.rate = 500e3; FmGenerator gen(g);
        std::vector<cf32> sig; gen.generate((size_t)(secs * 500e3), sig);
        return feedRx<FmReceiver>("fm500", "", 500e3, sig, [](FmReceiver& rx, long long&) { rx.configure(500e3); rx.setSilent(true); if (g_dg) rx.setAudioTap([](const float* l, const float* r, size_t n) { dgAdd(l, n * 4); dgAdd(r, n * 4); }); });
    }});
    return b;
}

} // namespace

int main(int argc, char** argv) {
    double secs = 4;
    int repeat = 1;   // --repeat N: every row N times, the figures summed (long enough to profile a fast kernel with `sample`)
    int best = 1;     // --best N: every row N times, the run with the lowest cost is shown (the least disturbed by other jobs)
    std::vector<std::string> only;
    bool list = false, md = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            std::string s = argv[++i]; size_t p = 0;
            while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); only.push_back(s.substr(p, q - p)); p = q + 1; }
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--best") && i + 1 < argc) best = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--md")) md = true;
        else if (!strcmp(argv[i], "--digest")) g_dg = true;
    }
    auto all = benches();
    if (list) { for (auto& b : all) printf("%-14s %s\n", b.name.c_str(), b.what.c_str()); return 0; }
    if (md) printf("| Item | What | Rate (Msps) | Signal (s) | CPU (s) | Cost (CPU s / signal s) | Items |\n|---|---|--:|--:|--:|--:|--:|\n");
    else printf("%-14s %8s %8s %8s %10s %10s\n", "item", "Msps", "sig s", "cpu s", "cost", "items");
    for (auto& b : all) {
        if (!only.empty() && std::find(only.begin(), only.end(), b.name) == only.end()) continue;
        auto once = [&] {
            Row r = b.run(secs);
            for (int k = 1; k < repeat; k++) { Row x = b.run(secs); r.cpu += x.cpu; r.sigSec += x.sigSec; r.items += x.items; }
            return r;
        };
        Row r = once();
        for (int k = 1; k < best; k++) { Row x = once(); if (x.cpu / x.sigSec < r.cpu / r.sigSec) r = x; }
        const double cost = r.sigSec > 0 ? r.cpu / r.sigSec : 0;
        if (md) printf("| `%s` | %s | %.3g | %.1f | %.2f | %.4f | %lld |\n", b.name.c_str(), b.what.c_str(), b.rate / 1e6, r.sigSec, r.cpu, cost, r.items);
        else printf("%-14s %8.3f %8.1f %8.2f %10.4f %10lld\n", b.name.c_str(), b.rate / 1e6, r.sigSec, r.cpu, cost, r.items);
        fflush(stdout);
    }
    return 0;
}
