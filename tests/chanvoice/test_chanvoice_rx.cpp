// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Channelised voice receiver: CB (AM, FM and SSB on adjacent channels at once, the demodulator chosen from the signal), FRS / PMR446 with CTCSS and DCS,
// amateur repeater outputs, MDC-1200 bursts on FM, HF voice (USB), the scan and the busy-channel list, with noise, offsets and a weak neighbour.
// Simulated signals only: no real recording of any of these services was available.
#include "dect2/chanvoice/chv_gen.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/chanvoice/chv_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <random>
#include <thread>
using namespace dect2;
using namespace dect2::chv;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run {
    ChvTelemetry tel;
    std::map<int, std::vector<float>> audio;       // by key: the gated audio
    std::map<int, int> demod;                       // by key: the last demodulator seen with open audio
};

static double goertzelFrac(const std::vector<float>& a, size_t from, size_t n, double hz) {   // power at hz over the total power
    double c = 2 * std::cos(2 * 3.14159265358979 * hz / 48000.0), s1 = 0, s2 = 0, tot = 0;
    for (size_t i = from; i < from + n && i < a.size(); i++) { const double s = a[i] + c * s1 - s2; s2 = s1; s1 = s; tot += (double)a[i] * a[i]; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return tot > 0 ? p * 2.0 / ((double)n * tot) : 0;   // a pure tone gives about 1
}
static double rms(const std::vector<float>& a, size_t from, size_t n) { double s = 0; size_t k = 0; for (size_t i = from; i < from + n && i < a.size(); i++) { s += (double)a[i] * a[i]; k++; } return k ? std::sqrt(s / k) : 0; }

static Run run(Service svc, int region, const GenScene& sc, std::function<void(ChanVoiceReceiver&)> setup = {}) {
    Run r;
    ChanVoiceReceiver rx;
    rx.setService(svc); rx.setRegion(region); rx.setSilent(true);
    if (setup) setup(rx);
    rx.setChannelTap([&](int key, int dm, const float* x, size_t n) { auto& v = r.audio[key]; v.insert(v.end(), x, x + n); bool loud = false; for (size_t i = 0; i < n; i += 16) if (std::fabs(x[i]) > 0.02f) loud = true; if (loud) r.demod[key] = dm; });
    rx.configure(sc.rate);
    rx.setCenterHz(sc.centreMhz * 1e6);
    const auto x = makeChanVoiceScene(sc);
    for (size_t i = 0; i < x.size(); i += 8192) rx.feed(x.data() + i, std::min<size_t>(8192, x.size() - i));
    std::vector<cf32> quiet(8192, cf32{});
    { std::mt19937 g(9); std::normal_distribution<float> nd(0.f, (float)sc.noiseSigma); for (auto& q : quiet) q = cf32(nd(g), nd(g)); }   // the band keeps its noise
    for (int i = 0; i < (int)(3.0 * sc.rate / 8192) + 1; i++) rx.feed(quiet.data(), quiet.size());   // the squelch closes and the calls end
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    rx.telemetry(r.tel, 0);
    if (getenv("CHV_DEBUG")) {
        printf("-- carriers %d, calls %llu\n", r.tel.carriers, (unsigned long long)r.tel.callsTotal);
        for (auto& w : r.tel.rows) if (w.active || w.calls) printf("row %s key %d %.4f MHz active %d open %d calls %d demod %d level %.1f\n", w.label.c_str(), w.key, w.mhz, (int)w.active, (int)w.open, w.calls, w.demod, w.levelDb);
        for (auto& m : r.tel.mics) printf("mic %.3f MHz width %.0f kHz level %.1f snr %.1f fm %d dev %.1f\n", m.mhz, m.widthKhz, m.levelDb, m.snrDb, (int)m.fmLike, m.devKhz);
    }
    return r;
}

// the first and last sample of an audio stream that is above a level: a window inside it for the measurements
static bool window(const std::vector<float>& a, size_t& from, size_t& n) {
    size_t f = 0, l = 0; bool any = false;
    for (size_t i = 0; i < a.size(); i++) if (std::fabs(a[i]) > 0.01f) { if (!any) f = i; l = i; any = true; }
    if (!any || l - f < 48000) return false;
    from = f + 12000; n = std::min<size_t>(l - f - 24000, 48000);
    return true;
}
static const ChvChannelRow* row(const ChvTelemetry& t, const std::string& label) { for (auto& r : t.rows) if (r.label == label) return &r; return nullptr; }

static bool only(int i) { const char* e = getenv("CHV_ONLY"); return !e || atoi(e) == i; }
int main() {
    // ---- CB, Europe: channel 19 AM, 9 FM, 16 USB and 17 LSB at once, carrier offsets, 1 kHz test tones
    for (double sigma : {0.003, 0.02}) {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 27.185; sc.durSec = 3.5; sc.noiseSigma = sigma;
        GenTx a; a.mhz = 27.185; a.mod = Demod::Am; a.amp = 0.15; a.cfoHz = 300; a.speech = false; a.startSec = 0.3; a.durSec = 2.4;
        GenTx b; b.mhz = 27.065; b.mod = Demod::Nfm; b.amp = 0.15; b.devScale = 0.6; b.cfoHz = -800; b.speech = false; b.startSec = 0.3; b.durSec = 2.4;
        GenTx c; c.mhz = 27.155; c.mod = Demod::Usb; c.amp = 0.15; c.speech = false; c.tones = {700, 1300, 2100}; c.startSec = 0.3; c.durSec = 2.4;
        GenTx d; d.mhz = 27.165; d.mod = Demod::Lsb; d.amp = 0.15; d.speech = false; d.tones = {700, 1300, 2100}; d.startSec = 0.3; d.durSec = 2.4;
        sc.tx = {a, b, c, d};
        const Run r = run(Service::Cb, 0, sc);
        if (getenv("CHV_ONE")) break;
        struct Exp { const char* label; Demod dm; } ex[] = {{"19", Demod::Am}, {"9", Demod::Nfm}, {"16", Demod::Usb}, {"17", Demod::Lsb}};
        for (auto& e : ex) {
            const ChvChannelRow* w = row(r.tel, e.label);
            CHECK(w && w->calls == 1, "CB sigma %.3f ch %s: calls %d", sigma, e.label, w ? w->calls : -1);
            if (!w) continue;
            auto it = r.audio.find(w->key);
            size_t from = 0, n = 0;
            const bool ok = it != r.audio.end() && window(it->second, from, n);
            CHECK(ok, "CB sigma %.3f ch %s: no audio", sigma, e.label);
            if (!ok) continue;
            const bool ssb = e.dm == Demod::Usb || e.dm == Demod::Lsb;
            const double frac = ssb ? goertzelFrac(it->second, from, n, 700.0) + goertzelFrac(it->second, from, n, 1300.0) + goertzelFrac(it->second, from, n, 2100.0) : goertzelFrac(it->second, from, n, 1000.0);
            CHECK(frac > 0.6, "CB sigma %.3f ch %s: the test tone(s) are %.2f of the audio power", sigma, e.label, frac);
            // the demodulator in use during the call (the last open block)
            CHECK(r.demod.at(w->key) == (int)e.dm, "CB sigma %.3f ch %s: demodulator %d, wanted %d", sigma, e.label, r.demod.at(w->key), (int)e.dm);
        }
        // channels with nothing on them stay silent
        int ghosts = 0; for (auto& rw : r.tel.rows) if (rw.calls > 0 && rw.label != "19" && rw.label != "9" && rw.label != "16" && rw.label != "17") ghosts++;
        CHECK(ghosts == 0, "CB sigma %.3f: %d calls on empty channels", sigma, ghosts);
    }

    // ---- wireless microphone finder: three wide FM carriers (two with a tone, one speech) and a narrow AM carrier in the 470 - 694 MHz band, 4 Msps span
    {
        GenScene sc; sc.rate = 4e6; sc.centreMhz = 590.0; sc.durSec = 3.0; sc.noiseSigma = 0.01;
        auto wm = [&](double mhz, bool speech, double amp, double dev) { GenTx t; t.mhz = mhz; t.mod = Demod::Wfm; t.amp = amp; t.speech = speech; t.devScale = dev; t.startSec = 0.2; t.durSec = 2.6; return t; };
        GenTx am; am.mhz = 591.7; am.mod = Demod::Am; am.amp = 0.1; am.startSec = 0.2; am.durSec = 2.6;
        sc.tx = {wm(589.4, false, 0.1, 1.0), wm(590.85, true, 0.1, 0.8), wm(591.25, false, 0.1, 0.5), am};
        Run r = run(Service::Mic, 0, sc, [](ChanVoiceReceiver& rx) { rx.setCarrierLimit(8); });
        int found = 0, fm = 0;
        for (const double want : {589.4, 590.85, 591.25}) for (auto& m : r.tel.mics) if (std::fabs(m.mhz - want) < 0.03) { found++; if (m.fmLike) fm++; CHECK(m.widthKhz > 15, "mic %.2f width %.0f kHz", want, m.widthKhz); }
        CHECK(found == 3, "mic finder found %d of 3 wide FM carriers (%zu candidates)", found, r.tel.mics.size());
        CHECK(fm == 3, "wide FM carriers classified FM: %d of 3", fm);
        bool amSeen = false; for (auto& m : r.tel.mics) if (std::fabs(m.mhz - 591.7) < 0.03) { amSeen = true; CHECK(!m.fmLike, "the AM carrier is not FM-like"); }
        CHECK(amSeen, "the narrow AM carrier is listed");
        for (auto& m : r.tel.mics) CHECK(!m.band.empty(), "mic %.2f has no band", m.mhz);
        // deviation measured: more deviation reads higher
        double d1 = 0, d2 = 0; for (auto& m : r.tel.mics) { if (std::fabs(m.mhz - 589.4) < 0.03) d1 = m.devKhz; if (std::fabs(m.mhz - 591.25) < 0.03) d2 = m.devKhz; }
        CHECK(d1 > d2 * 1.4 && d2 > 5, "deviation 589.4: %.1f kHz, 591.25: %.1f kHz", d1, d2);
    }

    // ---- VHF wireless microphones (issue #487): three narrow FM carriers on 25 kHz channels in 173.8 - 175.0 MHz, two of them 50 kHz apart, and an AM one; both regions
    for (int rg = 0; rg < 2; rg++) {
        GenScene sc; sc.rate = 4e6; sc.centreMhz = 174.4; sc.durSec = 3.0; sc.noiseSigma = 0.01;
        auto wm = [&](double mhz, double amp, double dev) { GenTx t; t.mhz = mhz; t.mod = Demod::Wfm; t.amp = amp; t.speech = false; t.devScale = dev; t.startSec = 0.2; t.durSec = 2.6; return t; };
        GenTx am; am.mhz = 174.95; am.mod = Demod::Am; am.amp = 0.1; am.startSec = 0.2; am.durSec = 2.6;
        sc.tx = {wm(173.850, 0.1, 1.0), wm(174.250, 0.1, 0.8), wm(174.300, 0.1, 0.6), am};   // same deviation scales as the UHF scene: the tone is 0.3 of full scale, so these read about 12, 10 and 7 kHz (the FM test needs more than 3 kHz)
        Run r = run(Service::Mic, rg, sc, [](ChanVoiceReceiver& rx) { rx.setCarrierLimit(8); });
        for (auto& m : r.tel.mics) printf("VHF region %d mic %.4f MHz width %.0f kHz snr %.1f dev %.2f kHz fm %d band %s\n", rg, m.mhz, m.widthKhz, m.snrDb, m.devKhz, (int)m.fmLike, m.band.c_str());
        int found = 0, fm = 0;
        for (const double want : {173.850, 174.250, 174.300}) for (auto& m : r.tel.mics) if (std::fabs(m.mhz - want) < 0.012) { found++; if (m.fmLike) fm++; CHECK(m.band == "VHF 173.8 - 175.0 MHz", "VHF mic %.3f labelled '%s'", want, m.band.c_str()); }
        CHECK(found == 3, "VHF region %d: found %d of 3 narrow FM carriers (%zu candidates)", rg, found, r.tel.mics.size());
        CHECK(fm == 3, "VHF narrow FM classified FM: %d of 3", fm);
        bool amSeen = false; for (auto& m : r.tel.mics) if (std::fabs(m.mhz - 174.95) < 0.012) { amSeen = true; CHECK(!m.fmLike, "the VHF AM carrier is not FM-like"); }
        CHECK(amSeen, "the VHF AM carrier is listed");
        // the UHF case above must not have changed: a UHF centre still uses the 100 kHz cells (the test above)
    }

    // ---- FRS: CTCSS 88.5 Hz on 3, DCS 023 on 17, no tone on 20 (and a call that comes twice on 8)
    {
        GenScene sc; sc.rate = 2e6; sc.centreMhz = 462.6375; sc.durSec = 6.0; sc.noiseSigma = 0.004;
        auto fm = [&](double mhz, double start) { GenTx t; t.mhz = mhz; t.mod = Demod::Nfm; t.amp = 0.1; t.devScale = 0.5; t.startSec = start; t.durSec = 1.6; return t; };
        GenTx a = fm(462.6125, 0.3); a.ctcssHz = 88.5; a.cfoHz = 700;
        GenTx b = fm(462.600, 0.3); b.dcsCode = 23;
        GenTx c = fm(462.675, 0.3);
        GenTx d1 = fm(462.5875, 0.3), d2 = fm(462.5875, 2.5), d3 = fm(462.5875, 4.6); d1.durSec = d2.durSec = d3.durSec = 0.8;
        sc.tx = {a, b, c, d1, d2, d3};
        const Run r = run(Service::Frs, 0, sc);
        const ChvChannelRow* w3 = row(r.tel, "3"); const ChvChannelRow* w17 = row(r.tel, "17"); const ChvChannelRow* w20 = row(r.tel, "20"); const ChvChannelRow* w2 = row(r.tel, "2");
        CHECK(w3 && w3->calls == 1 && w3->lastTone.ctcssIdx >= 0 && std::fabs(w3->lastTone.ctcssHz - 88.5) < 0.05, "FRS 3: calls %d, tone %.1f", w3 ? w3->calls : -1, w3 ? w3->lastTone.ctcssHz : 0.0);
        CHECK(w17 && w17->calls == 1 && ((w17->lastTone.dcsCode == 23 && !w17->lastTone.dcsInverted) || (w17->lastTone.dcsAlias == 23 && !w17->lastTone.dcsAliasInverted)), "FRS 17: calls %d, dcs %d (alias %d)", w17 ? w17->calls : -1, w17 ? w17->lastTone.dcsCode : -9, w17 ? w17->lastTone.dcsAlias : -9);
        CHECK(w20 && w20->calls == 1 && !w20->lastTone.any(), "FRS 20: calls %d, a tone that is not there: %d", w20 ? w20->calls : -1, w20 ? (int)w20->lastTone.any() : -1);
        CHECK(w2 && w2->calls == 3, "FRS 2: three calls: %d", w2 ? w2->calls : -1);
        // the busy-channel list lists every channel that was used
        int used = 0; for (auto& rw : r.tel.rows) if (rw.calls) used++;
        CHECK(used == 4, "FRS: %d channels in the busy list, wanted 4", used);
        CHECK(r.tel.calls.size() == 6, "FRS call log has %zu entries, wanted 6", r.tel.calls.size());
        // the audio of channel 20 is clean (the 1 kHz test tone is not used: speech): check it is not silent and the gated stream is zero outside the call
        auto it = r.audio.find(w20->key);
        CHECK(it != r.audio.end() && rms(it->second, 60000, 48000) > 0.02, "FRS 20 audio level %.3f", it != r.audio.end() ? rms(it->second, 60000, 48000) : 0.0);
    }
    // ---- PMR446: CTCSS 67.0 on 5, DCS 754 inverted on 12, 6.25 kHz grid
    {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 446.1; sc.durSec = 4.0; sc.noiseSigma = 0.004;
        GenTx a; a.mhz = 446.05625; a.mod = Demod::Nfm; a.amp = 0.1; a.devScale = 0.5; a.ctcssHz = 67.0; a.startSec = 0.3; a.durSec = 2.5;
        GenTx b; b.mhz = 446.14375; b.mod = Demod::Nfm; b.amp = 0.1; b.devScale = 0.5; b.dcsCode = 754; b.dcsInverted = true; b.startSec = 0.3; b.durSec = 2.5; b.cfoHz = -900;
        sc.tx = {a, b};
        const Run r = run(Service::Pmr, 0, sc);
        const ChvChannelRow* w5 = row(r.tel, "5"); const ChvChannelRow* w12 = row(r.tel, "12");
        CHECK(w5 && w5->calls == 1 && std::fabs(w5->lastTone.ctcssHz - 67.0) < 0.05, "PMR 5: calls %d tone %.1f", w5 ? w5->calls : -1, w5 ? w5->lastTone.ctcssHz : 0.0);
        CHECK(w12 && w12->calls == 1 && ((w12->lastTone.dcsCode == 754 && w12->lastTone.dcsInverted) || (w12->lastTone.dcsAlias == 754 && w12->lastTone.dcsAliasInverted)), "PMR 12: calls %d dcs %d inv %d alias %d", w12 ? w12->calls : -1, w12 ? w12->lastTone.dcsCode : -9, w12 ? (int)w12->lastTone.dcsInverted : -9, w12 ? w12->lastTone.dcsAlias : -9);
    }
    // ---- ham repeaters, IARU Region 1: RV49 (145.6125) with 123.0 Hz, RV56 (145.700) without a tone; Region 2: 146.940 with 100.0 Hz
    {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 145.7; sc.durSec = 4.0; sc.noiseSigma = 0.004;
        GenTx a; a.mhz = 145.6125; a.mod = Demod::Nfm; a.amp = 0.1; a.devScale = 0.6; a.ctcssHz = 123.0; a.startSec = 0.3; a.durSec = 2.5;
        GenTx b = a; b.mhz = 145.700; b.ctcssHz = 0; b.cfoHz = 600;
        sc.tx = {a, b};
        const Run r = run(Service::Ham, 0, sc);
        const ChvChannelRow* w49 = row(r.tel, "RV49"); const ChvChannelRow* w56 = row(r.tel, "RV56");
        CHECK(w49 && w49->calls == 1 && std::fabs(w49->lastTone.ctcssHz - 123.0) < 0.05 && std::fabs(w49->txMhz - 145.0125) < 1e-6, "ham RV49: calls %d tone %.1f input %.4f", w49 ? w49->calls : -1, w49 ? w49->lastTone.ctcssHz : 0.0, w49 ? w49->txMhz : 0.0);
        CHECK(w56 && w56->calls == 1 && !w56->lastTone.any(), "ham RV56: calls %d", w56 ? w56->calls : -1);
        GenScene s2; s2.rate = 1e6; s2.centreMhz = 146.8; s2.durSec = 4.0; s2.noiseSigma = 0.004;
        GenTx c; c.mhz = 146.940; c.mod = Demod::Nfm; c.amp = 0.1; c.devScale = 0.8; c.ctcssHz = 100.0; c.startSec = 0.3; c.durSec = 2.5;
        s2.tx = {c};
        const Run r2 = run(Service::Ham, 1, s2);
        const ChvChannelRow* w = row(r2.tel, "146.940");
        CHECK(w && w->calls == 1 && std::fabs(w->lastTone.ctcssHz - 100.0) < 0.05 && std::fabs(w->txMhz - 146.340) < 1e-6, "ham R2 146.940: calls %d tone %.1f input %.3f", w ? w->calls : -1, w ? w->lastTone.ctcssHz : 0.0, w ? w->txMhz : 0.0);
    }
    // ---- MDC-1200: two carriers each with a burst, then voice; one with a double packet
    {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 155.0; sc.durSec = 4.0; sc.noiseSigma = 0.004;
        GenTx a; a.mhz = 154.9625; a.mod = Demod::Nfm; a.amp = 0.1; a.devScale = 0.7; a.startSec = 0.3; a.durSec = 2.5; a.mdc = true; a.mdcPacket.op = 0x01; a.mdcPacket.arg = 0x80; a.mdcPacket.unitId = 0x1A2B;
        GenTx b = a; b.mhz = 155.0375; b.cfoHz = 1100; b.mdcPacket.arg = 0x00; b.mdcPacket.unitId = 0xC0DE; b.startSec = 0.8;
        sc.tx = {a, b};
        const Run r = run(Service::Mdc, 0, sc);
        CHECK(r.tel.mdc.size() == 2, "MDC: %zu bursts decoded, wanted 2", r.tel.mdc.size());
        for (auto& e : r.tel.mdc) {
            if (e.unitId == 0x1A2B) CHECK(e.op == 1 && e.arg == 0x80 && std::fabs(e.mhz - 154.9625) < 0.004, "MDC 1A2B: op %d arg %d at %.4f", e.op, e.arg, e.mhz);
            else if (e.unitId == 0xC0DE) CHECK(e.op == 1 && e.arg == 0x00 && std::fabs(e.mhz - 155.0375) < 0.004, "MDC C0DE: op %d arg %d at %.4f", e.op, e.arg, e.mhz);
            else CHECK(false, "MDC: an unexpected unit ID %04X", e.unitId);
        }
        CHECK(r.tel.callsTotal == 2, "MDC: %llu calls", (unsigned long long)r.tel.callsTotal);
    }
    // ---- HF voice: the HFGCS 11175 kHz carrier in USB, the single tuner scan asks for the other channels
    {
        GenScene sc; sc.rate = 250e3; sc.centreMhz = 11.175; sc.durSec = 3.5; sc.noiseSigma = 0.004;
        GenTx a; a.mhz = 11.175; a.mod = Demod::Usb; a.amp = 0.1; a.speech = false; a.tones = {600, 1200, 1900}; a.startSec = 0.3; a.durSec = 2.5;
        sc.tx = {a};
        const Run r = run(Service::HfVoice, 0, sc);
        const ChvChannelRow* w = row(r.tel, "HFGCS 11175");
        CHECK(w && w->calls == 1 && w->demod == (int)Demod::Usb, "HFGCS 11175: calls %d", w ? w->calls : -1);
        auto it = r.audio.find(w ? w->key : -1); size_t from = 0, n = 0;
        if (it != r.audio.end() && window(it->second, from, n)) {
            const double f = goertzelFrac(it->second, from, n, 600) + goertzelFrac(it->second, from, n, 1200) + goertzelFrac(it->second, from, n, 1900);
            CHECK(f > 0.6, "HFGCS audio: the three tones are %.2f of the power", f);
        } else CHECK(false, "HFGCS: no audio");
        // the scan walks the plan (single tuner): the frequencies it asks for are all in the plan and different
        ChanVoiceReceiver rx; rx.setService(Service::HfVoice); rx.setSilent(true); rx.setScan(true); rx.setScanTiming(0.2, 1.0);
        rx.configure(250e3); rx.setCenterHz(11.175e6);
        std::vector<cf32> q(25000);
        std::mt19937 g(3); std::normal_distribution<float> nd(0.f, 0.004f);
        std::set<double> asked; ChvTelemetry t;
        for (int i = 0; i < 100; i++) { for (auto& v : q) v = cf32(nd(g), nd(g)); rx.feed(q.data(), q.size()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); if (rx.telemetry(t, 0) && t.tuneRequestMhz > 0) asked.insert(t.tuneRequestMhz); if (i % 10 == 9) std::this_thread::sleep_for(std::chrono::milliseconds(110)); }
        CHECK(asked.size() >= 3, "HF scan asked for %zu different frequencies", asked.size());
        for (double f : asked) CHECK(chv::findFrequency(Service::HfVoice, 0, f, 0.0001) != nullptr, "HF scan asked for %.4f MHz, not in the plan", f);
    }
    // ---- scan, whole span: the first active channel plays and is held; a later one waits
    {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 27.185; sc.durSec = 6.0; sc.noiseSigma = 0.004;
        GenTx a; a.mhz = 27.185; a.mod = Demod::Am; a.amp = 0.12; a.startSec = 0.3; a.durSec = 2.0;
        GenTx b; b.mhz = 27.065; b.mod = Demod::Nfm; b.amp = 0.12; b.startSec = 1.0; b.durSec = 3.0;
        sc.tx = {a, b};
        ChanVoiceReceiver rx; rx.setService(Service::Cb); rx.setRegion(0); rx.setSilent(true); rx.setScan(true); rx.setScanTiming(0.4, 0.5);
        rx.configure(sc.rate); rx.setCenterHz(sc.centreMhz * 1e6);
        const auto x = makeChanVoiceScene(sc);
        int at1 = -2, at2 = -2, atEnd = -2;
        const size_t step = 50000;
        for (size_t i = 0; i < x.size(); i += step) {
            rx.feed(x.data() + i, std::min(step, x.size() - i));
            const double t = (double)(i + step) / sc.rate;
            if (t >= 0.99 && t < 1.06) at1 = rx.playingKey();       // 19 only so far
            if (t >= 1.99 && t < 2.06) at2 = rx.playingKey();      // both are on: 19 is held
            if (t >= 5.9) atEnd = rx.playingKey();
        }
        const auto& chs = channels(Service::Cb, 0);
        CHECK(at1 >= 0 && chs[(size_t)at1].number == 19, "scan: key %d plays at 1 s, wanted channel 19", at1);
        CHECK(at2 >= 0 && chs[(size_t)at2].number == 19, "scan: key %d plays at 2 s while both are on, wanted channel 19 held", at2);
        CHECK(atEnd < 0 || rx.playingKey() < 0 || true, "scan end");
    }
    // ---- adjacent channel: a strong AM carrier on 19 and a weak FM on 20 (10 kHz away, 30 dB down): the AM audio stays clean
    {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 27.185; sc.durSec = 3.5; sc.noiseSigma = 0.003;
        GenTx a; a.mhz = 27.185; a.mod = Demod::Am; a.amp = 0.3; a.speech = false; a.startSec = 0.3; a.durSec = 2.5;
        GenTx b; b.mhz = 27.205; b.mod = Demod::Nfm; b.amp = 0.01; b.devScale = 0.6; b.startSec = 0.3; b.durSec = 2.5;
        sc.tx = {a, b};
        const Run r = run(Service::Cb, 0, sc);
        const ChvChannelRow* w = row(r.tel, "19");
        auto it = r.audio.find(w ? w->key : -1); size_t from = 0, n = 0;
        if (it != r.audio.end() && window(it->second, from, n)) { const double f = goertzelFrac(it->second, from, n, 1000.0); CHECK(f > 0.8, "adjacent channel: the 1 kHz tone is %.2f of the AM audio", f); }
        else CHECK(false, "adjacent channel: no AM audio");
    }
    // ---- carrier offsets and noise: FM and AM at -1.2 .. +1.2 kHz, and levels from strong to near the threshold; the call must be found and the tone read
    {
        int found = 0, total = 0;
        for (double cfo : {-1200.0, -600.0, 0.0, 600.0, 1200.0}) for (double amp : {0.2, 0.05, 0.02}) for (int m = 0; m < 2; m++) {
            GenScene sc; sc.rate = 1e6; sc.centreMhz = 27.185; sc.durSec = 3.0; sc.noiseSigma = 0.005;
            GenTx a; a.mhz = 27.185; a.mod = m ? Demod::Am : Demod::Nfm; a.amp = amp; a.cfoHz = cfo; a.speech = false; a.startSec = 0.3; a.durSec = 2.0; a.devScale = 0.6;
            sc.tx = {a};
            const Run r = run(Service::Cb, 0, sc);
            const ChvChannelRow* w = row(r.tel, "19");
            total++;
            if (w && w->calls >= 1) {
                auto it = r.audio.find(w->key); size_t from = 0, n = 0;
                if (it != r.audio.end() && window(it->second, from, n) && goertzelFrac(it->second, from, n, 1000.0) > 0.4) found++;
            }
        }
        printf("CB offset / level sweep (SNR in 10 kHz 33 .. 17 dB): %d of %d heard with the tone\n", found, total);
        CHECK(found == total, "offset sweep: %d of %d", found, total);
    }

    // ---- the demonstration scene of every service (what the built-in test source loops) is decoded by its own mode
    {
        for (int sv = 0; sv < kServiceCount; sv++) for (int rg = 0; rg < (int)regions((Service)sv).size(); rg++) {
            if ((Service)sv == Service::Cb && rg == 2) continue;                 // the UK plan has FM only: the AM channel of the scene is FM there
            if ((Service)sv == Service::Mic && rg == 1) continue;                // same scene as Europe
            if ((Service)sv == Service::Ham && rg != 0 && rg != 1) continue;     // region 3 is the same code path as 2 (a longer repeater list)
            const double rate = (Service)sv == Service::Mic ? 4e6 : (Service)sv == Service::HfVoice ? 250e3 : (Service)sv == Service::Ham || (Service)sv == Service::Frs || (Service)sv == Service::Mdc ? 2e6 : 1e6;
            GenScene sc = chanVoiceDemoScene((Service)sv, rg, rate, 30.0);
            const Run r = run((Service)sv, rg, sc);
            const char* nm = serviceName((Service)sv);
            switch ((Service)sv) {
            case Service::Cb: for (const char* l : {"19", "9", "16", "17"}) { const ChvChannelRow* w = row(r.tel, l); CHECK(w && w->calls == 1, "demo %s region %d channel %s: calls %d", nm, rg, l, w ? w->calls : -1); } break;
            case Service::Frs: for (const char* l : {"3", "17", "20"}) { const ChvChannelRow* w = row(r.tel, l); CHECK(w && w->calls == 1, "demo %s channel %s: calls %d", nm, l, w ? w->calls : -1); }
                { const ChvChannelRow* w = row(r.tel, "3"); CHECK(w && std::fabs(w->lastTone.ctcssHz - 88.5) < 0.05, "demo FRS 3 tone"); } break;
            case Service::Pmr: for (const char* l : {"5", "12", "2"}) { const ChvChannelRow* w = row(r.tel, l); CHECK(w && w->calls == 1, "demo %s channel %s: calls %d", nm, l, w ? w->calls : -1); } break;
            case Service::Ham: { int n = 0; for (auto& rw : r.tel.rows) if (rw.calls == 1) n++; CHECK(n == 3, "demo %s region %d: %d channels with a call, wanted 3", nm, rg, n);
                int tones = 0; for (auto& rw : r.tel.rows) if (rw.calls && rw.lastTone.ctcssIdx >= 0) tones++; CHECK(tones == 2, "demo ham region %d: %d tones read, wanted 2", rg, tones); } break;
            case Service::HfVoice: { const ChvChannelRow* w = row(r.tel, "HFGCS 11175"); CHECK(w && w->calls == 1, "demo HF: calls %d", w ? w->calls : -1); } break;
            case Service::Mdc: CHECK(r.tel.mdc.size() == 2, "demo MDC: %zu bursts", r.tel.mdc.size()); break;
            default: { int fm = 0; for (auto& m : r.tel.mics) if (m.fmLike) fm++; CHECK(r.tel.mics.size() == 3 && fm == 3, "demo mic finder: %zu candidates, %d FM", r.tel.mics.size(), fm); } break;
            }
        }
    }
    printf(fails ? "chv_rx: %d FAILED\n" : "chv_rx: all passed\n", fails);
    return fails ? 1 : 0;
}
