// The mode registry of sens_sweep: for every mode with a built-in test signal, the clean signal generator, the receiver (set up as the engine sets it
// up) and the measure of "valid output": the receiver's own count of units whose check passed (CRC, Reed-Solomon, LDPC/BCH, FIB CRC ...).
// Pattern and receiver set-up are those of tests/engine/test_iq_modes.cpp.
#pragma once
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/atsc/atsc_gen.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2gen.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/fusion/fusion_rx.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/hrpt/hrpt_tel.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/globalstar/globalstar_tel.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include "dect2/orbcomm/orbcomm_tel.h"
#include "dect2/argos/argos_rx.h"
#include "dect2/argos/argos_tel.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/halow/halow_tel.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/zwave/zwave_tel.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/sigfox/sigfox_tel.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/inmc/inmc_rx.h"
#include "dect2/inmc/inmc_tel.h"
#include "sens_impair.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sens {
using namespace dect2;

struct Metric { double good = 0, bad = 0; };   // cumulative: units with a good check, units that failed theirs

// One receiver instance fed with one trial
struct Session {
    virtual ~Session() = default;
    virtual void feed(const cf32* x, size_t n) = 0;
    virtual Metric poll() = 0;
    virtual void settle() {}     // threaded receivers: give the decoder threads a moment to catch up with what was fed
    virtual void finish() {}     // end of the signal: flush and wait for the decoder threads
};
using Factory = std::function<std::unique_ptr<Session>(double rate)>;

struct Mode {
    std::string id, name, family;
    int stdMode = 0;
    double rate = 2e6;      // sample rate of the test signal and of the receiver
    double secs = 2;        // signal given to the receiver per trial
    double bwHz = 10e3;     // noise bandwidth the SNR refers to
    double mhz = 100;       // carrier frequency (turns radio ppm into Hz)
    double echoUs = 5;      // delay of the 2 path echo (about half the mode's guard interval / symbol)
    double fdHz = 10;       // Doppler of the fading profiles
    double snrHi = 30;      // where a sweep starts
    double pnHz = 50;       // linewidth of the phase noise scenarios (a mode with a narrow subcarrier spacing needs a realistic one)
    int maxJobs = 8;        // trials at once (big receivers hold a lot of memory)
    bool heavy = false;     // needs tens of seconds of signal: only with --heavy
    std::function<Sig(size_t n)> gen;   // the clean signal, noise free; at least n samples
    Factory make;
};

inline Sig clean(int stdMode, SynthConfig c, double rate, size_t n) {
    c.snrDb = 120;
    auto s = makeModeSynth(stdMode, c, rate);
    Sig v(n);
    if (!s) return v;
    for (size_t o = 0; o < n; o += 65536) s->generate(v.data() + o, std::min<size_t>(65536, n - o));
    return v;
}
inline void scaleTo(Sig& v, double rms) {
    const double r = std::sqrt(std::max(1e-30, meanPower(v.data(), v.size())));
    const float g = (float)(rms / r);
    for (auto& x : v) x *= g;
}

template <class Rx, class Tel>
struct Sess : Session {
    Rx rx;
    std::function<Metric(const Tel&)> measure;
    std::function<void(Rx&)> fin;
    bool threaded = false;
    Metric poll() override { Tel t; rx.telemetry(t, 0); return measure(t); }
    void feed(const cf32* x, size_t n) override { rx.feed(x, n); }
    void settle() override {
        if (!threaded) return;
        Metric last = poll();
        for (int i = 0, still = 0; i < 40 && still < 3; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const Metric m = poll();
            still = (m.good == last.good && m.bad == last.bad) ? still + 1 : 0;
            last = m;
        }
    }
    void finish() override {
        if constexpr (requires { rx.flush(); }) rx.flush();
        if (threaded) {
            Metric last = poll();
            for (int i = 0, still = 0; i < 1000 && still < 60; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const Metric m = poll();
                still = (m.good == last.good && m.bad == last.bad) ? still + 1 : 0;
                last = m;
            }
        }
        if (fin) fin(rx);
    }
};

template <class Rx, class Tel>
Factory factory(std::function<void(Rx&, double)> prep, std::function<Metric(const Tel&)> measure, bool threaded = false, std::function<void(Rx&)> fin = nullptr) {
    return [=](double rate) -> std::unique_ptr<Session> {
        auto s = std::make_unique<Sess<Rx, Tel>>();
        s->measure = measure; s->threaded = threaded; s->fin = fin;
        prep(s->rx, rate);
        return s;
    };
}

template <class Tel>
Metric blocks(const Tel& t) { return {(double)t.blocksOk, (double)t.blocksBad}; }

// the usual set-up: silent, configured, the tune offset of the mode mixed out as the engine does for a radio
template <class Rx>
void prepGeneric(Rx& rx, double rate, int stdMode) {
    if constexpr (requires { rx.setSilent(true); }) rx.setSilent(true);
    rx.configure(rate);
    const ModeTuning* mt = modeTuning(stdMode);
    if (mt && mt->tuneOffsetHz != 0) {
        if constexpr (requires { rx.setSignalOffset(0.0); }) rx.setSignalOffset(-mt->tuneOffsetHz);
    }
    if constexpr (requires { rx.setCenterHz(0.0); }) rx.setCenterHz(mt ? mt->defMhz * 1e6 : 0.0);
}

// Power of the strongest sinusoid within +-searchHz of hz in x (mean removed, n samples at 48 kHz). A sample clock error of a few tens of ppm moves
// the 1 kHz test tone by a few hundredths of a hertz; measured at exactly 1000.000 Hz the tone leaks into "everything else" and the audio
// SNR is capped at about 27 dB, which made the 20 dB criterion fail early and read as a loss of sensitivity (FM: 7 dB, with 50 ppm).
inline double tonePower(const float* x, size_t n, double mean, double hz, double searchHz = 1.5) {
    auto at = [&](double f) {
        std::complex<double> c = 0, r = 1; const std::complex<double> st = std::polar(1.0, -2 * M_PI * f / 48000.0);
        for (size_t i = 0; i < n; i++) { c += (x[i] - mean) * r; r *= st; if ((i & 1023) == 1023) r = std::polar(1.0, -2 * M_PI * f * (double)(i + 1) / 48000.0); }
        return 2 * std::norm(c / (double)n);
    };
    double best = at(hz), bf = hz;
    for (double f = hz - searchHz; f <= hz + searchHz + 1e-9; f += 0.2) { const double v = at(f); if (v > best) { best = v; bf = f; } }
    for (double f = bf - 0.2; f <= bf + 0.2 + 1e-9; f += 0.02) { const double v = at(f); if (v > best) best = v; }
    return best;
}

// FM audio: the 1 kHz tone against everything else over the last half second of the left channel (the 3 kHz tone of the right channel leaking
// over is not noise). "Valid" = at least 20 dB.
struct FmAudio : Session {
    FmReceiver rx;
    std::vector<float> l;
    explicit FmAudio(double rate) {
        rx.setSilent(true);
        rx.configure(rate);
        rx.setAudioTap([this](const float* L, const float*, size_t n) { l.insert(l.end(), L, L + n); });
    }
    void feed(const cf32* x, size_t n) override { rx.feed(x, n); }
    Metric poll() override {
        const size_t n = 24000;
        if (l.size() < n) return {0, 0};
        const float* x = l.data() + l.size() - n;
        double mean = 0, tot = 0;
        for (size_t i = 0; i < n; i++) mean += x[i];
        mean /= (double)n;
        for (size_t i = 0; i < n; i++) tot += (x[i] - mean) * (x[i] - mean);
        tot /= (double)n;
        auto tone = [&](double hz) { return tonePower(x, n, mean, hz); };
        const double p1 = tone(1000), p3 = tone(3000);
        const double snr = 10 * std::log10(std::max(1e-15, p1) / std::max(1e-15, tot - p1 - p3));
        return {snr >= 20 ? 1.0 : 0.0, snr >= 20 ? 0.0 : 1.0};
    }
};

// AM / SSB through the general receiver: the 1 kHz tone of the synthetic station against everything else over the last half second
// ("valid" = at least 20 dB). det: the AM detector (GeneralReceiver::setAmDetector).
struct GeneralAudio : Session {
    GeneralReceiver rx;
    std::vector<float> a;
    GeneralAudio(double rate, int demod, int det) {
        rx.setSilent(true);
        rx.configure(rate);
        const ModeTuning* mt = modeTuning(57);
        rx.setSignalOffset(mt ? -mt->tuneOffsetHz : 0.0);
        rx.setDemod(demod);
        rx.setWidthHz(generalDefaultWidth(demod));
        rx.setSquelch(false, false, -100);
        rx.setAmDetector(det);
        rx.setAudioTap([this](const float* x, size_t n) { a.insert(a.end(), x, x + n); });
    }
    void feed(const cf32* x, size_t n) override { rx.feed(x, n); }
    Metric poll() override {
        const size_t n = 24000;
        if (a.size() < n) return {0, 0};
        const float* x = a.data() + a.size() - n;
        double mean = 0, tot = 0;
        for (size_t i = 0; i < n; i++) mean += x[i];
        mean /= (double)n;
        for (size_t i = 0; i < n; i++) tot += (x[i] - mean) * (x[i] - mean);
        tot /= (double)n;
        const double p1 = tonePower(x, n, mean, 1000.0);
        const double snr = 10 * std::log10(std::max(1e-15, p1) / std::max(1e-15, tot - p1));
        return {snr >= 20 ? 1.0 : 0.0, snr >= 20 ? 0.0 : 1.0};
    }
};

inline std::vector<Mode> allModes() {
    std::vector<Mode> v;
    auto add = [&](Mode m) { v.push_back(std::move(m)); };
    auto base = [](const char* id, const char* name, const char* fam, int std_, double rate, double secs, double bw, double mhz) {
        Mode m; m.id = id; m.name = name; m.family = fam; m.stdMode = std_; m.rate = rate; m.secs = secs; m.bwHz = bw; m.mhz = mhz;
        return m;
    };

    {   // DVB-T2: 8K, GI 1/8, 64-QAM 2/3 rotated
        Mode m = base("dvbt2", "DVB-T2", "ofdm-tv", 1, nativeRateHz(8), 2.0, 7.61e6, 522);
        m.pnHz = 1;   // a TV tuner: about -75 dBc/Hz at 1 kHz; the 50 Hz default is far worse than any receiver front end and no OFDM TV receiver decodes it
        m.echoUs = 14; m.maxJobs = 4; m.snrHi = 30;
        m.gen = [](size_t n) {
            TxParams tp;
            tp.s1 = 0; tp.s2field1 = 1; tp.giIdx = 2; tp.payload = true; tp.plpMod = 2; tp.plpCod = 2; tp.plpRot = true; tp.plpTi = 3;
            T2Generator g(tp);
            Sig v, f;
            while (v.size() < n) { g.nextFrame(f); v.insert(v.end(), f.begin(), f.end()); }
            v.resize(n); scaleTo(v, 0.2);
            return v;
        };
        m.make = factory<T2Receiver, RxTelemetry>([](T2Receiver& rx, double r) { rx.setComputeMode(0); rx.configure(r, 8); },
                                                  [](const RxTelemetry& t) { return Metric{(double)t.blocksOk, (double)t.blocksBad}; }, true);
        add(m);
    }
    {   // DVB-T: 8K, GI 1/8, 64-QAM 2/3
        Mode m = base("dvbt", "DVB-T", "ofdm-tv", 2, nativeRateHz(8), 1.5, 7.61e6, 522);
        m.pnHz = 1;   // a TV tuner: about -75 dBc/Hz at 1 kHz; the 50 Hz default is far worse than any receiver front end and no OFDM TV receiver decodes it
        m.echoUs = 14; m.maxJobs = 6; m.snrHi = 30;
        m.gen = [](size_t n) {
            dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi8; p.mod = dvbt::k64Qam; p.crHp = p.crLp = dvbt::kR23;
            dvbt::Generator g(p, dvbt::testTsSource());
            Sig v, s;
            while (v.size() < n) { g.nextSymbol(s); v.insert(v.end(), s.begin(), s.end()); }
            v.resize(n); scaleTo(v, 0.2);
            return v;
        };
        m.make = factory<DvbtReceiver, RxTelemetry>([](DvbtReceiver& rx, double r) { rx.configure(r, 8); },
                                                    [](const RxTelemetry& t) { return Metric{(double)(t.dvbt.rsClean + t.dvbt.rsCorrected), (double)t.dvbt.rsFailed}; });
        add(m);
    }
    {   // ATSC 8-VSB
        Mode m = base("atsc", "ATSC 1.0", "single-carrier-tv", 3, 8e6, 1.5, 5.38e6, 600);
        m.pnHz = 1;   // a TV tuner: about -75 dBc/Hz at 1 kHz; the 50 Hz default is far worse than any receiver front end and no OFDM TV receiver decodes it
        m.echoUs = 3; m.snrHi = 30;
        m.gen = [](size_t n) {
            atsc::ChannelConfig cc; cc.snrDb = 120;
            atsc::Generator g(dvbt::testTsSource(), cc, 8e6, 3);
            Sig v; v.reserve(n);
            g.generate(n, v);
            v.resize(n); scaleTo(v, 0.22);
            return v;
        };
        m.make = factory<AtscReceiver, AtscTelemetry>([](AtscReceiver& rx, double r) { rx.configure(r); rx.setBlocking(true); },
                                                      [](const AtscTelemetry& t) { return Metric{(double)(t.rsClean + t.rsCorrected), (double)t.rsFailed}; }, false);
        add(m);
    }
    {   // DAB mode I: FIBs with a good CRC
        Mode m = base("dab", "DAB", "ofdm-radio", 4, 2.048e6, 3.0, 1.536e6, 225);
        m.echoUs = 30; m.fdHz = 20; m.snrHi = 25;
        m.gen = [](size_t n) { return clean(4, SynthConfig(), 2.048e6, n); };
        m.make = factory<DabReceiver, DabTelemetry>([](DabReceiver& rx, double r) { rx.configure(r); },
                                                    [](const DabTelemetry& t) { return Metric{(double)t.fibOk, (double)t.fibBad}; });
        add(m);
    }
    {   // ISDB-T 13 segments
        Mode m = base("isdbt", "ISDB-T", "ofdm-tv", 6, 10e6, 2.0, 5.57e6, 600);
        m.pnHz = 1;   // a TV tuner: about -75 dBc/Hz at 1 kHz; the 50 Hz default is far worse than any receiver front end and no OFDM TV receiver decodes it
        m.echoUs = 10; m.maxJobs = 6; m.snrHi = 30;
        m.gen = [](size_t n) { return clean(6, SynthConfig(), 10e6, n); };
        m.make = factory<IsdbtReceiver, RxTelemetry>([](IsdbtReceiver& rx, double r) { rx.configure(r); },
                                                     [](const RxTelemetry& t) {
                                                         Metric r; for (const auto& L : t.isdbt.layer) { r.good += (double)(L.rsClean + L.rsCorrected); r.bad += (double)L.rsFailed; }
                                                         return r; });
        add(m);
    }
    {   // FM broadcast, audio
        Mode m = base("fm", "FM broadcast (audio)", "analog-radio", 7, 2e6, 3.0, 200e3, 93.9);
        m.echoUs = 1; m.snrHi = 30; m.fdHz = 10;
        m.gen = [](size_t n) { FmGenConfig c; c.rate = 2e6; c.cnrDb = 200; FmGenerator g(c); Sig v(n); g.generate(v.data(), n, 0.3f); return v; };
        m.make = [](double rate) -> std::unique_ptr<Session> { return std::make_unique<FmAudio>(rate); };
        add(m);
    }
    {   // FM broadcast, RDS groups with a good checkword
        Mode m = base("fm-rds", "FM broadcast (RDS)", "analog-radio", 7, 2e6, 4.0, 200e3, 93.9);
        m.echoUs = 1; m.snrHi = 30;
        m.gen = [](size_t n) { FmGenConfig c; c.rate = 2e6; c.cnrDb = 200; FmGenerator g(c); Sig v(n); g.generate(v.data(), n, 0.3f); return v; };
        m.make = factory<FmReceiver, FmTelemetry>([](FmReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); },
                                                  [](const FmTelemetry& t) { return Metric{(double)t.rdsGroups, 0}; });
        add(m);
    }
    {   // DVB-S2 QPSK 2/3, 5 Msym/s
        Mode m = base("dvbs2", "DVB-S2", "satellite-tv", 8, 10e6, 1.5, 5e6, 11700);
        m.echoUs = 0.4; m.snrHi = 20; m.maxJobs = 4;
        m.gen = [](size_t n) { return clean(8, SynthConfig(), 10e6, n); };
        m.make = factory<DvbsReceiver, DvbsTelemetry>([](DvbsReceiver& rx, double r) { rx.setBlocking(true); rx.configure(r); }, blocks<DvbsTelemetry>, false,
                                                      [](DvbsReceiver&) {});
        add(m);
    }
    {   // DTMB PN945 64-QAM 0.6
        Mode m = base("dtmb", "DTMB", "ofdm-tv", 9, 10e6, 2.0, 7.56e6, 600);
        m.pnHz = 1;   // a TV tuner: about -75 dBc/Hz at 1 kHz; the 50 Hz default is far worse than any receiver front end and no OFDM TV receiver decodes it
        m.echoUs = 10; m.maxJobs = 4; m.snrHi = 32;
        m.gen = [](size_t n) { return clean(9, SynthConfig(), 10e6, n); };
        m.make = factory<DtmbReceiver, DtmbTelemetry>([](DtmbReceiver& rx, double r) { rx.configure(r); }, blocks<DtmbTelemetry>, true);
        add(m);
    }
    {   // analog TV B/G with a steady tone
        Mode m = base("atv", "Analog TV (PAL B/G)", "analog-tv", 10, 10e6, 1.5, 7e6, 600);
        m.echoUs = 3; m.snrHi = 40;
        m.gen = [](size_t n) { SynthConfig c; c.modeOpt[3] = 4; return clean(10, c, 10e6, n); };
        m.make = factory<AtvReceiver, AtvTelemetry>([](AtvReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, blocks<AtvTelemetry>);
        add(m);
    }
    {   // DMR base station, busy
        Mode m = base("dmr", "DMR", "digital-voice", 11, 2.4e6, 4.0, 12.5e3, 450);
        m.snrHi = 25; m.fdHz = 20;
        m.gen = [](size_t n) { SynthConfig c; c.modeOpt[3] = 2; return clean(11, c, 2.4e6, n); };
        m.make = factory<DmrReceiver, DmrTelemetry>([](DmrReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, blocks<DmrTelemetry>);
        add(m);
    }
    {   // DRM mode B, 10 kHz
        Mode m = base("drm", "DRM", "ofdm-radio", 12, 2e6, 8.0, 10e3, 10);
        m.echoUs = 1000; m.fdHz = 2; m.snrHi = 28; m.maxJobs = 4; m.pnHz = 1;   // 46.9 Hz spacing: 50 Hz is not decodable by any receiver
        m.gen = [](size_t n) { SynthConfig c; c.modeOpt[4] = 1; c.modeOpt[5] = 1; return clean(12, c, 2e6, n); };
        m.make = factory<DrmReceiver, DrmTelemetry>([](DrmReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, blocks<DrmTelemetry>);
        add(m);
    }
    {   // ADS-B: 12 aircraft
        Mode m = base("adsb", "ADS-B", "burst", 13, 4e6, 3.0, 2e6, 1090);
        m.echoUs = 0.3; m.snrHi = 30;
        m.gen = [](size_t n) { SynthConfig c; c.modeOpt[0] = 12; return clean(13, c, 4e6, n); };
        m.make = factory<AdsbReceiver, AdsbTelemetry>([](AdsbReceiver& rx, double r) { rx.configure(r); }, blocks<AdsbTelemetry>);
        add(m);
    }
    {   // radiosondes: an RS41 on the centre, a DFM-17 and an M10 elsewhere
        Mode m = base("sonde", "Radiosonde", "narrowband-data", 15, 8e6, 5.0, 10e3, 403);
        m.snrHi = 25; m.maxJobs = 4;
        m.gen = [](size_t n) { return clean(15, SynthConfig(), 8e6, n); };
        m.make = factory<SondeReceiver, SondeTelemetry>([](SondeReceiver& rx, double r) { rx.setSynchronous(true); rx.setSignalOffset(0); rx.configure(r); rx.setCenterMhz(403); },
                                                        blocks<SondeTelemetry>, false, [](SondeReceiver& rx) { rx.flush(); });
        add(m);
    }
    {   // AIS: AIS 1 and 2
        Mode m = base("ais", "AIS", "narrowband-data", 16, 2e6, 12.0, 25e3, 162);
        m.snrHi = 30; m.maxJobs = 4;
        m.gen = [](size_t n) { return clean(16, SynthConfig(), 2e6, n); };
        m.make = factory<AisReceiver, AisTelemetry>([](AisReceiver& rx, double r) { rx.setSignalOffset(0); rx.configure(r); }, blocks<AisTelemetry>);
        add(m);
    }
    {   // ACARS: six aircraft on three channels
        Mode m = base("acars", "ACARS", "narrowband-data", 18, 2e6, 12.0, 25e3, 131.5);
        m.snrHi = 30; m.maxJobs = 4;
        m.gen = [](size_t n) { SynthConfig c; c.modeVal[0] = 6; return clean(18, c, 2e6, n); };
        m.make = factory<AcarsReceiver, AcarsTelemetry>([](AcarsReceiver& rx, double r) { rx.setSignalOffset(0); rx.setCenterHz(131.5e6); rx.configure(r); }, blocks<AcarsTelemetry>);
        add(m);
    }
    {   // Inmarsat Aero 10500 bit/s P channel
        Mode m = base("aero", "Inmarsat Aero", "satellite-data", 20, 2e6, 6.0, 12.5e3, 1545);
        m.snrHi = 25;
        m.gen = [](size_t n) { return clean(20, SynthConfig(), 2e6, n); };
        const double off = modeTuning(20) ? modeTuning(20)->tuneOffsetHz : 0;
        m.make = factory<AeroReceiver, AeroTelemetry>([off](AeroReceiver& rx, double r) { rx.setSignalOffset(-off); rx.configure(r); }, blocks<AeroTelemetry>);
        add(m);
    }
    {   // Iridium bursts
        Mode m = base("iridium", "Iridium", "burst", 21, 10e6, 2.0, 35e3, 1622);
        m.snrHi = 25; m.maxJobs = 4;
        m.gen = [](size_t n) { return clean(21, SynthConfig(), 10e6, n); };
        m.make = factory<IridiumReceiver, IridiumTelemetry>([](IridiumReceiver& rx, double r) { rx.setSignalOffset(0); rx.setOffline(true); rx.configure(r); rx.setCenterMhz(1622); },
                                                            blocks<IridiumTelemetry>, false, [](IridiumReceiver& rx) { rx.flush(); });
        add(m);
    }
    {   // mobile-satellite and ISM data modes (satdata family): the clean test signal of the mode, the receiver set up by prepGeneric
        struct S { const char* id; const char* name; int std_; double secs, bw, snrHi; };
        auto addGen = [&](auto tag, const S& d) {
            using Rx = typename decltype(tag)::Rx; using Tel = typename decltype(tag)::Tel;
            const ModeTuning* mt = modeTuning(d.std_);
            if (!mt) return;
            Mode m = base(d.id, d.name, "satdata", d.std_, mt->sampleRate, d.secs, d.bw, mt->defMhz);
            m.snrHi = d.snrHi; m.maxJobs = 4;
            const double rate = mt->sampleRate; const int sd = d.std_;
            m.gen = [rate, sd](size_t n) { return clean(sd, SynthConfig(), rate, n); };
            m.make = factory<Rx, Tel>([sd](Rx& rx, double r) { prepGeneric(rx, r, sd); }, blocks<Tel>);
            add(m);
        };
        struct T { };
        #define SATDATA_TAG(NAME, RX, TEL) struct NAME { using Rx = RX; using Tel = TEL; };
        SATDATA_TAG(TGlobalstar, GlobalstarReceiver, GlobalstarTelemetry)
        SATDATA_TAG(TOrbcomm, OrbcommReceiver, OrbcommTelemetry)
        SATDATA_TAG(TArgos, ArgosReceiver, ArgosTelemetry)
        SATDATA_TAG(TSarsat, SarsatReceiver, SarsatTelemetry)
        SATDATA_TAG(TWisun, WisunReceiver, WisunTelemetry)
        SATDATA_TAG(THalow, HalowReceiver, HalowTelemetry)
        SATDATA_TAG(TZigbee, ZigbeeReceiver, ZigbeeTelemetry)
        SATDATA_TAG(TZwave, ZwaveReceiver, ZwaveTelemetry)
        SATDATA_TAG(TSigfox, SigfoxReceiver, SigfoxModeTelemetry)
        SATDATA_TAG(TBle, BleReceiver, BleTelemetry)
        SATDATA_TAG(TInmc, InmcReceiver, InmcTelemetry)
        #undef SATDATA_TAG
        addGen(TGlobalstar(), {"globalstar", "Globalstar", 64, 4.0, 1.23e6, 25});
        addGen(TOrbcomm(), {"orbcomm", "Orbcomm", 46, 6.0, 10e3, 25});
        addGen(TArgos(), {"argos", "Argos", 154, 6.0, 5e3, 25});
        addGen(TSarsat(), {"sarsat", "COSPAS-SARSAT", 94, 6.0, 3e3, 25});
        addGen(TWisun(), {"wisun", "Wi-SUN FAN", 86, 4.0, 200e3, 25});
        addGen(THalow(), {"halow", "Wi-Fi HaLow", 72, 4.0, 1e6, 30});
        addGen(TZigbee(), {"zigbee", "Zigbee", 67, 4.0, 2e6, 25});
        addGen(TZwave(), {"zwave", "Z-Wave", 100, 4.0, 100e3, 25});
        addGen(TSigfox(), {"sigfox", "Sigfox", 155, 6.0, 600, 25});
        addGen(TBle(), {"ble", "Bluetooth LE", 53, 4.0, 1e6, 25});
        addGen(TInmc(), {"inmc", "Inmarsat-C", 19, 40.0, 1.2e3, 25});
    }
    {   // Meshtastic LongFast
        Mode m = base("mesh", "Meshtastic / MeshCore LoRa", "lora", 22, 2e6, 15.0, 250e3, 869.5);
        m.snrHi = 20; m.heavy = true; m.maxJobs = 2;
        m.gen = [](size_t n) { SynthConfig c; c.modeVal[0] = 5; return clean(22, c, 2e6, n); };
        const double off = modeTuning(22) ? modeTuning(22)->tuneOffsetHz : 0;
        m.make = factory<MeshReceiver, MeshTelemetry>([off](MeshReceiver& rx, double r) { rx.setSignalOffset(-off); rx.configure(r); }, blocks<MeshTelemetry>);
        add(m);
    }
    // The table driven narrowband modes: the same set-up for all (see prepGeneric); the synthetic option in the lambda is the test signal the
    // mode's own engine test uses.
    auto narrow = [&]<class Rx, class Tel>(const char* id, const char* name, const char* fam, int std_, double rate, double secs, double bw, double mhz,
                                           std::function<void(SynthConfig&)> opt, double snrHi) {
        Mode m = base(id, name, fam, std_, rate, secs, bw, mhz);
        m.snrHi = snrHi; m.maxJobs = 4;
        m.gen = [std_, opt](size_t n) { SynthConfig c; if (opt) opt(c); const ModeTuning* t = modeTuning(std_); return clean(std_, c, t ? t->sampleRate : 2e6, n); };
        const ModeTuning* t = modeTuning(std_);
        m.rate = t ? t->sampleRate : rate;
        m.make = factory<Rx, Tel>([std_](Rx& rx, double r) { prepGeneric(rx, r, std_); }, blocks<Tel>);
        add(m);
    };
    {   // AM broadcast (general receiver), envelope and synchronous detector; USB voice
        struct G { const char* id; const char* name; int demod, det; double bw; };
        for (const G g : {G{"am", "AM (envelope)", GenAm, 0, 9e3}, G{"am-sync", "AM (synchronous)", GenAm, 1, 9e3}, G{"ssb", "SSB (USB)", GenUsb, 0, 2.7e3}}) {
            Mode m = base(g.id, g.name, "analog-radio", 57, 1e6, 3.0, g.bw, 7.2);
            m.echoUs = 300; m.fdHz = 1; m.snrHi = 40;
            const int dm = g.demod, dt = g.det;
            m.gen = [dm](size_t n) { SynthConfig c; c.modeOpt[0] = dm; return clean(57, c, 1e6, n); };
            m.make = [dm, dt](double rate) -> std::unique_ptr<Session> { return std::make_unique<GeneralAudio>(rate, dm, dt); };
            add(m);
        }
    }
    {   // HD Radio: FM hybrid MP1 and AM hybrid MA1 (P1 transfer frames with a valid header)
        for (int am = 0; am < 2; am++) {
            Mode m = base(am ? "hdr-am" : "hdr-fm", am ? "HD Radio AM (MA1)" : "HD Radio FM (MP1)", "ofdm-radio", 23, 2e6, 6.0, am ? 30e3 : 400e3, am ? 1.0 : 98.1);
            m.echoUs = am ? 100 : 3; m.snrHi = am ? 40 : 30; m.maxJobs = 4;
            m.secs = am ? 20.0 : 10.0;   // AM gives its first P1 frame about 7 s after sync (4 frames of diversity delay); FM: ~6 frames to count
            m.pnHz = am ? 3 : 10;        // 363 Hz (FM) / 181 Hz (AM) spacing
            m.gen = [am](size_t n) { SynthConfig c; c.modeOpt[0] = am; return clean(23, c, 2e6, n); };
            m.make = factory<HdrReceiver, HdrTelemetry>([](HdrReceiver& rx, double r) { rx.setSilent(true); rx.configure(r); }, blocks<HdrTelemetry>);
            add(m);
        }
    }
    narrow.template operator()<PagerReceiver, PagerTelemetry>("pager", "POCSAG / FLEX pagers", "narrowband-data", 25, 1.2e6, 8.0, 25e3, 466, nullptr, 25);
    narrow.template operator()<PacketReceiver, PacketTelemetry>("packet", "APRS / AX.25 packet", "narrowband-data", 26, 1.2e6, 6.0, 25e3, 144.8, [](SynthConfig& c) { c.modeOpt[0] = 0; }, 25);
    narrow.template operator()<Vdl2Receiver, Vdl2Telemetry>("vdl2", "VDL Mode 2", "narrowband-data", 29, 2e6, 8.0, 25e3, 136.9, [](SynthConfig& c) { c.modeVal[0] = 3; }, 28);
    narrow.template operator()<DstarReceiver, DstarTelemetry>("dstar", "D-STAR", "digital-voice", 35, 1.2e6, 6.0, 6.25e3, 145, nullptr, 25);
    narrow.template operator()<NxdnReceiver, NxdnTelemetry>("nxdn", "NXDN", "digital-voice", 40, 1.2e6, 6.0, 6.25e3, 450, nullptr, 25);
    narrow.template operator()<DpmrReceiver, DpmrTelemetry>("dpmr", "dPMR", "digital-voice", 47, 1.2e6, 6.0, 6.25e3, 446, nullptr, 25);
    narrow.template operator()<TetraReceiver, TetraTelemetry>("tetra", "TETRA", "digital-voice", 48, 2e6, 8.0, 25e3, 390, [](SynthConfig& c) { c.modeOpt[0] = 1; }, 22);
    narrow.template operator()<M17Receiver, M17Telemetry>("m17", "M17", "digital-voice", 33, 2e6, 6.0, 9e3, 433, nullptr, 25);
    narrow.template operator()<FusionReceiver, FusionTelemetry>("fusion", "Yaesu System Fusion", "digital-voice", 130, 2e6, 6.0, 12.5e3, 144, nullptr, 25);
    narrow.template operator()<P25p2Receiver, P25p2Telemetry>("p25p2", "P25 phase 2", "digital-voice", 182, 2e6, 6.0, 12.5e3, 851, nullptr, 25);
    narrow.template operator()<TetrapolReceiver, TetrapolTelemetry>("tetrapol", "TETRAPOL", "digital-voice", 81, 2e6, 6.0, 12.5e3, 400, nullptr, 25);
    narrow.template operator()<P25Receiver, P25Telemetry>("p25", "P25 phase 1", "digital-voice", 52, 1.2e6, 6.0, 12.5e3, 851, nullptr, 25);
    // weather satellites: the SNR is Es/N0 (the noise bandwidth is the symbol rate); frames whose Reed-Solomon codewords all passed
    narrow.template operator()<MeteorReceiver, MeteorTelemetry>("meteor", "Meteor-M LRPT (OQPSK 72k)", "satellite-weather", 61, 1e6, 8.0, 72e3, 137.9, nullptr, 16);
    narrow.template operator()<HrptReceiver, HrptTelemetry>("hrpt", "MetOp AHRPT", "satellite-weather", 66, 8e6, 1.5, 2.33e6, 1701.3, nullptr, 16);
    narrow.template operator()<GoesReceiver, GoesTelemetry>("goes", "GOES HRIT", "satellite-weather", 62, 2.4e6, 4.0, 927e3, 1694.1, nullptr, 16);
    return v;
}

} // namespace sens
