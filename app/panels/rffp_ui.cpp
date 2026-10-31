// RF Fingerprint window (View > RF fingerprint): recognises an individual NFM / WFM / AM transmitter by its RF fingerprint (carrier offset, keying
// transient, pilot, spurs ...), also when it sends only a carrier. Watches the tuned channel through Engine::setIqTap(), logs every transmission,
// saves named fingerprints locally and matches new ones against them. The DSP is core/src/fingerprint (docs/RF-FINGERPRINT.md).
#include "../app.h"
#include "dect2/fingerprint/rf_fingerprint.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <mutex>

using namespace dect2;
namespace fp = dect2::fp;

namespace {

struct Row {
    fp::Event ev;
    std::string match;     // best library name, empty when none
    double score = 0;
    bool weak = false;
};

struct State {
    std::mutex mu;
    std::unique_ptr<fp::Watcher> w;
    double rate = 0, tunedHz = 0, centreHz = 0;
    int kind = -1;               // -1 auto, else Kind
    bool watching = false, loaded = false;
    fp::Library lib;
    std::vector<Row> log;
    fp::Features cap;            // the fingerprint of the last capture / selected event
    bool haveCap = false;
    std::vector<std::pair<size_t, fp::Match>> capRank;
    char name[64] = "";
    char note[96] = "";
    std::string msg;
    // advanced
    float cfoTol = 3.f, onDb = 8.f, carrierDev = 0.f, rxPpm = 0.f;
};
State S;

std::string libPath() { return plat::dataDir() + "/rf_fingerprints.txt"; }

fp::Kind kindOf(const App& a) {
    if (S.kind >= 0) return (fp::Kind)S.kind;
    return a.family == 5 ? fp::Kind::WFM : a.family == 26 ? fp::Kind::AM : fp::Kind::NFM;
}

fp::MatchOptions mopts() { fp::MatchOptions m; m.cfoTolHz = S.cfoTol; return m; }

void stopWatch(App& a) {
    if (S.watching) a.engine.setIqTap(nullptr);
    std::lock_guard<std::mutex> l(S.mu);
    S.watching = false;
    S.w.reset();
}

void startWatch(App& a) {
    if (!a.engine.running()) { S.msg = tr("Start the receiver first: the fingerprint is taken from the live signal."); return; }
    S.msg.clear();
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.w.reset();
        S.tunedHz = a.freqMhz * 1e6;
        S.centreHz = radioCenterMhz(a) * 1e6;
        S.rate = 0;
    }
    S.watching = true;
    const fp::Kind k = kindOf(a);
    const double off = S.tunedHz - S.centreHz, tuned = S.tunedHz;
    const float on = S.onDb;
    a.engine.setIqTap([k, off, tuned, on](const std::complex<float>* x, size_t n, double rate) {
        std::lock_guard<std::mutex> l(S.mu);
        if (!S.watching) return;
        if (!S.w || S.rate != rate) {
            S.w = std::make_unique<fp::Watcher>(rate, off, k, tuned);
            S.w->setThresholdDb(on);
            S.rate = rate;
        }
        S.w->feed(x, n, (int64_t)std::time(nullptr));
    });
}

void saveLib() { if (!S.lib.save(libPath())) S.msg = tr("Could not save the fingerprint library."); }

void rematch(const fp::Features& f, Row* r) {
    auto rk = S.lib.rank(f, mopts());
    if (r) {
        r->match.clear(); r->score = 0;
        if (!rk.empty()) { r->match = S.lib.items[rk[0].first].name; r->score = rk[0].second.score; r->weak = rk[0].second.weight < 4.0; }
    }
}

void pollLog() {
    std::vector<fp::Event> ev;
    { std::lock_guard<std::mutex> l(S.mu); if (S.w) ev = S.w->poll(); }
    for (auto& e : ev) {
        Row r; r.ev = e;
        if (e.f.valid) rematch(e.f, &r);
        S.log.push_back(r);
        if (S.log.size() > 500) S.log.erase(S.log.begin());
    }
}

void exportCsv() {
    const std::string p = plat::dataDir() + "/rf_fingerprint_log.csv";
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) { S.msg = tr("Could not write the log."); return; }
    std::fprintf(f, "time,seconds,kind,carrier_only,cfo_hz,cfo_std_hz,snr_db,rise_us,match,score\n");
    for (const Row& r : S.log) {
        char t[32] = "";
        const time_t w = (time_t)r.ev.wallStart;
        if (w > 0) std::strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", std::localtime(&w));
        std::fprintf(f, "%s,%.2f,%s,%d,%.4f,%.4f,%.1f,%.0f,%s,%.0f\n", t, r.ev.durationS, fp::kindName(r.ev.f.kind), r.ev.f.carrierOnly ? 1 : 0,
                     r.ev.f.cfoHz, r.ev.f.cfoStdHz, r.ev.f.snrDb, r.ev.f.riseUs, r.match.c_str(), r.score);
    }
    std::fclose(f);
    S.msg = std::string(tr("Log written to ")) + p;
}

void showFeatures(const fp::Features& f) {
    ImGui::Text("%s: %+.3f Hz (+/- %.3f)   %s %.1f dB", tr("Carrier offset"), f.cfoHz, f.cfoStdHz, tr("SNR"), f.snrDb);
    ImGui::TextDisabled("%s %.0f MHz+  %s", tr("tuned"), f.tunedHz / 1e6, f.carrierOnly ? tr("carrier only, no modulation") : tr("modulated"));
    if (f.carrierOnly) ImGui::Text("%s: %+.2f Hz/s", tr("Warm-up drift"), f.driftHzPerS);
    else ImGui::Text("%s: %.0f Hz rms, %.0f Hz p95%s", f.kind == fp::Kind::AM ? tr("Depth (percent)") : tr("Deviation"), f.devRmsHz, f.devP95Hz, "");
    if (f.tiltValid) ImGui::Text("%s: %+.1f dB/oct", tr("Pre-emphasis tilt"), f.tiltDbOct);
    static const char* hn[4] = {"50", "60", "100", "120"};
    for (int i = 0; i < 4; i++) if (f.hum[i] > 0) ImGui::Text("%s %s Hz: %.1f", tr("Mains hum"), hn[i], f.hum[i]);
    if (f.pilot) ImGui::Text("%s: %.0f Hz dev, %+.4f Hz from 19 kHz, jitter %.1f deg", tr("Stereo pilot"), f.pilotDevHz, f.pilotErrHz, f.pilotJitterDeg);
    if (f.transient) ImGui::Text("%s: rise %.0f us, overshoot %.1f%%, chirp %+.0f Hz, settles in %.0f ms", tr("Key-up"), f.riseUs, f.overshootPct, f.chirpHz, f.settleMs);
    else ImGui::TextDisabled("%s", tr("No key-up transient (the carrier was already on)."));
    if (f.spurValid) {
        for (size_t i = 0; i < f.spurs.size() && i < 4; i++) ImGui::Text("%s %+.1f kHz: %.0f dBc", tr("Spur"), f.spurs[i].offHz / 1e3, f.spurs[i].dbc);
        if (f.imageDbc < 90) ImGui::Text("%s: %.0f dBc", tr("I/Q image"), f.imageDbc);
    }
}

}  // namespace

void rfFingerprintWindow(App& a) {
    if (!a.rfFpOpen) { if (S.watching) stopWatch(a); return; }
    if (!S.loaded) { S.lib.load(libPath()); S.loaded = true; }
    firstWindowSize(660 * gUi, 560 * gUi);
    if (!ImGui::Begin(trw("RF fingerprint"), &a.rfFpOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Recognise a transmitter by its RF fingerprint, even when it sends only a carrier (a repeater keying up). Tune to the station, press Watch, then Capture."));
    // retuned while watching: start over on the new channel
    if (S.watching && (std::fabs(S.tunedHz - a.freqMhz * 1e6) > 1.0 || std::fabs(S.centreHz - radioCenterMhz(a) * 1e6) > 1.0 || !a.engine.running())) { stopWatch(a); if (a.engine.running()) startWatch(a); }
    const char* kn[] = {"Auto", "NFM", "WFM", "AM"};
    int ki = S.kind + 1;
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::Combo(tr("Type"), &ki, kn, 4)) { S.kind = ki - 1; if (S.watching) { stopWatch(a); startWatch(a); } }
    ImGui::SameLine();
    if (!S.watching) { if (ImGui::Button(tr("Watch this channel"))) startWatch(a); }
    else if (ImGui::Button(tr("Stop"))) stopWatch(a);
    pollLog();
    fp::Live lv;
    { std::lock_guard<std::mutex> l(S.mu); if (S.w) lv = S.w->live(); }
    if (S.watching) {
        ImGui::SameLine();
        ImGui::TextColored(lv.carrier ? ImVec4(.4f, 1, .4f, 1) : ImVec4(.6f, .6f, .6f, 1), "%s  %.0f dB  %s %.0f dB  %+.1f Hz",
                           lv.carrier ? tr("CARRIER") : tr("idle"), lv.levelDb, tr("SNR"), lv.snrDb, lv.cfoHz);
        ImGui::SameLine();
        if (ImGui::Button(tr("Capture now"))) {
            std::lock_guard<std::mutex> l(S.mu);
            if (S.w) {
                S.cap = S.w->captureNow(1.0);
                S.haveCap = S.cap.valid;
                S.cap.rxPpm = S.rxPpm;
                if (!S.haveCap) S.msg = tr("No usable signal in the last second: is the carrier on and strong enough?");
                else { S.msg.clear(); S.capRank = S.lib.rank(S.cap, mopts()); }
            }
        }
    }
    if (!S.msg.empty()) ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s", S.msg.c_str());

    if (S.haveCap) {
        ImGui::SeparatorText(tr("Last capture"));
        showFeatures(S.cap);
        if (!S.capRank.empty()) {
            ImGui::TextUnformatted(tr("Matches in your library:"));
            for (size_t i = 0; i < S.capRank.size() && i < 3; i++) {
                const auto& m = S.capRank[i];
                ImGui::ProgressBar((float)m.second.score / 100.f, ImVec2(120 * gUi, 0), "");
                ImGui::SameLine();
                ImGui::Text("%s  %.0f%%%s", S.lib.items[m.first].name.c_str(), m.second.score, m.second.weight < 4.0 ? tr("  (few features)") : "");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    for (const auto& p : m.second.parts) ImGui::Text("%s: %.1f sigma", p.name.c_str(), p.d);
                    ImGui::EndTooltip();
                }
            }
        } else ImGui::TextDisabled("%s", tr("The library has no fingerprint of this type yet."));
        ImGui::SetNextItemWidth(180 * gUi);
        ImGui::InputTextWithHint("##fpname", tr("Name, e.g. Repeater Hill A"), S.name, sizeof S.name);
        ImGui::SameLine();
        if (ImGui::Button(tr("Save fingerprint")) && S.name[0]) {
            S.cap.tunedHz = S.cap.tunedHz > 0 ? S.cap.tunedHz : a.freqMhz * 1e6;
            S.lib.add(S.name, S.cap, S.note);
            saveLib();
            S.capRank = S.lib.rank(S.cap, mopts());
        }
    }

    if (ImGui::CollapsingHeader(tr("Library"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (S.lib.items.empty()) ImGui::TextDisabled("%s", tr("Nothing saved yet."));
        int del = -1;
        for (size_t i = 0; i < S.lib.items.size(); i++) {
            const auto& e = S.lib.items[i];
            ImGui::PushID((int)i);
            ImGui::Text("%s  [%s %.4f MHz, %+.3f Hz%s]", e.name.c_str(), fp::kindName(e.f.kind), e.f.tunedHz / 1e6, e.f.cfoHz, e.f.carrierOnly ? ", carrier" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Delete"))) del = (int)i;
            ImGui::PopID();
        }
        if (del >= 0) { S.lib.remove((size_t)del); saveLib(); }
    }

    if (ImGui::CollapsingHeader(tr("Transmission log"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton(tr("Export CSV"))) exportCsv();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear"))) S.log.clear();
        if (ImGui::BeginTable("##fplog", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 180 * gUi))) {
            ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("What"));
            ImGui::TableSetupColumn(tr("Offset Hz")); ImGui::TableSetupColumn(tr("Match")); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (size_t i = S.log.size(); i-- > 0;) {
                const Row& r = S.log[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                char t[16] = "";
                const time_t w = (time_t)r.ev.wallStart;
                if (w > 0) std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&w));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t);
                ImGui::TableNextColumn(); ImGui::Text("%.2f s", r.ev.durationS);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(!r.ev.f.valid ? tr("too weak") : r.ev.f.carrierOnly ? tr("carrier only") : tr("modulated"));
                ImGui::TableNextColumn(); if (r.ev.f.valid) ImGui::Text("%+.3f", r.ev.f.cfoHz);
                ImGui::TableNextColumn(); if (!r.match.empty()) ImGui::Text("%s %.0f%%", r.match.c_str(), r.score);
                ImGui::TableNextColumn();
                if (r.ev.f.valid && ImGui::SmallButton(tr("Use"))) { S.cap = r.ev.f; S.haveCap = true; S.capRank = S.lib.rank(S.cap, mopts()); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(tr("Advanced"))) {
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::DragFloat(tr("Offset allowance (Hz)"), &S.cfoTol, 0.1f, 0.05f, 100.f, "%.2f");
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::DragFloat(tr("Carrier detect (dB over noise)"), &S.onDb, 0.2f, 3.f, 30.f, "%.1f");
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::DragFloat(tr("Carrier-only below (Hz deviation, 0 = automatic)"), &S.carrierDev, 10.f, 0.f, 5000.f, "%.0f");
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::DragFloat(tr("Receiver clock error (ppm, 0 = unknown)"), &S.rxPpm, 0.01f, -100.f, 100.f, "%.2f");
        ImGui::TextWrapped("%s", tr("The offset includes your receiver's own clock error. A GPS-locked or calibrated radio gives the tightest match; with an uncalibrated radio keep the allowance wide or rely on the key-up transient, pilot and spurs."));
    }
    ImGui::End();
}
