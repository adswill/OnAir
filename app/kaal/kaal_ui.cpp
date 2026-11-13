// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL screens: the receiver panel (lock, frames, SNR, CFO, sweep level, a speech level meter and the last beacon) and the transmit options
// (callsign, grid, position, power, bio text, beacon on or off) that the TX panel draws for the KAAL signal. English texts only.
#include "app.h"
#include "../panels/tx_state.h"
#include "dect2/kaal/kaal_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 123; }   // the engine reports its standard code minus one

// the status bar: only what KAAL has (no video, TS or guide lamps): lock, decoded voice, a valid beacon
void status(App& a) {
    const bool run = a.engine.running(), lv = live(a);
    const KaalTelemetry& k = a.rx.kaal;
    lamp("Lock", !run || !lv ? 0 : k.state == 2 ? 1 : k.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Voice", !run || !lv ? 0 : k.dataValid ? (k.blocksOk > 4 * k.blocksBad ? 1 : 2) : 0, (int)Ic::Speaker); flowNext(12 * gUi);
    lamp("Beacon", !run || !lv ? 0 : k.beaconValid ? (k.beaconAgeSec < 25 ? 1 : 2) : 0); flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(lv ? dect2::kaalSummary(k).c_str() : (run ? "starting" : "stopped"));
}

const char* stateName(int s) { return s == 2 ? "locked" : s == 1 ? "searching" : "no samples"; }

std::string position(double lat, double lon) {
    char b[64];
    snprintf(b, sizeof b, "%.4f %c, %.4f %c", std::fabs(lat), lat >= 0 ? 'N' : 'S', std::fabs(lon), lon >= 0 ? 'E' : 'W');
    return b;
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAAL is a digital voice link 10 kHz wide: a 12, 16, 24 or 32 kbit/s voice codec on 16-QAM with LDPC, a sweep signature for synchronisation and station ID, and "
                        "a narrow beacon side channel with the station identity. Tune to the signal; the speech plays through the sound output (the volume and "
                        "the mute of the top bar apply). To transmit, switch to TX and pick KAAL.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }

    const dect2::KaalTelemetry& t = a.rx.kaal;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[160];
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 1 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", stateName(t.state));
    ImGui::Spacing();
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    row("frames", b);
    snprintf(b, sizeof b, "%.1f dB", (double)t.snrDb);
    row("SNR", b);
    snprintf(b, sizeof b, "%d ksym/s (%s)", t.symRate / 1000, t.symRate >= 16000 ? "24 / 32 kbit/s, about 20 kHz wide" : "12 / 16 kbit/s, about 10 kHz wide");
    row("Symbol rate", b);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz);
    row("CFO", b);
    snprintf(b, sizeof b, "%.1f dB", (double)t.sweepDb);
    row("sweep level", b);
    if (t.frameVersion == 4) {
        row("frame", "version 4: four slots, chirp sync every 40 ms");
        for (int i = 0; i < 4; i++) {
            static const char* const kP[7] = {"idle", "voice 8.0k", "voice 6.0k", "voice 4.8k", "voice 3.2k", "data", "data (robust)"};
            const int pr = t.v4Profile[i];
            snprintf(b, sizeof b, "%s%s%s%s", pr >= 0 && pr < 7 ? kP[pr] : "?", t.v4Call[i].empty() ? "" : ", ", t.v4Call[i].c_str(), t.v4PlaySlot == i ? " (playing)" : "");
            char k[16];
            snprintf(k, sizeof k, "slot %d", i + 1);
            row(k, b);
        }
        snprintf(b, sizeof b, "%llu", (unsigned long long)t.v4Texts);
        row("texts", b);
    }
    if (t.bitrateKbps) { snprintf(b, sizeof b, "%d kbit/s (read from the frame header)", t.bitrateKbps); row("voice codec", b); }
    row("sweep ID", t.sweepIdValid ? t.sweepId : std::string("not received yet (a full cycle takes 12 s)"));

    ImGui::TextDisabled("%s", "audio level");
    kvColumn(130 * gUi);
    ImGui::ProgressBar(std::max(0.f, std::min(1.f, t.audioLevel)), ImVec2(180 * gUi, 0), "");

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Last beacon");
    if (!t.beaconValid) {
        ImGui::TextDisabled("%s", "none received yet (the beacon repeats every 2 s, with the bio text every 10 s)");
        return;
    }
    row("callsign", t.callsign);
    row("grid", t.grid);
    row("location", position(t.lat, t.lon));
    snprintf(b, sizeof b, "%.1f dBm", t.powerDbm);
    row("power", b);
    snprintf(b, sizeof b, "%d", t.linkQuality);
    row("quality", b);
    snprintf(b, sizeof b, "%.0f s ago, %llu received", t.beaconAgeSec, (unsigned long long)t.beacons);
    row("heard", b);
    ImGui::TextDisabled("%s", "bio text");
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t.text.empty() ? "(no text in the last frame)" : t.text.c_str());
    ImGui::PopTextWrapPos();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAAL";
    if (live(a)) l2 = dect2::kaalSummary(a.rx.kaal);
}

struct Pushed { bool init = false; float vol = -1; bool mute = false; } P;

void tick(App& a) {
    if (!a.engine.running()) { P.init = false; return; }
    dect2::KaalLiveReceiver& r = a.engine.kaal();
    if (!P.init || a.volume != P.vol || a.muted != P.mute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        P.init = true; P.vol = a.volume; P.mute = a.muted;
    }
}

} // namespace

extern const ModeUi kKaalUi;
const ModeUi kKaalUi = {
    .sideTitle = "BEACONS",
    .tabName = "KAAL",
    .tabIcon = Ic::Speaker,
    .tab = tab,
    .status = status,
    .summary = summary,
    .tick = tick,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
}

// KAAL transmit options in the TX panel. The callsign, grid, position, power, bio text and beacon switch are saved in the settings.
void kaalPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kaalLoaded) {
        s.kaalLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kaal.callsign", s.callsign).c_str());
        snprintf(s.grid, sizeof s.grid, "%s", p.getS("tx.kaal.grid", s.grid).c_str());
        snprintf(s.text, sizeof s.text, "%s", p.getS("tx.kaal.text", s.text).c_str());
        s.lat = p.getD("tx.kaal.lat", s.lat);
        s.lon = p.getD("tx.kaal.lon", s.lon);
        s.kaalPower = p.getD("tx.kaal.power", 10);
        s.kaalBeacon = p.getB("tx.kaal.beacon", true);
        s.kaalBitrate = (int)p.getD("tx.kaal.bitrate", 16);
        if (s.kaalBitrate != 12 && s.kaalBitrate != 16 && s.kaalBitrate != 24 && s.kaalBitrate != 32) s.kaalBitrate = 16;
        s.kaalFrame = (int)p.getD("tx.kaal.frame", 3) == 4 ? 4 : 3;
        s.kaalV4Profile = std::max(1, std::min(4, (int)p.getD("tx.kaal.v4profile", 2)));
        s.kaalV4Voice = std::max(0, std::min(4, (int)p.getD("tx.kaal.v4voice", 1)));
    }
    ImGui::TextWrapped("%s", "KAAL: digital voice, 10 kHz wide (20 kHz at 24 and 32 kbit/s). The speech of the audio input below is coded and sent with the identity beacon. "
                              "Empty callsign and grid send the demo identity ONAIR / AA00aa. OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    {
        int fv = s.kaalFrame == 4 ? 1 : 0;
        static const char* const kFrames[2] = {"v3: one voice stream (10 / 20 kHz)", "v4: four slots, voice + data, 20 kHz (new)"};
        ImGui::SetNextItemWidth(330 * gUi);
        if (ImGui::Combo("Frame", &fv, kFrames, 2)) s.kaalFrame = fv ? 4 : 3;
    }
    if (s.kaalFrame == 4) {
        static const char* const kProf[4] = {"8.0 kbit/s per slot", "6.0 kbit/s per slot (default)", "4.8 kbit/s per slot (weak signals)", "3.2 kbit/s per slot (QPSK, most robust)"};
        int pi = s.kaalV4Profile - 1;
        ImGui::SetNextItemWidth(330 * gUi);
        if (ImGui::Combo("Slot voice", &pi, kProf, 4)) s.kaalV4Profile = pi + 1;
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::SliderInt("Voice slots", &s.kaalV4Voice, 0, 4);
        ImGui::TextDisabled("%s", "Slots without voice carry the text (any length) and data. The beacon goes in the control channel every 400 ms. "
                                  "Voice uses the KAAL voice codec 4; while the speaker is silent the slot carries data.");
    } else {
        static const int kRates[4] = {12, 16, 24, 32};
        static const char* const kNames[4] = {"12 kbit/s (real time, 8 ksym/s, most robust)", "16 kbit/s (real time, 8 ksym/s)", "24 kbit/s (real time, 16 ksym/s, 20 kHz wide)",
                                              "32 kbit/s (real time, 16 ksym/s, 20 kHz wide)"};
        int cur = 1;
        for (int i = 0; i < 4; i++) if (kRates[i] == s.kaalBitrate) cur = i;
        ImGui::SetNextItemWidth(330 * gUi);
        if (ImGui::BeginCombo("Voice codec", kNames[cur])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(kNames[i], i == cur)) s.kaalBitrate = kRates[i];
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s", "The receiver reads the bitrate from the frame header. The callsign (6 characters + /P, /M, ...) is also sent in the sweep.");
    }
    ImGui::Checkbox("Send the beacon (station identity)", &s.kaalBeacon);
    ImGui::BeginDisabled(!s.kaalBeacon);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (9 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 9);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputText("Grid (6 characters, e.g. KO26ab)", s.grid, sizeof s.grid)) clampText(s.grid, 6);
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble("Latitude (degrees, north +)", &s.lat, 0.0, 0.0, "%.5f");
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble("Longitude (degrees, east +)", &s.lon, 0.0, 0.0, "%.5f");
    s.lat = std::max(-90.0, std::min(90.0, s.lat));
    s.lon = std::max(-180.0, std::min(180.0, s.lon));
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble("Power (dBm, -40 to 87.5)", &s.kaalPower, 0.5, 5.0, "%.1f");
    s.kaalPower = std::max(-40.0, std::min(87.5, s.kaalPower));
    ImGui::SetNextItemWidth(std::max(200.f * gUi, ImGui::GetContentRegionAvail().x - 160 * gUi));
    if (ImGui::InputText("Bio text (100 characters at most)", s.text, sizeof s.text)) clampText(s.text, 100);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    audioSourceRow(s, running);

    char sig[600];
    snprintf(sig, sizeof sig, "%s|%s|%s|%.6f|%.6f|%.2f|%d|%d|%d|%d|%d", s.callsign, s.grid, s.text, s.lat, s.lon, s.kaalPower, (int)s.kaalBeacon, s.kaalBitrate,
             s.kaalFrame, s.kaalV4Profile, s.kaalV4Voice);
    if (s.kaalSaved != sig) {   // saved when something changed
        s.kaalSaved = sig;
        p.setS("tx.kaal.callsign", s.callsign); p.setS("tx.kaal.grid", s.grid); p.setS("tx.kaal.text", s.text);
        p.setD("tx.kaal.lat", s.lat); p.setD("tx.kaal.lon", s.lon); p.setD("tx.kaal.power", s.kaalPower); p.setB("tx.kaal.beacon", s.kaalBeacon); p.setD("tx.kaal.bitrate", s.kaalBitrate);
        p.setD("tx.kaal.frame", s.kaalFrame); p.setD("tx.kaal.v4profile", s.kaalV4Profile); p.setD("tx.kaal.v4voice", s.kaalV4Voice);
    }
}

} // namespace txui
