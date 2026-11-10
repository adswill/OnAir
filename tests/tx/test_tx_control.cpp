// Full control over transmitting (docs/TX-CONTROL-3.8.md): the per-driver parameter mapping with fake radios that record every call, the per-radio
// calibration (ppm, offset, DC, I/Q balance, power vs gain table), the IQ correction, the timeout timer, the schedule and DCS on analog FM.
// Everything is generated into memory and sent to recording sinks; NOTHING is transmitted.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_control.h"
#include "dect2/audio/audio_control.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- a fake radio that records the calls its driver makes ----------------------------------------------------------------------------------
struct RecordingOps : TxHwOps {
    std::vector<std::string> calls;
    std::string refuse;   // the call this radio refuses ("freq", "gain", ...)
    double rateHz = 0, filterHz = 0, freqHz = 0; uint32_t num = 0, div = 0; int amp = -1;
    std::vector<std::pair<std::string, double>> gains;
    bool rec(const std::string& what, std::string& err) {
        calls.push_back(what);
        if (what.rfind(refuse, 0) == 0 && !refuse.empty()) { err = "the fake radio refused " + what; return false; }
        return true;
    }
    bool setRate(const TxHwPlan& p, std::string& err) override { rateHz = p.rateHz; num = p.rateNum; div = p.rateDiv; return rec("rate", err); }
    bool setFilter(double hz, std::string& err) override { filterHz = hz; return rec("filter", err); }
    bool setFrequency(double hz, std::string& err) override { freqHz = hz; return rec("freq", err); }
    bool setGain(const std::string& s, double db, std::string& err) override { gains.push_back({s, db}); return rec("gain:" + s, err); }
    bool setAmp(bool on, std::string& err) override { amp = on ? 1 : 0; return rec("amp", err); }
};

// a radio sink that records what it was opened / retuned with and the samples it was given
struct RecordingSink : TxSink {
    struct Rec { std::mutex mu; std::vector<TxTune> opens, retunes; std::vector<cf32> iq; size_t n = 0; };
    std::shared_ptr<Rec> r = std::make_shared<Rec>();
    double rate = 0;
    bool open(const TxTune& t, std::string&) override { rate = t.sampleRate; std::lock_guard<std::mutex> l(r->mu); r->opens.push_back(t); return true; }
    bool write(const cf32* x, size_t n, std::string&) override { std::lock_guard<std::mutex> l(r->mu); r->iq.insert(r->iq.end(), x, x + n); r->n += n; return true; }
    bool retune(const TxTune& t, std::string&) override { std::lock_guard<std::mutex> l(r->mu); r->retunes.push_back(t); return true; }
    void close() override {}
    double sampleRate() const override { return rate; }
    bool realtime() const override { return false; }
};

static TxTune tune(double hz, double rate, double gain) { TxTune t; t.centerHz = hz; t.sampleRate = rate; t.gainDb = gain; return t; }

// ---- the per-driver plans and their calls ---------------------------------------------------------------------------------------------------
static void hackrfChecks() {
    // every combination of rate, gain, amplifier, ppm, offset and filter: the calls, their order and their values
    const double rates[] = {2e6, 8e6, 10e6, 20e6, 64e6 / 7, 20e6 / 3};
    const double gainsIn[] = {-10, 0, 0.4, 0.6, 23.5, 47, 47.4, 60};
    const double ppms[] = {0, -12.5, 3};
    const double offs[] = {0, 1.25e3, -455e3};
    const double bws[] = {0, 1.75e6, 28e6};
    int combos = 0;
    for (double rate : rates) for (double g : gainsIn) for (int amp = 0; amp < 2; amp++) for (double ppm : ppms) for (double off : offs) for (double bw : bws) {
        TxTune t = tune(433.92e6, rate, g);
        t.ampOn = amp; t.offsetHz = off; t.bandwidthHz = bw;
        if (ppm != 0) t.radio["ppm"] = std::to_string(ppm);
        RecordingOps ops;
        std::string err;
        const bool ok = txHwApply(hackrfTxPlan(t), ops, true, err);
        combos++;
        CHECK(ok, "hackrf %g %g: %s", rate, g, err.c_str());
        CHECK(ops.calls.size() == 5 && ops.calls[0] == "rate" && ops.calls[1] == "filter" && ops.calls[2] == "freq" && ops.calls[3] == "gain:TXVGA" && ops.calls[4] == "amp", "hackrf call order");
        CHECK(near((double)ops.num / ops.div, rate, 1e-3) && ops.div >= 1 && ops.div < 32, "hackrf rate %u/%u for %.6f", ops.num, ops.div, rate);
        const double want = std::floor((433.92e6 + off) / (1 + ppm * 1e-6));
        CHECK(near(ops.freqHz, want, 1.0), "hackrf frequency %.1f want %.1f (ppm %g offset %g)", ops.freqHz, want, ppm, off);
        CHECK(ops.gains.size() == 1 && ops.gains[0].second == std::min(47.0, std::max(0.0, (double)std::lround(g))), "hackrf TX VGA %g for %g", ops.gains[0].second, g);
        CHECK(ops.amp == amp, "hackrf amplifier");
        if (bw > 0) CHECK(ops.filterHz == bw, "hackrf filter as asked: %g", ops.filterHz);
        else CHECK(ops.filterHz > 0 && ops.filterHz <= rate, "hackrf automatic filter %g at %g", ops.filterHz, rate);
    }
    CHECK(combos == 6 * 8 * 2 * 3 * 3 * 3, "combinations %d", combos);
    // a live retune: no rate and no filter call
    {
        RecordingOps ops; std::string err;
        CHECK(txHwApply(hackrfTxPlan(tune(145e6, 2e6, 10)), ops, false, err) && ops.calls.size() == 3 && ops.calls[0] == "freq", "hackrf retune: frequency, gain, amplifier only");
    }
    // the radio refuses a call: nothing after it is sent, its text is the error
    {
        RecordingOps ops; ops.refuse = "freq"; std::string err;
        CHECK(!txHwApply(hackrfTxPlan(tune(7e9, 2e6, 10)), ops, true, err) && ops.calls.size() == 3 && err.find("refused freq") != std::string::npos, "hackrf refusal stops at the call: %s", err.c_str());
    }
    // the plan itself refuses only what the radio's request format cannot hold; one output only
    {
        RecordingOps ops; std::string err;
        TxTune t = tune(1e6, 2e6, 0); t.channel = 1;
        CHECK(!txHwApply(hackrfTxPlan(t), ops, true, err) && ops.calls.empty() && err.find("one transmit output") != std::string::npos, "hackrf TX2 refused before any call");
        CHECK(!hackrfTxPlan(tune(5e18, 2e6, 0)).err.empty(), "a frequency beyond the request format is refused in words");
        const TxHwPlan neg = hackrfTxPlan(tune(-5e6, 2e6, 0));
        CHECK(neg.err.empty() && neg.loHz == 0 && !neg.note.empty(), "a negative frequency becomes 0 Hz with a note, not a refusal");
        // OnAir's own limits: none. Any frequency the format holds, any band, is planned as asked
        for (double hz : {1.0, 26.965e6, 121.5e6, 1575.42e6, 5.8e9, 7.25e9, 1e12}) CHECK(hackrfTxPlan(tune(hz, 2e6, 0)).err.empty(), "no frequency refusal at %g", hz);
    }
}

static void plutoUsrpSoapyChecks() {
    // PlutoSDR: the filter between 0.2 and 56 MHz, the crystal correction, the gain to the radio's reported range (attenuation as a negative gain)
    for (double rate : {2.083e6, 4e6, 30.72e6, 61.44e6}) for (double bw : {0.0, 0.1e6, 5e6, 80e6}) for (double g : {-120.0, -89.75, -30.0, 0.0, 10.0}) for (double xo : {0.0, 25.0}) {
        TxTune t = tune(2400e6, rate, g); t.bandwidthHz = bw; t.radio["ppm"] = "2";
        const TxHwPlan p = plutoTxPlan(t, rate, {}, xo);
        CHECK(p.err.empty(), "pluto plan: %s", p.err.c_str());
        const double f = bw > 0 ? bw : rate * 0.95;
        CHECK(near(p.filterHz, std::round(std::min(56e6, std::max(0.2e6, f))), 1), "pluto filter %g", p.filterHz);
        CHECK(near(p.loHz, std::round(2400e6 / (1 + 2e-6) * (1 + xo * 1e-6)), 1), "pluto LO %.0f", p.loHz);
        CHECK(p.gains.size() == 1 && p.gains[0].first == "hardwaregain" && p.gains[0].second == std::min(0.0, std::max(-89.75, g)), "pluto gain %g", p.gains[0].second);
    }
    {
        const TxHwPlan p = plutoTxPlan(tune(915e6, 4e6, -10), 4e6, {-40, -5}, 0);
        CHECK(p.gains[0].second == -10, "pluto gain inside the reported range kept");
        CHECK(plutoTxPlan(tune(915e6, 4e6, -1), 4e6, {-40, -5}, 0).gains[0].second == -5, "pluto gain clamped to the radio's reported top");
        CHECK(!plutoTxPlan(tune(915e6, 1e6, 0), 1e6, {}, 0).err.empty(), "pluto below its converter's rate: the driver's words");
        TxTune t2 = tune(915e6, 4e6, 0); t2.channel = 2;
        CHECK(!plutoTxPlan(t2, 4e6, {}, 0).err.empty(), "pluto TX3 does not exist");
        // in full duplex the receiver's rate sets the automatic filter
        CHECK(near(plutoTxPlan(tune(915e6, 4e6, 0), 10e6, {}, 0).filterHz, 9.5e6, 1), "pluto filter from the running rate");
    }
    // USRP: default and lowest rate, the TX/RX connector, the overall gain to the reported range
    {
        const TxHwPlan a = usrpTxPlan(tune(1.2e9, 0, 50), 0, {});
        CHECK(a.rateHz == 1e6 && a.antenna == "TX/RX" && a.gains.size() == 1 && a.gains[0].first.empty() && a.gains[0].second == 50, "usrp defaults");
        CHECK(usrpTxPlan(tune(1.2e9, 0.05e6, 0), 0, {}).rateHz == 0.2e6, "usrp lowest rate");
        CHECK(usrpTxPlan(tune(1.2e9, 1e6, 120), 1e6, {0, 76}).gains[0].second == 76, "usrp gain clamped to the reported range only");
        TxTune t = tune(1.2e9, 4e6, 10); t.offsetHz = -2e3; t.radio["ppm"] = "-1";
        CHECK(near(usrpTxPlan(t, 4e6, {}).loHz, (1.2e9 - 2e3) / (1 - 1e-6), 1e-3), "usrp offset and ppm");
        RecordingOps ops; std::string err;
        CHECK(txHwApply(usrpTxPlan(t, 4e6, {}), ops, false, err) && ops.calls.size() == 2 && ops.calls[0] == "freq" && ops.calls[1] == "gain:", "usrp retune calls");
    }
    // SoapySDR (LimeSDR, bladeRF, ...): the overall gain, then the typed stages clamped to their own ranges; an unknown stage is asked anyway with a note
    {
        TxTune t = tune(433e6, 2e6, 30);
        CHECK(txParseStageGains("PAD=52, IAMP = -3,  ", t.radio), "stage gains parsed");
        CHECK(!txParseStageGains("PAD", t.radio) && !txParseStageGains("PAD=x", t.radio), "bad stage text refused (the old values stay)");
        CHECK(txStageGainsText(t.radio) == "IAMP=-3, PAD=52", "stage gains text: %s", txStageGainsText(t.radio).c_str());
        std::map<std::string, TxHwRange> st = {{"PAD", {0, 52}}, {"IAMP", {-12, 12}}};
        TxHwPlan p = soapyTxPlan(t, 2e6, {-12, 64}, st);
        CHECK(p.gains.size() == 3 && p.gains[0].first.empty() && p.gains[0].second == 30, "soapy overall gain first");
        CHECK(p.gains[1].first == "IAMP" && p.gains[1].second == -3 && p.gains[2].first == "PAD" && p.gains[2].second == 52 && p.note.empty(), "soapy stages");
        t.radio["txgain.PAD"] = "70";
        CHECK(soapyTxPlan(t, 2e6, {-12, 64}, st).gains[2].second == 52, "soapy stage clamped to its range");
        t.radio["txgain.dsa"] = "-20";
        p = soapyTxPlan(t, 2e6, {}, st);
        CHECK(p.gains.size() == 4 && !p.note.empty() && p.err.empty(), "soapy unknown stage: asked anyway, with a note");
        CHECK(txParseStageGains("", t.radio) && txStageGains(t.radio).empty(), "empty text removes the stages");
        t.bandwidthHz = 0;
        CHECK(near(soapyTxPlan(t, 5e6, {}, {}).filterHz, 4.75e6, 1), "soapy automatic filter from the running rate");
        CHECK(soapyTxPlan(tune(433e6, 2e6, 500), 2e6, {}, {}).gains[0].second == 500, "soapy unknown range: the gain is passed as typed");
    }
}

// ---- calibration ---------------------------------------------------------------------------------------------------------------------------
static void calibrationChecks() {
    TxCalibration c;
    c.ppm = -1.7; c.offsetHz = 312.5; c.dcI = 0.0125; c.dcQ = -0.004; c.iqGainDb = 0.3; c.iqPhaseDeg = -1.5;
    c.power = {{0, false, -40}, {20, false, -20.5}, {47, false, 1}, {0, true, -27}, {47, true, 13}};
    const TxCalibration d = txCalFromString(txCalToString(c));
    CHECK(d.ppm == c.ppm && d.offsetHz == c.offsetHz && d.dcI == c.dcI && d.dcQ == c.dcQ && d.iqGainDb == c.iqGainDb && d.iqPhaseDeg == c.iqPhaseDeg && d.power.size() == 5, "calibration round trip: %s", txCalToString(c).c_str());
    CHECK(txCalFromString("").empty() && txCalFromString("ppm=x;junk;pw=1:0").empty(), "bad text gives an empty calibration");
    // the power table: interpolation, extrapolation, inverse, per amplifier setting
    CHECK(near(txCalPowerDbm(c, 10, false), -30.25, 1e-9), "interpolated %g", txCalPowerDbm(c, 10, false));
    CHECK(near(txCalPowerDbm(c, 47, false), 1, 1e-9) && near(txCalPowerDbm(c, 0, true), -27, 1e-9), "measured points exact");
    CHECK(near(txCalPowerDbm(c, -10, false), -49.75, 1e-6), "extrapolated below along the first slope");
    CHECK(near(txCalPowerDbm(c, 23.5, true), -7, 1e-9), "the amplifier's own table");
    for (double g = -5; g <= 52; g += 0.5) for (int amp = 0; amp < 2; amp++) {
        const double p = txCalPowerDbm(c, g, amp);
        CHECK(near(txCalGainForDbm(c, p, amp), g, 1e-6), "inverse at gain %g amp %d", g, amp);
    }
    TxCalibration one; one.power = {{10, false, -15}};
    CHECK(near(txCalPowerDbm(one, 13, false), -12, 1e-9) && std::isnan(txCalPowerDbm(one, 13, true)) && near(txCalGainForDbm(one, -20, false), 5, 1e-9), "one point: 1 dB per dB");
    CHECK(std::isnan(txCalPowerDbm(TxCalibration(), 0, false)), "no table: NaN");
    // into a request: ppm and offset to the radio, the IQ terms to the output stage; set values win
    TxRequest r; r.tune.centerHz = 145.5e6; r.tune.offsetHz = 100;
    txApplyCalibration(c, r);
    CHECK(r.tune.radio["ppm"] == "-1.7" && near(r.tune.offsetHz, 412.5, 1e-9), "ppm %s offset %g", r.tune.radio["ppm"].c_str(), r.tune.offsetHz);
    CHECK(r.o("out.dcI", 0) == c.dcI && r.o("out.iqPhaseDeg", 0) == c.iqPhaseDeg, "IQ terms in the request");
    TxRequest r2; r2.tune.radio["ppm"] = "4"; r2.opt["out.dcI"] = 0.5;
    txApplyCalibration(c, r2);
    CHECK(r2.tune.radio["ppm"] == "4" && r2.o("out.dcI", 0) == 0.5, "an explicit value is not overwritten by the calibration");
    TxRequest r3; txApplyCalibration(TxCalibration(), r3);
    CHECK(r3.opt.empty() && r3.tune.radio.empty() && r3.tune.offsetHz == 0, "an empty calibration changes nothing");
    // the whole path: calibration -> request -> TxSession -> the radio's open() -> each driver's plan
    {
        TxRequest q; q.modeId = "fm"; q.tune.centerHz = 100e6; q.tune.sampleRate = 2e6; q.tune.gainDb = 20; q.loop = false; q.onceSeconds = 0.05;
        txApplyCalibration(c, q);
        auto sink = std::make_unique<RecordingSink>();
        auto rec = sink->r;
        TxSession s; std::string err;
        CHECK(s.start(std::move(sink), q, {}, err), "start: %s", err.c_str());
        for (int i = 0; i < 400 && s.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TxTune t2 = q.tune; t2.centerHz = 101e6; t2.gainDb = 25;
        s.stop();
        std::lock_guard<std::mutex> l(rec->mu);
        CHECK(rec->opens.size() == 1, "one open");
        if (!rec->opens.empty()) {
            const TxTune& o = rec->opens[0];
            const double want = (100e6 + 312.5) / (1 - 1.7e-6);
            TxTune o4 = o; o4.sampleRate = 4e6;   // a PlutoSDR runs from 2.083 Msps
            CHECK(near(hackrfTxPlan(o).loHz, std::floor(want), 1.0), "hackrf LO %.1f want %.1f", hackrfTxPlan(o).loHz, want);
            CHECK(near(usrpTxPlan(o, 2e6, {}).loHz, want, 1e-3) && near(soapyTxPlan(o, 2e6, {}, {}).loHz, want, 1e-3) && near(plutoTxPlan(o4, 4e6, {}, 0).loHz, std::round(want), 1), "every driver gets the calibrated frequency: usrp %.3f soapy %.3f pluto %.1f want %.3f (offset %.3f ppm %s)", usrpTxPlan(o, 2e6, {}).loHz, soapyTxPlan(o, 2e6, {}, {}).loHz, plutoTxPlan(o4, 4e6, {}, 0).loHz, want, o.offsetHz, o.radio.count("ppm") ? o.radio.at("ppm").c_str() : "-");
        }
    }
}

// ---- IQ correction -------------------------------------------------------------------------------------------------------------------------
// a radio's mixer with gain and phase error and a carrier leak (the model the user calibrates out by watching the image and the centre)
static void radioMixer(std::vector<cf32>& x, double eps, double psiDeg, cf32 leak) {
    const double ps = psiDeg * M_PI / 180;
    for (auto& v : x) {
        const double I = v.real(), Q = v.imag();
        v = cf32((float)((1 + eps) * (I * std::cos(ps / 2) - Q * std::sin(ps / 2))), (float)((1 - eps) * (Q * std::cos(ps / 2) - I * std::sin(ps / 2)))) + leak;
    }
}
static double bin(const std::vector<cf32>& x, double f, double fs) {
    std::complex<double> s = 0;
    for (size_t i = 0; i < x.size(); i++) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * f * (double)i / fs);
    return std::abs(s) / (double)x.size();
}
static void iqChecks() {
    std::vector<cf32> x(4096), y;
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 0.3f);
    for (auto& v : x) v = cf32(nd(rng), nd(rng));
    y = x;
    txIqCorrect(y.data(), y.size(), 0, 0, 0, 0);
    bool same = true;
    for (size_t i = 0; i < x.size(); i++) same &= y[i] == x[i];
    CHECK(same, "no correction: bit-identical");
    TxRequest r; r.opt["out.rampMs"] = 0; r.opt["out.iqGainDb"] = 0.5; r.opt["out.iqPhaseDeg"] = 2; r.opt["out.dcI"] = 0.01; r.opt["out.dcQ"] = -0.02;
    y = x; txOutputStage(r, y.data(), y.size(), 0, 1e6);
    std::vector<cf32> z = x; txIqCorrect(z.data(), z.size(), 0.5, 2, 0.01, -0.02);
    same = true;
    for (size_t i = 0; i < x.size(); i++) same &= y[i] == z[i];
    CHECK(same, "the output stage applies the IQ correction keys");
    // a tone through imbalanced mixers: uncorrected image and leak, then the best correction the user can find on a 0.05 dB / 0.1 degree / 0.001 grid
    const double fs = 1e6, ft = 125e3;
    const size_t n = 8000;   // a whole number of tone periods
    std::vector<cf32> tone(n);
    for (size_t i = 0; i < n; i++) tone[i] = 0.5f * cf32((float)std::cos(2 * M_PI * ft * i / fs), (float)std::sin(2 * M_PI * ft * i / fs));
    struct Case { double eps, psi; cf32 leak; };
    for (const Case& k : {Case{0.03, 2.0, {0.01f, -0.006f}}, Case{-0.05, -4.0, {-0.02f, 0.0f}}, Case{0.0, 6.0, {0.0f, 0.015f}}}) {
        std::vector<cf32> raw = tone;
        radioMixer(raw, k.eps, k.psi, k.leak);
        const double img0 = 20 * std::log10(bin(raw, -ft, fs) / bin(raw, ft, fs)), dc0 = 20 * std::log10(bin(raw, 0, fs) / bin(raw, ft, fs));
        double best = 1e9, bg = 0, bp = 0;
        for (double g = -1.0; g <= 1.0; g += 0.05) for (double p = -8; p <= 8; p += 0.1) {
            std::vector<cf32> v = tone;
            txIqCorrect(v.data(), v.size(), g, p, 0, 0);
            radioMixer(v, k.eps, k.psi, {0, 0});
            const double im = bin(v, -ft, fs) / bin(v, ft, fs);
            if (im < best) { best = im; bg = g; bp = p; }
        }
        // the leak is cancelled by the DC terms: the radio's mixer scales and rotates them, so the user's search finds the right pair
        double bestDc = 1e9;
        for (double di = -0.04; di <= 0.04; di += 0.001) for (double dq = -0.04; dq <= 0.04; dq += 0.001) {
            cf32 v(0.f, 0.f);
            txIqCorrect(&v, 1, bg, bp, di, dq);
            std::vector<cf32> one = {v};
            radioMixer(one, k.eps, k.psi, k.leak);
            bestDc = std::min(bestDc, (double)std::abs(one[0]));
        }
        const double img1 = 20 * std::log10(best), dc1 = 20 * std::log10(bestDc / bin(raw, ft, fs));
        CHECK(img0 > -40 && img1 < -50, "image %0.1f dBc corrected to %0.1f dBc (gain %.2f dB, phase %.1f deg)", img0, img1, bg, bp);
        CHECK(dc0 > -40 && dc1 < -50, "carrier leak %0.1f dBc corrected to %0.1f dBc", dc0, dc1);
    }
}

// ---- timeout and schedule ------------------------------------------------------------------------------------------------------------------
static void timeoutChecks() {
    for (double tot : {0.02, 0.1, 0.25}) {
        TxRequest q; q.modeId = "fm"; q.tune.centerHz = 100e6; q.tune.sampleRate = 1e6; q.loop = true; q.opt["tx.totSec"] = tot;
        auto sink = std::make_unique<RecordingSink>();
        auto rec = sink->r;
        TxSession s; std::string err;
        CHECK(s.start(std::move(sink), q, {}, err), "start: %s", err.c_str());
        int i = 0;
        for (; i < 1000 && s.running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(!s.running(), "the timeout ends a repeating transmission by itself (tot %.2f)", tot);
        const auto st = s.stats();
        s.stop();
        std::lock_guard<std::mutex> l(rec->mu);
        const double want = tot * 1e6;
        CHECK(rec->n >= (size_t)(want - 1) && rec->n <= (size_t)(want + 1), "samples sent %zu, want %.0f", rec->n, want);
        if (rec->iq.size() > 10) CHECK(std::abs(rec->iq.back()) < 0.01f && std::abs(rec->iq[rec->iq.size() / 2]) > 0.05f, "the key-down fade at the timeout: last %.4f", std::abs(rec->iq.back()));
        bool logged = false;
        for (const auto& line : s.log()) logged |= line.find("timeout") != std::string::npos;
        CHECK(logged, "the log says the timeout ended it");
        (void)st;
    }
    TxRequest q; q.opt["tx.totSec"] = -3;
    CHECK(txTimeoutSec(q) == 0, "a negative timeout is off");

    // the schedule
    const double now = 1.7e9;
    TxSchedule a;
    CHECK(!a.active() && txScheduleNext(a, now, 0, 0) == now && txScheduleNext(a, now, now, 1) == -1, "no schedule: once, now");
    TxSchedule b; b.startUtc = now + 600; b.everySec = 120; b.count = 3;
    CHECK(txScheduleNext(b, now, 0, 0) == now + 600, "first start at the set time");
    CHECK(txScheduleNext(b, now + 650, now + 600, 1) == now + 720, "then every 120 s");
    CHECK(txScheduleNext(b, now + 900, now + 720, 2) == now + 900, "a long transmission: the next starts at once");
    CHECK(txScheduleNext(b, now + 900, now + 900, 3) == -1, "three in all");
    TxSchedule c; c.startUtc = now - 50; c.everySec = 10;
    CHECK(txScheduleNext(c, now, 0, 0) == now && txScheduleNext(c, now + 1, now, 1000) == now + 10, "a past start: now; count 0: until cancelled");
    const double midnight = std::floor(now / 86400) * 86400;
    CHECK(txParseUtcTime("23:59:30", now) == midnight + 86370 || txParseUtcTime("23:59:30", now) == midnight + 86370 - 86400, "HH:MM:SS");
    const double t1 = txParseUtcTime("00:00", now);
    CHECK(t1 >= now && t1 - now <= 86400 && std::fmod(t1, 86400) == 0, "the next midnight: %.0f", t1);
    CHECK(txParseUtcTime("24:00", now) < 0 && txParseUtcTime("12", now) < 0 && txParseUtcTime("1a:00", now) < 0 && txParseUtcTime("12:60", now) < 0, "bad times");
}

// ---- DCS on analog FM ------------------------------------------------------------------------------------------------------------------------
static std::vector<float> fmDemod(const std::vector<cf32>& y) {
    std::vector<float> f(y.size() > 1 ? y.size() - 1 : 0);
    for (size_t i = 1; i < y.size(); i++) f[i - 1] = (float)std::arg(std::conj(y[i - 1]) * y[i]);
    return f;
}
static bool dcsHeard(const std::vector<float>& f, double fs, int code) {
    actl::DcsDetector d;
    d.prepare((float)fs, code);
    bool any = false;
    const size_t blk = 4800;
    for (size_t i = 0; i + blk <= f.size(); i += blk) any |= d.process(f.data() + i, (int)blk) && i > f.size() / 2;
    return any;
}
static void dcsChecks() {
    const double fs = 250e3;   // the general receiver's generator works from 0.25 Msps
    const std::vector<int>& codes = txDcsCodes();
    CHECK(codes.size() == 83 && codes.front() == 23 && codes.back() == 754 && txDcsCode(1) == 23 && txDcsCode(0) == 0 && txDcsCode(84) == 0, "the usual 83 codes (%zu)", codes.size());
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0, 1);
    int heard = 0, tried = 0;
    for (size_t k = 0; k < codes.size(); k++) {
        const int code = codes[k];
        const bool inv = k % 5 == 1;
        const double cfo = (k % 3 == 0) ? 1500.0 : (k % 3 == 1) ? -700.0 : 0.0;   // a radio off frequency: the demodulated audio has a DC offset
        const double noise = (k % 4 == 3) ? 0.25 : 0.02;                           // and a weak signal
        TxRequest c; c.modeId = "general"; c.analogVoice = true; c.audioSrc = 2; c.toneHz = 1000; c.opt["audio.toneLevelPct"] = 60; c.deviationHz = 2500;
        c.opt["fm.dcsIdx"] = (double)(k + 1); c.opt["fm.dcsInv"] = inv ? 1 : 0;
        if (k % 7 == 0) c.opt["fm.ctcssIdx"] = 12;   // a CTCSS tone set as well: DCS replaces it
        std::string err;
        auto g = makeTxGenerator(*txModeById("general"), c, fs, err);
        CHECK(g != nullptr, "general analog voice: %s", err.c_str());
        if (!g) continue;
        std::vector<cf32> y((size_t)(fs * 1.6));
        g->generate(y.data(), y.size());
        for (size_t i = 0; i < y.size(); i++) y[i] = y[i] * std::polar(1.0f, (float)(2 * M_PI * cfo * i / fs)) + cf32(noise * nd(rng), noise * nd(rng)) * 0.3f;
        const std::vector<float> f = fmDemod(y);
        tried++;
        const bool ok = dcsHeard(f, fs, code);
        heard += ok;
        CHECK(ok, "DCS %03d%c heard (offset %.0f Hz, noise %.2f)", code, inv ? 'I' : 'N', cfo, noise);
        if (k % 10 == 0) {
            const int other = codes[(k + 17) % codes.size()];
            CHECK(!dcsHeard(f, fs, other), "DCS %03d is not taken for %03d", code, other);
        }
    }
    printf("DCS: %d of %d codes heard by the receiver's detector\n", heard, tried);
    // no DCS chosen: nothing is heard
    TxRequest c; c.modeId = "general"; c.analogVoice = true; c.audioSrc = 2;
    std::string err;
    auto g = makeTxGenerator(*txModeById("general"), c, fs, err);
    if (g) {
        std::vector<cf32> y((size_t)(fs * 1.6));
        g->generate(y.data(), y.size());
        CHECK(!dcsHeard(fmDemod(y), fs, 23), "no DCS when none is chosen");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    hackrfChecks();
    plutoUsrpSoapyChecks();
    calibrationChecks();
    iqChecks();
    timeoutChecks();
    dcsChecks();
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
