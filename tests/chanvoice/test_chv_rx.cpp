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

int main() {
    // ---- CB, Europe: channel 19 AM, 9 FM, 16 USB and 17 LSB at once, carrier offsets, 1 kHz test tones
    for (double sigma : {0.003, 0.02}) {
        GenScene sc; sc.rate = 1e6; sc.centreMhz = 27.185; sc.durSec = 3.5; sc.noiseSigma = sigma;
        GenTx a; a.mhz = 27.185; a.mod = Demod::Am; a.amp = 0.15; a.cfoHz = 300; a.speech = false; a.startSec = 0.3; a.durSec = 2.4;
        GenTx b; b.mhz = 27.065; b.mod = Demod::Nfm; b.amp = 0.15; b.devScale = 0.6; b.cfoHz = -800; b.speech = false; b.startSec = 0.3; b.durSec = 2.4;
        GenTx c; c.mhz = 27.155; c.mod = Demod::Usb; c.amp = 0.15; c.speech = false; c.startSec = 0.3; c.durSec = 2.4;
        GenTx d; d.mhz = 27.165; d.mod = Demod::Lsb; d.amp = 0.15; d.speech = false; d.startSec = 0.3; d.durSec = 2.4;
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
            const double frac = goertzelFrac(it->second, from, n, 1000.0);
            CHECK(frac > 0.6, "CB sigma %.3f ch %s: 1 kHz tone is %.2f of the audio power", sigma, e.label, frac);
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
    printf(fails ? "chv_rx: %d FAILED\n" : "chv_rx: all passed\n", fails);
    return fails ? 1 : 0;
}
