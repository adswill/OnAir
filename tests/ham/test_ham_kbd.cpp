// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham keyboard modes: every preset's text goes to audio (kbdRender) and back through the repo's own decoder (HfdigKbd for Olivia, Contestia, MFSK, THOR,
// DominoEX, Hell; HfdigRtty; the full PskReceiver for BPSK31 .. 250 and QPSK31 .. 125; Mt63Receiver; FsqReceiver / IfkpReceiver; the CW timing decoder), plus the transmit limits, the macros, the callsign pick and the ADIF record.
// Nothing is transmitted: the rig is a recording fake.
#include "dect2/ham/ham_kbd.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/psk/psk_varicode.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/fsq/fsq_rx.h"
#include "dect2/fsq/fsq_tel.h"
#include "dect2/ifkp/ifkp_rx.h"
#include "dect2/ifkp/ifkp_tel.h"
#include <complex>
#include "dect2/cw/cw_morse.h"
#include "../../core/src/hfdig/hfdig_kbd_int.h"
#include "../../core/src/hfdig/hfdig_kbd_tables.h"
#include "../common/test_util.h"
#include <cmath>
#include <cstring>
using namespace dect2;
using namespace dect2::ham;

struct Rec : HamRigIo {
    std::vector<float> a;
    int ptt1 = 0, ptt0 = 0;
    bool ptt(bool on, std::string&) override { (on ? ptt1 : ptt0)++; return true; }
    bool beginAudio(int, std::string&) override { a.clear(); return true; }
    int write(const float* m, int n) override { a.insert(a.end(), m, m + n); return n; }
    int pending() override { return 0; }
    void flushAudio() override {}
    void endAudio() override {}
};
static HamTxCheck gate(double dial = 14.07e6) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; g.audioHz = 1000; return g;
}
struct Gauss { uint64_t s = 12345; double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; } double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); } };

// 48 kHz -> 8 kHz, windowed sinc, with `tail` seconds of silence after and light noise
static std::vector<float> to8k(const std::vector<float>& x, double gain, double sigma, double tail) {
    static std::vector<double> h;
    const int N = 96;
    if (h.empty()) for (int k = -N; k <= N; k++) { const double a = k * 2 * 3500.0 / 48000.0 * 3.141592653589793; h.push_back((k == 0 ? 1.0 : std::sin(a) / a) * (0.5 + 0.5 * std::cos(3.141592653589793 * k / (N + 1))) * 2 * 3500.0 / 48000.0); }
    std::vector<float> o((size_t)(x.size() / 6 + tail * 8000), 0.f);
    Gauss g;
    for (size_t n = 0; n < o.size(); n++) {
        double acc = 0;
        const long c = (long)n * 6;
        for (long k = -N; k <= N; k++) { const long i = c + k; if (i >= 0 && i < (long)x.size()) acc += h[(size_t)(k + N)] * x[(size_t)i]; }
        o[n] = (float)(gain * acc + sigma * g.g());
    }
    return o;
}
// The audio as a radio input at 250 kHz (the rate the receivers want, dial at 0 Hz): linear interpolation, its images far outside their front filters;
// white noise for 30 dB SNR in 3 kHz (a good signal, not a perfect one: the FSQ receiver garbles some symbols above about 55 dB in its band);
// two seconds of silence after it. The last telemetry.
template <class Tel, class Rx> static Tel throughRadio(Rx& r, const std::vector<float>& a) {
    r.configure(250000);
    r.setSignalOffset(0);
    r.setSilent(true);
    Gauss g;
    std::vector<cf32> x(25000);
    Tel t; uint64_t seq = 0;
    const size_t total = (size_t)((double)(a.size() + 96000) * 250000 / 48000);
    for (size_t i = 0; i < total; i += x.size()) {
        for (size_t k = 0; k < x.size(); k++) {
            const double u = (double)(i + k) * 48000 / 250000; const size_t j = (size_t)u; const double f = u - (double)j;
            const double v = j + 1 < a.size() ? a[j] * (1 - f) + a[j + 1] * f : 0.0;
            x[k] = cf32((float)(0.5 * v + 0.1 * g.g()), 0.f);
        }
        r.feed(x.data(), x.size());
        Tel tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; t = std::move(tt); }
    }
    return t;
}
static bool has(const std::string& hay, const std::string& need) { return hay.find(need) != std::string::npos; }

int main() {
    const std::string text = "CQ DE LY1ABC LY1ABC K";
    std::string err;

    // ---- every preset: render, then decode with the matching receiver ----
    for (const KbdPreset& p : kbdPresets()) {
        std::string e;
        const std::vector<float> a = kbdRender(p, text, p.defaultHz, 20, e);
        CHECK(!a.empty() && e.empty(), "%s: render: %s", p.name, e.c_str());
        if (a.empty()) continue;
        CHECK(kbdSeconds(a) > 0.5 && kbdSeconds(a) < kKbdMaxBurstSec, "%s: %.1f s", p.name, kbdSeconds(a));
        float pk = 0; for (float v : a) pk = std::max(pk, std::fabs(v));
        CHECK(pk > 0.9f && pk <= 1.0001f, "%s: peak %.3f", p.name, pk);
        std::string got;
        switch (p.kind) {
        case KbdKind::Hfdig: {
            const std::vector<float> x = to8k(a, 0.5, 0.02, 4);
            auto dec = makeKbdDecoder();
            KbdConfig k; k.family = p.family; k.tones = p.tones ? p.tones : 32; k.bandwidth = p.bw ? p.bw : 1000; k.variant = p.variant; k.centreHz = p.defaultHz; k.autoCentre = false;
            dec->setConfig(k);
            for (size_t i = 0; i < x.size(); i += 800) dec->feedAudio(&x[i], std::min<size_t>(800, x.size() - i));
            HfdigKbdTelemetry t; dec->telemetry(t);
            if (p.family == KbdHell) {   // a picture: the columns of the font, as the hfdig test does it
                std::string tx = "CQ DE LY1ABC LY1ABC K";
                std::vector<std::vector<int>> ideal;
                auto nullCol = [&] { ideal.push_back(std::vector<int>(14, 0)); };
                for (size_t ti = 0; ti < tx.size(); ti++) {
                    nullCol();
                    if (tx[ti] == ' ') { nullCol(); nullCol(); nullCol(); }
                    else {
                        unsigned short g[14]; kbd::hellGlyph((uint8_t)tx[ti], g);
                        int ord = 0; for (int i = 0; i < 14; i++) ord |= g[i];
                        for (int col = 0; col < 16; col++) {
                            std::vector<int> cl(14, 0); int any = 0;
                            for (int r = 0; r < 14; r++) if (g[r] & (1 << (15 - col))) { cl[r] = 1; any = 1; }
                            if (!any && (ord & ((1 << (15 - col)) - 1)) == 0) break;
                            ideal.push_back(cl);
                        }
                    }
                    nullCol();
                }
                double best = 0;
                const int N = 60;
                for (int po = 0; po + N <= t.hellCols; po += 2)
                    for (size_t sh = 0; sh + N <= ideal.size(); sh++) {
                        int ok = 0;
                        for (int ci = 0; ci < N; ci++) for (int r = 0; r < 14; r++) ok += (t.hell[(size_t)(po + ci) * 14 + r] > 127) == ideal[sh + ci][r];
                        best = std::max(best, (double)ok / (N * 14));
                    }
                CHECK(best > 0.9, "%s: the picture matches the font %.3f (%d columns)", p.name, best, t.hellCols);
                continue;
            }
            got = t.text;
            break;
        }
        case KbdKind::Rtty: {
            const std::vector<float> x = to8k(a, 0.5, 0.02, 2);
            auto dec = makeRttyDecoder();
            dec->setBaudIndex(0); dec->setShiftIndex(0);
            for (size_t i = 0; i < x.size(); i += 800) dec->feedAudio(&x[i], std::min<size_t>(800, x.size() - i));
            HfdigRttyTelemetry t; dec->telemetry(t);
            got = t.text;
            CHECK(t.framingErrors <= 2, "RTTY: %llu framing errors", (unsigned long long)t.framingErrors);
            break;
        }
        case KbdKind::Psk: {   // the repo's full PSK receiver (its own signal search, AFC, clock recovery, Viterbi for QPSK)
            PskReceiver r; r.setMode(p.pskMode);
            const PskTelemetry t = throughRadio<PskTelemetry>(r, a);
            for (const auto& s : t.signals) if (std::fabs(s.offsetHz - p.defaultHz) < 30 && s.text.size() > got.size()) got = s.text;
            break;
        }
        case KbdKind::Mt63: {
            Mt63Receiver r;
            got = throughRadio<Mt63Telemetry>(r, a).text;
            break;
        }
        case KbdKind::Fsq: {   // the receivers' automatic speed search
            if (p.variant >= 10) { IfkpReceiver r; got = throughRadio<IfkpTelemetry>(r, a).text; }
            else { FsqReceiver r; got = throughRadio<FsqTelemetry>(r, a).text; }
            break;
        }
        case KbdKind::Cw: {   // the key: the envelope of the tone, then the timing decoder of the CW receiver
            std::vector<float> env(a.size());
            double lp = 0;
            for (size_t i = 0; i < a.size(); i++) { lp += 0.01 * (std::fabs(a[i]) - lp); env[i] = (float)lp; }
            cw::Timing tm;
            bool on = false; size_t last = 0;
            for (size_t i = 0; i < env.size(); i++) {
                const bool k = env[i] > 0.3f;
                if (k != on) { const double ms = (double)(i - last) * 1000.0 / 48000; if (on) tm.mark(ms); else if (last > 0) tm.space(ms); on = k; last = i; }
            }
            tm.idle(2000);
            got = tm.take();
            CHECK(std::fabs(tm.wpm() - 20) < 2, "CW speed read as %.1f WPM, sent 20", tm.wpm());
            break;
        }
        }
        // the whole text is required (MFSK32 used to garble the first characters: the receiver moved to a wrong tone shift during the preamble)
        // THOR 22 used to lose its last character (too few characters after the text to flush the interleaver and the resampler)
        const bool whole = p.kind != KbdKind::Hfdig || p.family == KbdMfsk || p.family == KbdThor || p.family == KbdDominoex;
        CHECK(has(got, whole ? "CQ DE LY1ABC LY1ABC K" : "CQ DE LY1ABC LY1ABC"), "%s: decoded \"%s\"", p.name, got.c_str());
        printf("%-18s %5.1f s  -> \"%s\"\n", p.name, kbdSeconds(a), got.c_str());
    }

    // ---- the limits and the gate on the way out ----
    const KbdPreset& psk = *kbdPresetById("psk31");
    {
        Rec r; HamSlotTx tx(r); TxDuty duty;
        HamTxCheck g = gate(); g.armed = false;
        CHECK(!kbdSend(tx, duty, g, psk, "CQ DE LY1ABC", 1000, 0.4, 20, 0, err) && r.ptt1 == 0 && r.a.empty(), "disarmed: nothing keyed (%s)", err.c_str());
        g = gate(); g.licenceConfirmed = false;
        CHECK(kbdSend(tx, duty, g, psk, "CQ DE LY1ABC", 1000, 0.4, 20, 0, err) && r.ptt1 == 1, "no licence: a warning, sent (%s)", err.c_str());
        g = gate(100e6);
        CHECK(kbdSend(tx, duty, g, psk, "CQ DE LY1ABC", 1000, 0.4, 20, 0, err) && r.ptt1 == 2, "outside the amateur bands: a warning, sent (%s)", err.c_str());
        g = gate(14.3495e6);   // 20 m upper edge: a signal 1000 Hz up is outside, 100 Hz up is inside
        CHECK(kbdSend(tx, duty, g, psk, "CQ DE LY1ABC", 1000, 0.4, 20, 0, err) && r.ptt1 == 3, "over the band edge: a warning, sent (%s)", err.c_str());
        g = gate();
        CHECK(kbdSend(tx, duty, g, psk, "CQ DE LY1ABC", 1000, 0.4, 20, 0, err) && r.ptt1 == 4 && r.ptt0 == 4 && !r.a.empty(), "armed and inside: sent (%s)", err.c_str());
        float pk = 0; for (float v : r.a) pk = std::max(pk, std::fabs(v));
        CHECK(std::fabs(pk - 0.4f) < 0.01f, "level: peak %.3f for 0.4", pk);
        CHECK(!kbdSend(tx, duty, g, psk, std::string(kKbdMaxChars + 1, 'E'), 1000, 0.4, 20, 0, err) && !err.empty(), "too many characters");
        CHECK(kbdSend(tx, duty, g, *kbdPresetById("olivia-8-250"), std::string(500, 'E'), 1500, 0.4, 20, 0, err), "a long transmission has no time limit: %s", err.c_str());
        CHECK(!kbdSend(tx, duty, g, psk, "", 1000, 0.4, 20, 0, err), "empty text");
    }
    {   // the duty cycle: 50 % of 10 minutes, with a recording clock
        TxDuty d;
        CHECK(d.allow(1000, 120), "empty history allows");
        d.add(1000, 120); d.add(1200, 120); d.add(1400, 60);
        CHECK(std::fabs(d.used(1500) - 300) < 1e-6 && !d.allow(1500, 10), "300 s of 600 used: %.0f", d.used(1500));
        CHECK(d.allow(2000, 100), "an old burst has aged out");
        Rec r; HamSlotTx tx(r); TxDuty full; full.add(0, 300);
        CHECK(kbdSend(tx, full, gate(), psk, "CQ DE LY1ABC", 1000, 0.4, 20, 300, err) && r.ptt1 == 1, "a full duty counter does not refuse (3.2): %s", err.c_str());
    }

    // ---- macros, call pick, log ----
    {
        KbdStation s; s.myCall = "ly1abc"; s.myGrid = "ko24"; s.myName = "Kris"; s.myQth = "Vilnius";
        std::string o, miss;
        CHECK(kbdExpand(kbdDefaultMacros()[0].text, s, o), "CQ needs only my call");
        CHECK(o == "CQ CQ CQ DE LY1ABC LY1ABC LY1ABC PSE K\n", "CQ macro \"%s\"", o.c_str());
        CHECK(!kbdExpand(kbdDefaultMacros()[1].text, s, o, &miss) && miss == "CALL", "answer needs the other call (%s)", miss.c_str());
        s.call = "k1abc"; s.name = "Bob";
        CHECK(kbdExpand(kbdDefaultMacros()[1].text, s, o) && o == "K1ABC DE LY1ABC LY1ABC K\n", "answer \"%s\"", o.c_str());
        CHECK(kbdExpand(kbdDefaultMacros()[2].text, s, o) && o == "K1ABC DE LY1ABC UR RST 599 599 K\n", "RST \"%s\"", o.c_str());
        CHECK(kbdExpand(kbdDefaultMacros()[3].text, s, o) && o == "MY QTH IS VILNIUS GRID KO24 K\n", "QTH \"%s\"", o.c_str());
        CHECK(kbdExpand(kbdDefaultMacros()[4].text, s, o) && o == "MY NAME IS KRIS KRIS K\n", "name \"%s\"", o.c_str());
        CHECK(kbdExpand(kbdDefaultMacros()[5].text, s, o) && o == "K1ABC DE LY1ABC TNX FER QSO 73 SK\n", "73 \"%s\"", o.c_str());
        CHECK(kbdDefaultMacros().size() == 6, "six macros");
        const auto calls = kbdCallsInText("CQ CQ DE W9XYZ W9XYZ K  LY1ABC DE DL1ABC UR 599", "LY1ABC");
        CHECK(calls.size() == 2 && calls[0] == "W9XYZ" && calls[1] == "DL1ABC", "calls found: %zu", calls.size());
        const KbdPreset& ol = *kbdPresetById("olivia-16-500");
        const QsoRecord q = kbdLogRecord(ol, s, 14.1065e6, 1767225600, 1767225900, "599", "579");
        const std::string adi = adifRecord(q);
        CHECK(has(adi, "<MODE:6>OLIVIA") && has(adi, "<SUBMODE:13>OLIVIA 16/500") && has(adi, "K1ABC") && has(adi, "<BAND:3>20m") && has(adi, "<RST_RCVD:3>579"), "ADIF: %s", adi.c_str());
        const auto back = adifParse(adifHeader() + adi);
        CHECK(back.size() == 1 && back[0].call == "K1ABC" && back[0].mode == "OLIVIA 16/500", "ADIF reads back");
        const QsoRecord qc = kbdLogRecord(*kbdPresetById("cw"), s, 14.03e6, 0, 0, "599", "599");
        CHECK(has(adifRecord(qc), "<MODE:2>CW") && !has(adifRecord(qc), "SUBMODE"), "CW has no submode");
        CHECK(kbdPresetById("psk63") && kbdPresetById("rtty") && !kbdPresetById("nope"), "lookup");
        const std::string fl = kbdFsqLine("ly1abc", "", "hello"), fd = kbdFsqLine("ly1abc", "dl1xyz", "snr?");
        CHECK(fl.find("LY1ABC:") != std::string::npos && fl.find("hello") != std::string::npos && fd.find("DL1XYZ snr?") != std::string::npos, "FSQ lines \"%s\" \"%s\"", fl.c_str(), fd.c_str());
        for (const auto& p : kbdPresets()) CHECK(kbdBwHz(p) > 10 && kbdBwHz(p) < 3000, "%s bandwidth %.0f", p.name, kbdBwHz(p));
    }
    return testSummary("ham_kbd");
}
