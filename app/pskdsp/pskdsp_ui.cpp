// PSK demodulator screens (family 202, experimental): the analyser (what the blind analysis found: symbol rate and its candidates, order
// with the EVM of each trial, pulse shape, carrier, Doppler, Eb/N0, and the two line spectra the rate and the carrier come from), the bits
// (hex and text, lines starting at each sync word) and the log; the constellation and the eye diagram in the analysis row. Everything is
// automatic; the order, rate, roll-off, span, differential coding, scrambler and sync word can be set by hand under Decoder options.
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false, wasRunning = false, dirty = true;
    dect2::PskdSettings cfg;
    char sync[80] = {};
    int view = 0;                 // 0 analyser, 1 bits, 2 log
    int bitView = 0;              // 0 hex + text, 1 bits
};
State S;

bool live(const App& a) { return a.rx.standard == 203; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.cfg.order = (int)p.getI("pskdOrder", 0);
    S.cfg.baud = p.getD("pskdBaud", 0);
    S.cfg.alpha = p.getD("pskdAlpha", -2);
    S.cfg.spanHz = p.getD("pskdSpan", 0);
    S.cfg.snapStandard = p.getB("pskdSnap", true);
    S.cfg.followDoppler = p.getB("pskdFollow", true);
    S.cfg.diff = (int)p.getI("pskdDiff", 0);
    S.cfg.scrambler = (int)p.getI("pskdScr", 0);
    S.cfg.sync = p.getS("pskdSync", "");
    S.cfg.invert = p.getB("pskdInvert", false);
    snprintf(S.sync, sizeof S.sync, "%s", S.cfg.sync.c_str());
}
void store() {
    auto& p = plat::prefs();
    p.setI("pskdOrder", S.cfg.order); p.setD("pskdBaud", S.cfg.baud); p.setD("pskdAlpha", S.cfg.alpha); p.setD("pskdSpan", S.cfg.spanHz);
    p.setB("pskdSnap", S.cfg.snapStandard); p.setB("pskdFollow", S.cfg.followDoppler); p.setI("pskdDiff", S.cfg.diff); p.setI("pskdScr", S.cfg.scrambler);
    p.setS("pskdSync", S.cfg.sync); p.setB("pskdInvert", S.cfg.invert);
    S.dirty = true;
}

std::string newSavePath() {
    const std::string dir = plat::dataDir() + "/psk-bits";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char b[64];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(b, sizeof b, "psk_%Y%m%d_%H%M%S.bin", &tm);
    return dir + "/" + b;
}

const ImU32 kBg = IM_COL32(14, 16, 18, 255), kGrid = IM_COL32(50, 54, 58, 255);

void constel(const dect2::PskdTelemetry& t, ImVec2 size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, kBg, 3.f);
    const ImVec2 m((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    const float r = std::min(size.x, size.y) * 0.32f;
    dl->AddLine(ImVec2(p0.x, m.y), ImVec2(p1.x, m.y), kGrid);
    dl->AddLine(ImVec2(m.x, p0.y), ImVec2(m.x, p1.y), kGrid);
    dl->AddCircle(m, r, kGrid, 48);
    const ImU32 col = t.state == 2 ? IM_COL32(110, 220, 160, 150) : IM_COL32(200, 200, 120, 110);
    for (const auto& z : t.constellation) {
        const float x = m.x + std::clamp(z.real(), -1.5f, 1.5f) * r, y = m.y - std::clamp(z.imag(), -1.5f, 1.5f) * r;
        dl->AddRectFilled(ImVec2(x - 1, y - 1), ImVec2(x + 1, y + 1), col);
    }
    ImGui::Dummy(size);
}

void eye(const dect2::PskdTelemetry& t, ImVec2 size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, kBg, 3.f);
    auto y = [&](float v) { return p0.y + size.y * 0.5f - std::clamp(v, -2.f, 2.f) * size.y * 0.22f; };
    for (float v : {-1.f, 0.f, 1.f}) dl->AddLine(ImVec2(p0.x, y(v)), ImVec2(p1.x, y(v)), kGrid);
    const int L = 16;
    for (size_t s = 0; s + L <= t.eye.size(); s += L)
        for (int i = 1; i < L; i++)
            dl->AddLine(ImVec2(p0.x + (i - 1) * size.x / (L - 1), y(t.eye[s + i - 1])), ImVec2(p0.x + i * size.x / (L - 1), y(t.eye[s + i])), IM_COL32(240, 190, 90, 80));
    ImGui::Dummy(size);
}

// a line spectrum (dB) with one marker; x from x0 to x0 + n * hzPerBin
void lineSpec(const std::vector<float>& db, double x0, double hzPerBin, double markHz, ImVec2 size, const char* id) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, kBg, 3.f);
    if (db.size() < 2) return;
    float lo = 1e9f, hi = -1e9f;
    for (float v : db) { lo = std::min(lo, v); hi = std::max(hi, v); }
    lo = std::max(lo, hi - 50);
    const float span = std::max(1.f, hi - lo);
    const size_t n = db.size();
    for (size_t i = 1; i < n; i++) {
        const float xa = p0.x + (float)(i - 1) * size.x / (float)(n - 1), xb = p0.x + (float)i * size.x / (float)(n - 1);
        const float ya = p1.y - (std::max(db[i - 1], lo) - lo) / span * (size.y - 4), yb = p1.y - (std::max(db[i], lo) - lo) / span * (size.y - 4);
        dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), IM_COL32(130, 180, 230, 220));
    }
    if (hzPerBin > 0 && markHz != 0) {
        const float x = p0.x + (float)((markHz - x0) / (hzPerBin * (double)n)) * size.x;
        if (x >= p0.x && x <= p1.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(240, 160, 80, 160));
    }
    if (ImGui::IsItemHovered()) {
        const float fx = (ImGui::GetIO().MousePos.x - p0.x) / size.x;
        ImGui::SetTooltip("%.1f Hz", x0 + fx * hzPerBin * (double)n);
    }
}

void topRow(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Experimental"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Checked against generated signals only; no real satellite recording was available. Reports of what it does with real signals help."));
    flowNext(8 * gUi);
    char b[60];
    snprintf(b, sizeof b, "%s##pskre", tr("Analyse again"));
    if (ImGui::Button(b)) a.engine.pskd().reanalyse();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Drop the lock and look at the signal nearest the dial again (after tuning to another signal, for example)"));
    flowNext(6 * gUi);
    snprintf(b, sizeof b, "%s##pskclr", tr("Clear"));
    if (ImGui::Button(b)) a.engine.pskd().clearResults();
    flowNext(6 * gUi);
    bool save = S.cfg.saveBits;
    snprintf(b, sizeof b, "%s##psksave", tr("Save bits"));
    if (ImGui::Checkbox(b, &save)) {
        S.cfg.saveBits = save;
        if (save) S.cfg.savePath = newSavePath();
        S.dirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Write the decoded bits, packed into bytes (first bit in the top bit), to a new file in the psk-bits folder of the OnAir data folder"));
    if (S.cfg.saveBits && live(a) && !a.rx.pskd.savingTo.empty()) {
        flowNext(6 * gUi);
        ImGui::TextDisabled("%s (%llu B)", a.rx.pskd.savingTo.c_str(), (unsigned long long)a.rx.pskd.bytesSaved);
    }
    flowEnd();
}

void analyserView(App& a) {
    const dect2::PskdTelemetry& t = a.rx.pskd;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.92f, 0.6f, 1) : ImVec4(0.9f, 0.85f, 0.55f, 1), "%s", t.status.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    if (t.signal || t.state == 2) {
        kv(tr("signal"), "%s %.1f Hz, %s %.1f Hz, SNR %.1f dB", tr("-3 dB width"), t.bw3Hz, tr("99 % width"), t.occHz, t.snrDb);
        kv(tr("carrier"), "%+.1f Hz %s", t.cfoHz, tr("from the dial"));
    }
    if (t.rateFound || t.state == 2) {
        kv(tr("symbol rate"), "%.2f Bd%s (%s %.2f Bd)", t.baud, t.baudSnapped ? tr(", a standard rate") : "", tr("measured"), t.baudRaw);
        kv(tr("modulation"), "%s, %s %.0f %%", dect2::pskdOrderName(t.order), tr("confidence"), 100 * t.orderConf);
        std::string trials;
        for (const auto& e : t.orderEvm) { char b[40]; snprintf(b, sizeof b, "%s%s %.0f %%", trials.empty() ? "" : ", ", dect2::pskdOrderName(e.order), 100 * e.evm); trials += b; }
        if (!trials.empty()) kv(tr("EVM per order tried"), "%s", trials.c_str());
        kv(tr("pulse shape"), "%s", t.shape.c_str());
        kv(tr("bit rate"), "%.0f bit/s", t.bitRate > 0 ? t.bitRate : t.baud * (t.order == 4 ? 3 : t.order == 1 ? 1 : 2));
        kv(tr("quality"), "EVM %.0f %%, MER %.1f dB, Eb/N0 %.1f dB", 100 * t.evm, t.merDb, t.ebn0Db);
        if (std::fabs(t.dopplerHzPerSec) > 0.05) kv(tr("Doppler rate"), "%+.1f Hz/s", t.dopplerHzPerSec);
    }
    if (t.state == 2) {
        kv(tr("lock"), "%.0f %%, %llu %s", 100 * t.lockMetric, (unsigned long long)t.symbols, tr("symbols"));
        if (!S.cfg.sync.empty())
            kv(tr("sync word"), "%llu %s%s", (unsigned long long)t.syncs, tr("found"), t.ambiguityResolved ? tr(", phase ambiguity resolved") : "");
    }
    if (!t.rates.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("symbol-rate candidates"));
        if (ImGui::BeginTable("##pskrates", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(tr("rate")); ImGui::TableSetupColumn(tr("strength")); ImGui::TableSetupColumn(tr("from"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& r : t.rates) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%.2f Bd", r.baud);
                ImGui::TableNextColumn(); ImGui::Text("%.0f %%", 100 * r.conf);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.how.c_str());
            }
            ImGui::EndTable();
        }
    }
    const float W = ImGui::GetContentRegionAvail().x;
    if (!t.envSpecDb.empty()) {
        ImGui::Spacing();
        captionFit(W, "%s", tr("spectrum of |x|^2: the symbol-rate line (marked)"));
        lineSpec(t.envSpecDb, 0, t.envHzPerBin, t.envLineHz, ImVec2(W, 70 * gUi), "##pskenv");
    }
    if (!t.powSpecDb.empty()) {
        captionFit(W, tr("spectrum of x^%d: the carrier line at %d x the offset (marked)"), t.powM, t.powM);
        lineSpec(t.powSpecDb, -t.powHzPerBin * (double)t.powSpecDb.size() / 2, t.powHzPerBin, t.powLineHz, ImVec2(W, 70 * gUi), "##pskpow");
    }
}

void bitsView(App& a) {
    const dect2::PskdTelemetry& t = a.rx.pskd;
    subNav("pskbv", S.bitView, {"Hex and text", "Bits"});
    ImGui::TextDisabled(tr("%llu bits, %llu sync words"), (unsigned long long)t.bitsTotal, (unsigned long long)t.syncs);
    if (t.bits.empty()) { ImGui::TextDisabled("%s", tr("no bits yet")); return; }
    ImGui::BeginChild("##pskbits", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    ImGui::PushFont(a.mono, 0);
    // line starts: every sync word in the window, else every 256 bits
    std::vector<size_t> starts;
    for (uint64_t s : t.syncAt) if (s >= t.bitsBase && s < t.bitsBase + t.bits.size()) starts.push_back((size_t)(s - t.bitsBase));
    const size_t per = S.bitView == 0 ? 256 : 128;
    std::vector<std::pair<size_t, size_t>> lines;
    size_t pos = 0;
    for (size_t st : starts) {
        while (pos + per <= st) { lines.push_back({pos, per}); pos += per; }
        if (st > pos) lines.push_back({pos, st - pos});
        pos = st;
    }
    while (pos < t.bits.size()) { const size_t n = std::min(per, t.bits.size() - pos); lines.push_back({pos, n}); pos += n; }
    ImGuiListClipper clip;
    clip.Begin((int)lines.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& L = lines[(size_t)i];
            const bool atSync = std::find(starts.begin(), starts.end(), L.first) != starts.end();
            ImGui::TextDisabled("%9llu", (unsigned long long)(t.bitsBase + L.first));
            ImGui::SameLine();
            if (atSync) { ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1), "%s", ">"); ImGui::SameLine(); }
            std::string s;
            if (S.bitView == 0) {
                char h[4];
                std::string txt;
                for (size_t k = L.first; k + 8 <= L.first + L.second; k += 8) {
                    int v = 0;
                    for (int j = 0; j < 8; j++) v = (v << 1) | (t.bits[k + (size_t)j] & 1);
                    snprintf(h, sizeof h, "%02X ", v);
                    s += h;
                    txt += v >= 32 && v < 127 ? (char)v : '.';
                }
                s += " " + txt;
            } else {
                for (size_t k = L.first; k < L.first + L.second; k++) { s += t.bits[k] ? '1' : '0'; if ((k - L.first) % 8 == 7) s += ' '; }
            }
            ImGui::TextUnformatted(s.c_str());
        }
    ImGui::PopFont();
    ImGui::EndChild();
}

void logView(App& a) {
    const auto& l = a.rx.pskd.log;
    ImGui::BeginChild("##psklog", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    ImGui::PushFont(a.mono, 0);
    for (auto it = l.rbegin(); it != l.rend(); ++it) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(it->c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PopFont();
    ImGui::EndChild();
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    if (!S.wasRunning || S.dirty) {
        S.cfg.sync = S.sync;
        a.engine.pskd().setSettings(S.cfg);
        S.dirty = false;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    topRow(a);
    ImGui::Spacing();
    subNav("pskv", S.view, {"Analyser", "Bits", "Log"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) analyserView(a); else if (S.view == 1) bitsView(a); else logView(a);
}

void panels(App& a) {
    const dect2::PskdTelemetry& t = a.rx.pskd;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float cw = std::min(plotH * 1.2f, (W - 3 * gap) / 3.f), ew = std::max(110.f, (W - 3 * gap - cw) / 2.f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup(); captionFit(cw, "%s", tr("Constellation")); constel(t, ImVec2(cw, plotH)); ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup(); captionFit(ew, "%s", tr("Eye (I rail)")); eye(t, ImVec2(ew, plotH)); ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup(); captionFit(ew, "%s", tr("|x|^2 line spectrum")); lineSpec(t.envSpecDb, 0, t.envHzPerBin, t.envLineHz, ImVec2(ew, plotH), "##pskpenv"); ImGui::EndGroup();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::PskdTelemetry& t = a.rx.pskd;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::pskdSummary(t).c_str());
    kv(tr("search span"), "%.1f kHz", t.spanHz / 1e3);
    kv(tr("channel rate"), "%.0f samples/s", t.chanRate);
    kv(tr("analyses"), "%llu", (unsigned long long)t.analyses);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
    if (t.state == 2) {
        kv(tr("phase"), "%s %d x %d deg%s%s", tr("rotation"), t.rotation, t.order == 1 ? 180 : t.order == 4 ? 45 : 90, t.conj ? tr(", spectrum inverted") : "",
           t.railShift ? tr(", Q rail paired with the next I") : "");
        kv(tr("bits"), "%llu", (unsigned long long)t.bitsTotal);
    }
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& l = a.rx.pskd.log;
    for (auto it = l.rbegin(); it != l.rend() && it - l.rbegin() < 12; ++it) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", it->c_str()); ImGui::PopTextWrapPos(); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.pskd.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Lock"), !on ? 0 : a.rx.pskd.state == 2 ? 1 : 0); flowNext(10 * gUi);
    if (!S.cfg.sync.empty()) { lamp(tr("Sync"), !on ? 0 : a.rx.pskd.syncs > 0 ? 1 : 0); flowNext(10 * gUi); }
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::pskdSummary(a.rx.pskd).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "PSK";
    if (live(a)) l2 = dect2::pskdSummary(a.rx.pskd);
}

// Decoder options: everything is automatic; these override the analyser
void decoder(App&, bool&) {
    load();
    ImGui::TextDisabled("%s", tr("Automatic unless set here"));
    const float w = 150 * gUi;
    static const char* orders[] = {"auto", "BPSK", "QPSK", "OQPSK", "8PSK"};
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Modulation")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##pskdmodulation", &S.cfg.order, orders, 5)) store();
    float baud = (float)S.cfg.baud;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Symbol rate (0 = auto)")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::InputFloat("##pskdsymbolrate", &baud, 0, 0, "%.2f")) { S.cfg.baud = std::max(0.f, baud); store(); }
    static const char* shapes[] = {"auto", "rectangular", "RRC 0.2", "RRC 0.25", "RRC 0.35", "RRC 0.5", "RRC 0.8", "RRC 1.0"};
    static const double shapeVal[] = {-2, -1, 0.2, 0.25, 0.35, 0.5, 0.8, 1.0};
    int si = 0;
    for (int i = 0; i < 8; i++) if (std::fabs(shapeVal[i] - S.cfg.alpha) < 1e-6) si = i;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Pulse shape")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##pskdpulseshape", &si, shapes, 8)) { S.cfg.alpha = shapeVal[si]; store(); }
    static const char* spans[] = {"auto (200 kHz)", "10 kHz", "50 kHz", "500 kHz", "2 MHz", "whole capture"};
    static const double spanVal[] = {0, 10e3, 50e3, 500e3, 2e6, 1e9};
    int pi = 0;
    for (int i = 0; i < 6; i++) if (std::fabs(spanVal[i] - S.cfg.spanHz) < 1) pi = i;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Search span")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##pskdsearchspan", &pi, spans, 6)) { S.cfg.spanHz = spanVal[pi]; store(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far around the dial the analyser looks; it takes the signal nearest the dial"));
    static const char* diffs[] = {"none", "DPSK (symbol differential)", "NRZ-M (1 = change)", "NRZ-S / NRZI (0 = change)"};
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Differential")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##pskddifferenti", &S.cfg.diff, diffs, 4)) store();
    static const char* scrs[] = {"none", "G3RUH (1 + x^12 + x^17)", "CCSDS randomiser"};
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Descrambler")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##pskddescramble", &S.cfg.scrambler, scrs, 3)) store();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Sync word")); ImGui::SameLine(170 * gUi); ImGui::SetNextItemWidth(w);
    if (ImGui::InputText("##pskdsyncword", S.sync, sizeof S.sync, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) { S.cfg.sync = S.sync; store(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Bits (0101...) or hex (1ACFFC1D for CCSDS). Fixes the phase ambiguity and starts each line of the bit view; empty: none"));
    { char lb[96]; snprintf(lb, sizeof lb, "%s##pskdinvertbits", tr("Invert bits")); if (ImGui::Checkbox(lb, &S.cfg.invert)) store(); }
    { char lb[96]; snprintf(lb, sizeof lb, "%s##pskdsnaptostan", tr("Snap to standard rates")); if (ImGui::Checkbox(lb, &S.cfg.snapStandard)) store(); }
    { char lb[96]; snprintf(lb, sizeof lb, "%s##pskdfollowthed", tr("Follow the Doppler")); if (ImGui::Checkbox(lb, &S.cfg.followDoppler)) store(); }
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* orders[] = {"BPSK", "QPSK", "OQPSK", "8PSK"};
    ImGui::SetNextItemWidth(90 * gUi);
    int o = std::clamp(sc.modeOpt[0], 0, 3);
    if (ImGui::Combo("##pskso", &o, orders, 4)) { sc.modeOpt[0] = o; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(110 * gUi);
    float baud = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1200.f;
    if (ImGui::SliderFloat("##psksb", &baud, 75.f, 100000.f, "%.0f Bd", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = baud; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    float dop = (float)sc.modeVal[2];
    if (ImGui::SliderFloat("##psksd", &dop, -300.f, 300.f, "%.0f Hz/s")) { sc.modeVal[2] = dop; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Doppler ramp of the test signal"));
    flowEnd();
}

} // namespace

extern const ModeUi kPskdUi;
const ModeUi kPskdUi = {
    .sideTitle = "ANALYSIS",
    .tabName = "PSK",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
};
