// Shortwave through the real path: the synthetic HF slice (AM + RTTY + CW + BPSK31, shortwave_gen.h) goes into ShortwaveReceiver exactly as the
// engine feeds it, the buttons are pressed, and the real decoders (HF digital RTTY, CW skimmer, PSK) produce the text.
// Checked: each decoder alone at an offset with a carrier offset and noise, three decoders in parallel on one channel, a lower sideband
// decoder (the capture mirrored), following a retune (a small move keeps decoding; a jump away and back resumes), stopping, Auto, the log,
// the listening side (the general receiver still hears the AM station), and that a narrow capture is reported rather than crashing.
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/shortwave/shortwave_tel.h"
#include "dect2/shortwave/shortwave_gen.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::swr;
#include "../common/test_util.h"

namespace {

constexpr double kRate = 480000;
const double kDialIn = -shortwaveTuning().tuneOffsetHz;      // the dial in the samples
constexpr double kMarker = kSwSynthRttyCentreHz;             // the channel marker, from the dial

struct Rig {
    ShortwaveReceiver rx;
    std::unique_ptr<ModeSynth> syn;
    std::vector<cf32> buf = std::vector<cf32>(32768);
    bool conj = false;
    double t = 0;
    Rig(const SynthConfig& sc, double rate = kRate) {
        rx.setSilent(true);
        rx.configure(rate);
        syn = makeShortwaveSynth(sc, rate);
        rx.setSignalOffset(kDialIn);
        rx.setChannelOffsetHz(kMarker);
        rx.setDemod(GenUsb);
    }
    void run(double sec) {
        const size_t blocks = (size_t)(sec * kRate / buf.size());
        for (size_t b = 0; b < blocks; b++) {
            syn->generate(buf.data(), buf.size());
            if (conj) for (auto& s : buf) s = std::conj(s);
            rx.feed(buf.data(), buf.size());
            t += (double)buf.size() / kRate;
        }
    }
    ShortwaveTelemetry tel() {
        ShortwaveTelemetry out;
        rx.telemetry(out, 0);
        return out;
    }
    SwDecoderStatus dec(Dec d) {
        for (const auto& s : tel().decoders) if (s.id == (int)d) return s;
        return SwDecoderStatus();
    }
};

SynthConfig cfg(int which, double snr = 30, double cfo = 0) {
    SynthConfig sc;
    sc.mode = 213; sc.snrDb = snr; sc.cfoHz = cfo; sc.modeOpt[0] = which;
    return sc;
}
bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

int main() {
    // ---- the mode's tuning is registered
    {
        const ModeTuning mt = shortwaveTuning();
        CHECK(mt.stdMode == 213 && std::string(mt.id) == "shortwave", "tuning");
        CHECK(modeTuning(213) && modeTuningById("shortwave") && modeTuningById("shortwave")->stdMode == 213, "registered");
        CHECK(makeModeSynth(213, cfg(0), kRate) != nullptr, "the engine finds the test signal");
    }

    // ---- RTTY alone, with a carrier offset and noise
    {
        Rig r(cfg(2, 22, 140));   // the whole slice 140 Hz off; the marker (set for the nominal place) is 140 Hz away from the signal centre
        CHECK(r.rx.startDecoder(Dec::Rtty), "start RTTY");
        r.run(26);
        const SwDecoderStatus s = r.dec(Dec::Rtty);
        printf("  RTTY: %s | %.60s\n", s.status.c_str(), s.text.c_str());
        CHECK(has(s.text, "CQ CQ DE ONAIR"), "RTTY text with a 140 Hz offset and noise: '%s'", s.text.c_str());
        CHECK(s.valid && s.chars > 30, "RTTY valid %d chars %llu", s.valid, (unsigned long long)s.chars);
        CHECK(r.dec(Dec::Cw).id == 0 && r.tel().decoders.size() == 1, "only the pressed decoder runs");
    }

    // ---- three decoders at once on the same channel (the CW carrier sits on the marker; RTTY and PSK within the passband)
    {
        Rig r(cfg(0, 28));
        r.rx.startDecoder(Dec::Rtty);
        r.rx.startDecoder(Dec::Cw);
        r.rx.startDecoder(Dec::Psk);
        r.run(30);
        const ShortwaveTelemetry t = r.tel();
        CHECK(t.decoders.size() == 3, "three decoders reported: %zu", t.decoders.size());
        const SwDecoderStatus rt = r.dec(Dec::Rtty), cw = r.dec(Dec::Cw), ps = r.dec(Dec::Psk);
        printf("  parallel: RTTY '%.40s' | CW '%.40s' | PSK '%.40s'\n", rt.text.c_str(), cw.text.c_str(), ps.text.c_str());
        CHECK(has(rt.text, "CQ CQ DE ONAIR"), "parallel RTTY: '%s'", rt.text.c_str());
        CHECK(has(cw.text, "DE ONAIR"), "parallel CW: '%s'", cw.text.c_str());
        CHECK(has(ps.text, "LY2ABC"), "parallel PSK: '%s'", ps.text.c_str());
        // the listening side is untouched: the general receiver tracks its channel (the USB channel sits under the decoder's pitch)
        CHECK(t.general.rate > 0 && t.general.demod == GenUsb, "general receiver reports (rate %.0f demod %d)", t.general.rate, t.general.demod);
        CHECK(std::fabs(t.channelOffsetHz - (kDialIn + kMarker)) < 1e-6, "channel offset %.1f", t.channelOffsetHz);
        // stop one: the others keep their text and keep running
        r.rx.stopDecoder(Dec::Cw);
        const std::string psBefore = r.dec(Dec::Psk).text;
        r.run(3);
        CHECK(r.tel().decoders.size() == 2 && r.dec(Dec::Cw).id == 0, "CW gone from the list");
        CHECK(r.dec(Dec::Psk).text.size() >= psBefore.size(), "PSK keeps running");
        r.rx.stopAllDecoders();
        r.run(0.5);
        CHECK(r.tel().decoders.empty(), "None stops all");
    }

    // ---- PSK alone at its own offset (the marker on the RTTY centre, the PSK signal 1.6 kHz above it)
    {
        Rig r(cfg(4, 28));
        r.rx.startDecoder(Dec::Psk);
        r.run(14);
        const SwDecoderStatus ps = r.dec(Dec::Psk);
        CHECK(has(ps.text, "CQ CQ DE LY2ABC"), "PSK alone: '%s'", ps.text.c_str());
    }

    // ---- CW alone: the skimmer decodes the carrier on the marker
    {
        Rig r(cfg(3, 28));
        r.rx.startDecoder(Dec::Cw);
        r.run(12);
        const SwDecoderStatus cw = r.dec(Dec::Cw);
        CHECK(has(cw.text, "DE ONAIR ONAIR"), "CW alone: '%s'", cw.text.c_str());
    }

    // ---- follow a retune: a small move keeps decoding; a jump away and back resumes
    {
        Rig r(cfg(2, 30));
        r.rx.startDecoder(Dec::Rtty);
        r.run(14);
        const std::string before = r.dec(Dec::Rtty).text;
        CHECK(has(before, "CQ CQ"), "decoding before the retune: '%s'", before.c_str());
        r.rx.setChannelOffsetHz(kMarker + 60);    // 60 Hz: the same signal, the decoder follows without being restarted
        r.run(14);
        const std::string small = r.dec(Dec::Rtty).text;
        CHECK(small.size() > before.size() && has(small, before.substr(0, 12)), "kept the text and kept decoding after +60 Hz (%zu -> %zu)", before.size(), small.size());
        r.rx.setChannelOffsetHz(kMarker + 150000);   // a long way off: nothing there
        r.run(4);
        CHECK(r.rx.decoderActive(Dec::Rtty), "the decoder stays on while tuned away");
        const size_t away = r.dec(Dec::Rtty).chars;
        r.run(6);
        CHECK(r.dec(Dec::Rtty).chars <= away + 3, "nothing new decoded on an empty channel (%llu -> %llu)", (unsigned long long)away, (unsigned long long)r.dec(Dec::Rtty).chars);
        r.rx.setChannelOffsetHz(kMarker);
        r.run(16);
        const size_t back = r.dec(Dec::Rtty).chars;
        CHECK(back > away + 20, "decoding again after tuning back (%llu -> %llu)", (unsigned long long)away, (unsigned long long)back);
    }

    // ---- lower sideband: the capture is mirrored for the decoder (a BPSK carrier is symmetric, so this proves the mirror, the offsets and the dial arithmetic; RTTY finds a reversed polarity by itself)
    {
        auto run = [&](bool lsbOption) {
            Rig r(cfg(4, 28));
            r.conj = true;                                  // an LSB transmission: the whole spectrum mirrored, the dial at +20 kHz
            r.rx.setSignalOffset(-kDialIn);
            r.rx.setChannelOffsetHz(-kSwSynthPskHz);
            Opts o;
            o.sideband = lsbOption ? -1 : 0;
            r.rx.setDecoderOpts(Dec::Psk, o);
            r.rx.startDecoder(Dec::Psk);
            r.run(14);
            return r.dec(Dec::Psk).text;
        };
        const std::string with = run(true);
        CHECK(has(with, "CQ CQ DE LY2ABC"), "LSB PSK31 with the lower sideband option: '%s'", with.c_str());
    }

    // ---- fixed RTTY settings (Advanced) decode too
    {
        Rig r(cfg(2, 30));
        Opts o;
        o.rttyBaud = 0; o.rttyShift = 0;   // 45.45 baud, 170 Hz
        r.rx.setDecoderOpts(Dec::Rtty, o);
        r.rx.startDecoder(Dec::Rtty);
        r.run(22);
        CHECK(has(r.dec(Dec::Rtty).text, "CQ CQ DE ONAIR"), "fixed 45.45/170: '%s'", r.dec(Dec::Rtty).text.c_str());
        Opts wrong;
        wrong.rttyBaud = 2; wrong.rttyShift = 4;   // 75 baud, 850 Hz: wrong for this signal
        Rig w(cfg(2, 30));
        w.rx.setDecoderOpts(Dec::Rtty, wrong);
        w.rx.startDecoder(Dec::Rtty);
        w.run(22);
        CHECK(!has(w.dec(Dec::Rtty).text, "CQ CQ DE ONAIR"), "wrong fixed settings do not decode");
    }

    // ---- Auto finds the text decoder that works
    {
        Rig r(cfg(2, 30));
        r.rx.setAuto(true);
        r.run(30);
        const ShortwaveTelemetry t = r.tel();
        printf("  Auto: phase %d pick %d after %.1f s\n", t.autoPhase, t.autoPick, t.autoSec);
        CHECK(t.autoPhase == 2 && t.autoPick == (int)Dec::Rtty, "Auto picked RTTY (phase %d pick %d)", t.autoPhase, t.autoPick);
        CHECK(r.rx.decoderActive(Dec::Rtty), "the pick keeps running");
        CHECK(!r.rx.decoderActive(Dec::Psk) && !r.rx.decoderActive(Dec::Olivia) && !r.rx.decoderActive(Dec::Ale), "the others were stopped");
    }

    // ---- the text log and the saved text
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "onair_sw_log_test").string();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        Rig r(cfg(2, 30));
        r.rx.startDecoder(Dec::Rtty);
        r.rx.setDecoderLog(Dec::Rtty, true, dir);
        r.run(20);
        const std::string path = r.rx.decoderLogPath(Dec::Rtty);
        CHECK(!path.empty() && std::filesystem::exists(path), "log file made: '%s'", path.c_str());
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(has(ss.str(), "CQ CQ DE ONAIR"), "the log holds the text: '%.80s'", ss.str().c_str());
        const std::string out = dir + "/saved.txt";
        std::string err;
        CHECK(r.rx.saveDecoderText(Dec::Rtty, out, &err) && std::filesystem::exists(out), "save text: %s", err.c_str());
        CHECK(!r.rx.saveDecoderText(Dec::Cw, dir + "/none.txt", &err), "nothing to save for a decoder that is not running");
        r.rx.clearDecoder(Dec::Rtty);
        r.run(0.5);
        CHECK(r.dec(Dec::Rtty).text.size() < 40, "clear empties the pane");
        std::filesystem::remove_all(dir);
    }

    // ---- the listening side: the AM station is heard by the general receiver while decoders run
    {
        Rig r(cfg(0, 30));
        r.rx.setDemod(GenAm);
        r.rx.setChannelOffsetHz(kSwSynthAmHz);
        r.rx.startDecoder(Dec::Rtty);   // started on the AM channel: nothing to decode there, listening is not disturbed
        r.run(6);
        const ShortwaveTelemetry t = r.tel();
        CHECK(t.general.demod == GenAm && t.general.squelchOpen && t.general.snrDb > 15, "AM station heard: demod %d open %d snr %.1f", t.general.demod, t.general.squelchOpen, t.general.snrDb);
        CHECK(std::fabs(t.general.chanOffsetHz - kSwSynthAmHz) < 1.0, "AM stays on its channel (no audio shift in AM): %.1f", t.general.chanOffsetHz);
        // switch to USB: the listening channel moves under the decoder's pitch
        r.rx.setDemod(GenUsb);
        r.rx.setChannelOffsetHz(kMarker);
        r.run(1);
        CHECK(std::fabs(r.tel().general.chanOffsetHz - (kMarker - 1500)) < 1.0, "USB listening follows the decoder pitch: %.1f", r.tel().general.chanOffsetHz);
    }

    // ---- a capture that is too narrow for a decoder is reported, not crashed on
    {
        ShortwaveReceiver rx;
        rx.setSilent(true);
        rx.configure(16000);
        rx.setSignalOffset(0);
        rx.startDecoder(Dec::Rtty);
        std::vector<cf32> z(16000, cf32(0.f, 0.f));
        for (int i = 0; i < 4; i++) rx.feed(z.data(), z.size());
        ShortwaveTelemetry t;
        CHECK(rx.telemetry(t, 0), "telemetry at 16 ksps");
        bool noted = false;
        for (const auto& s : t.decoders) if (s.id == (int)Dec::Rtty && !s.note.empty()) noted = true;
        CHECK(noted, "the too-narrow capture is reported in the decoder's note");
    }
    return testSummary("shortwave_path");
}
