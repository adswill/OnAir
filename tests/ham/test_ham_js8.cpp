// Ham JS8: heartbeat, CQ, a directed SNR report and a multi-frame directed text, built by ham_js8 and played slot by slot (Js8Sender) as 48 kHz
// audio, then through the repo's Js8Receiver as a radio input (250 kHz, dial offset, noise at -10 dB in 2500 Hz, a time offset) in every
// submode; the gate, the duty limit and the frame limit. Nothing is transmitted: the rig is a recording fake.
#include "dect2/ham/ham_js8.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_tel.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
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
static HamTxCheck gate(double dial) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; return g;
}
struct Gauss { uint64_t s; explicit Gauss(uint64_t q) : s(q) {} double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; } double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); } };

// the sender's frames in consecutive slots from slot 1, at snrDb in 2500 Hz, dt late, through the receiver
static Js8Telemetry overAir(const std::vector<js8::TxFrame>& frames, int submode, double hz, double snrDb, double dt) {
    const js8::Spec& sp = js8::spec(submode);
    Js8Sender snd; snd.submode = submode; snd.hz = hz; snd.queue(frames);
    const int slots = (int)frames.size() + 2;
    const double total = slots * sp.period + 2;
    std::vector<float> a((size_t)(total * 48000), 0.f);
    js8::TxFrame f;
    for (int k = 1; snd.frameForSlot(k * sp.period, f); k++) {
        const auto b = js8FrameAudio(f, submode, hz);
        const size_t at = (size_t)((snd.txStartUtc(k * sp.period) + dt) * 48000);
        for (size_t i = 0; i < b.size() && at + i < a.size(); i++) a[at + i] += b[i];
    }
    // radio: 250 kHz, the dial 20 kHz below the centre; a tone of peak 1 has power 1/2; noise for snrDb in 2500 Hz
    const double rate = 250000, off = -20000;
    const double sigma = std::sqrt(0.5 / std::pow(10.0, snrDb / 10) * rate / 2500 / 2);
    Js8Receiver r; r.setStartTime(0); r.configure(rate); r.setSignalOffset(off); r.setSilent(true);
    Gauss g(17);
    std::vector<cf32> x(25000);
    const size_t n = (size_t)(total * rate);
    double ph = 0;
    for (size_t i = 0; i < n; i += x.size()) {
        for (size_t k = 0; k < x.size(); k++) {
            const double u = (double)(i + k) * 48000 / rate; const size_t j = (size_t)u; const double fr = u - (double)j;
            const double v = j + 1 < a.size() ? a[j] * (1 - fr) + a[j + 1] * fr : 0.0;
            // the audio is the upper sideband: shift the real audio up by the dial offset as an analytic signal would be (its image falls far outside)
            ph += 2 * 3.141592653589793 * off / rate;
            if (ph < -6.283185307179586) ph += 6.283185307179586;
            x[k] = cf32((float)(v * std::cos(ph) + sigma * g.g()), (float)(v * std::sin(ph) + sigma * g.g()));
        }
        r.feed(x.data(), x.size());
    }
    r.flush();
    r.waitIdle();
    Js8Telemetry t;
    r.telemetry(t, 0);
    return t;
}
static bool heard(const Js8Telemetry& t, const std::string& text) {
    for (const auto& d : t.decodes) if (d.text == text) return true;
    for (const auto& m : t.messages) if (m.text == text) return true;
    return false;
}

int main() {
    std::string err;
    std::vector<js8::TxFrame> hb, cq, snr, txt;
    CHECK(js8Heartbeat("LY1ABC", "KO24", hb, err) && hb.size() == 1, "heartbeat (%s)", err.c_str());
    CHECK(js8Cq("LY1ABC", "KO24", cq, err) && cq.size() == 1, "CQ (%s)", err.c_str());
    CHECK(js8Directed("LY1ABC", "DL1XYZ", " SNR", -12, "", snr, err) && snr.size() == 1, "SNR report (%s)", err.c_str());
    CHECK(js8Directed("LY1ABC", "DL1XYZ", "", 0, "HELLO FROM VILNIUS 73", txt, err) && txt.size() >= 2, "directed text: %zu frames (%s)", txt.size(), err.c_str());
    CHECK(!js8Directed("LY1ABC", "not a call", "", 0, "HI", txt, err), "bad addressee refused");
    std::vector<js8::TxFrame> grp;
    CHECK(js8Directed("LY1ABC", "@ALLCALL", "", 0, "QRV", grp, err) && !grp.empty(), "to a group (%s)", err.c_str());
    std::vector<js8::TxFrame> big;
    CHECK(!js8Directed("LY1ABC", "DL1XYZ", "", 0, std::string(400, 'A'), big, err) && err.find("too long") != std::string::npos, "frame limit: %s", err.c_str());
    CHECK(!js8Heartbeat("NOCALL", "KO24", hb, err), "heartbeat needs a valid call");
    js8Directed("LY1ABC", "DL1XYZ", "", 0, "HELLO FROM VILNIUS 73", txt, err);
    js8Heartbeat("LY1ABC", "KO24", hb, err);

    // ---- through the receiver, every submode ----
    for (int sm = 0; sm < js8::kSubmodes; sm++) {
        const char* nm = js8::spec(sm).name;
        // Turbo has twice Fast's symbol rate, so half the energy per symbol: 3 dB more signal keeps it as far above its limit
        // (about -11 dB here, Fast about -13 dB) as the others; at -10 dB it lost about one frame in four
        const double up = sm == js8::kTurbo ? 3 : 0;
        Js8Telemetry t = overAir(hb, sm, 1000, -10 + up, 0.3);
        CHECK(heard(t, "LY1ABC: @HB HEARTBEAT KO24"), "%s heartbeat at %.0f dB (%zu decodes%s%s)", nm, -10 + up, t.decodes.size(), t.decodes.empty() ? "" : ": ", t.decodes.empty() ? "" : t.decodes[0].text.c_str());
        t = overAir(snr, sm, 1400, -10 + up, 0);
        bool ok = false;
        for (const auto& d : t.decodes) if (d.text.find("DL1XYZ SNR -12") != std::string::npos) ok = true;
        CHECK(ok, "%s SNR report (%zu decodes%s%s)", nm, t.decodes.size(), t.decodes.empty() ? "" : ": ", t.decodes.empty() ? "" : t.decodes[0].text.c_str());
        t = overAir(txt, sm, 1800, -8 + up, 0.2);
        ok = false;
        for (const auto& m : t.messages) if (m.text.find("HELLO FROM VILNIUS 73") != std::string::npos && m.from == "LY1ABC") ok = true;
        CHECK(ok, "%s directed text over %zu frames (%zu messages%s%s)", nm, txt.size(), t.messages.size(), t.messages.empty() ? "" : ": ", t.messages.empty() ? "" : t.messages[0].text.c_str());
        printf("%-7s heartbeat, SNR report and %zu-frame text decoded at %.0f / %.0f / %.0f dB\n", nm, txt.size(), -10 + up, -10 + up, -8 + up);
    }

    // ---- shape, the gate and the limits ----
    {
        const auto a = js8FrameAudio(hb[0], js8::kNormal, 1000);
        CHECK(std::fabs((double)a.size() / 48000 - 79 / 6.25) < 0.01, "normal frame is 12.64 s (%.2f)", (double)a.size() / 48000);
        CHECK(std::fabs(js8BwHz(js8::kNormal) - 50) < 1e-9 && std::fabs(js8BwHz(js8::kTurbo) - 160) < 1e-9, "bandwidths");
        Rec r; HamSlotTx tx(r); TxDuty duty;
        HamTxCheck g = gate(7.078e6); g.armed = false;
        CHECK(!js8Send(tx, duty, g, hb[0], 0, 1000, 0.4, 0, err) && r.ptt1 == 0, "disarmed: nothing keyed");
        CHECK(js8Send(tx, duty, gate(7.3e6 - 500), hb[0], 0, 1000, 0.4, 0, err) && r.ptt1 == 1, "over the 40 m top edge: a warning, sent");
        CHECK(js8Send(tx, duty, gate(7.078e6), hb[0], 0, 1000, 0.4, 0, err) && r.ptt1 == 2 && r.ptt0 == 2, "40 m JS8 armed: sent (%s)", err.c_str());
        TxDuty full; full.add(0, 300);
        CHECK(js8Send(tx, full, gate(7.078e6), hb[0], 0, 1000, 0.4, 300, err), "a full duty counter does not refuse (3.2)");
        Js8Sender s; s.queue(txt); js8::TxFrame f;
        size_t n = 0; while (s.frameForSlot(0, f)) n++;
        CHECK(n == txt.size() && s.pending() == 0, "sender gives every frame once");
    }
    return testSummary("ham_js8");
}
