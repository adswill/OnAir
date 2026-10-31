// Shortwave mode screens: the General receiver's controls (frequency with kHz entry, demodulator, filter, squelch, S-meter, recording) over the
// spectrum and waterfall, a quick band selector (HF broadcast, ham, utility, digital calling frequencies; the last frequency of each band is
// remembered), a strip of one-click decoders (RTTY, CW, PSK31, FT8, SSTV, Olivia, ALE, NAVTEX, ...) and one pane per running decoder with its
// text or picture, Clear / Save / Log / Stop, and an Advanced foldout. Pressing a decoder starts it on the channel being listened to; tuning
// moves the decoders with the marker (the mode never changes and the frequency is never reset). Docs: docs/SHORTWAVE-MODE.md.
#include "app.h"
#include "gfx/gfx.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "platform/platform.h"
#include "dect2/shortwave/shortwave_route.h"
#include "dect2/shortwave/shortwave_tel.h"
#include "dect2/general/swbc.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/psk/psk_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using dect2::swr::Dec;

struct Band { const char* name; double loKhz, hiKhz, tuneKhz; };

// HF amateur bands (IARU region 1 limits; the tune point is a quiet spot in the middle, for listening)
const Band kHam[] = {
    {"160 m", 1810, 2000, 1900},  {"80 m", 3500, 3800, 3650}, {"60 m", 5351.5, 5366.5, 5358}, {"40 m", 7000, 7200, 7100}, {"30 m", 10100, 10150, 10125},
    {"20 m", 14000, 14350, 14200}, {"17 m", 18068, 18168, 18118}, {"15 m", 21000, 21450, 21225}, {"12 m", 24890, 24990, 24940}, {"10 m", 28000, 29700, 28500},
};
// Utility stations whose frequencies are published: time signals, NAVTEX, weather fax (DWD Pinneberg)
const Band kUtil[] = {
    {"WWV 2.5", 2499, 2501, 2500}, {"WWV 5", 4999, 5001, 5000}, {"WWV 10", 9999, 10001, 10000}, {"WWV 15", 14999, 15001, 15000}, {"WWV 20", 19999, 20001, 20000},
    {"CHU 3.33", 3329, 3331, 3330}, {"CHU 7.85", 7849, 7851, 7850}, {"CHU 14.67", 14669, 14671, 14670},
    {"NAVTEX 4209.5", 4208, 4211, 4209.5}, {"Fax 3855", 3854, 3856, 3855}, {"Fax 7880", 7879, 7881, 7880}, {"Fax 13882.5", 13881, 13884, 13882.5},
};
// Digital calling frequencies (dial, upper sideband). The tune point is 1.5 kHz above the dial: the middle of the 3 kHz the signals fill, so the decoders'
// window (1.5 kHz either side of the marker) covers it all.
const Band kDig[] = {
    {"FT8 40m", 7074, 7077, 7075.5}, {"FT8 30m", 10136, 10139, 10137.5}, {"FT8 20m", 14074, 14077, 14075.5}, {"FT8 17m", 18100, 18103, 18101.5}, {"FT8 15m", 21074, 21077, 21075.5},
    {"FT8 10m", 28074, 28077, 28075.5}, {"RTTY 80m", 3580, 3583, 3581.5}, {"RTTY 40m", 7040, 7043, 7041.5}, {"RTTY 20m", 14080, 14083, 14081.5},
    {"PSK31 40m", 7070, 7073, 7071.5}, {"PSK31 20m", 14070, 14073, 14071.5}, {"SSTV 20m", 14230, 14233, 14231.5}, {"WSPR 20m", 14095.6, 14098.6, 14097.1},
};

struct Ui {
    bool loaded = false;
    int group = 0;                           // 0 broadcast, 1 ham, 2 utility, 3 digital
    std::map<std::string, double> lastKhz;   // the last frequency in each band (preferences "sw.last.<band>")
    std::string lastBand;                    // the band the listening frequency is in now
    double enterKhz = 0;
    char enterBuf[32] = "";
    bool adv = false;
    bool logOn[dect2::swr::kDecCount] = {};
    std::string msg;
    double msgAt = 0;
    struct Tex { gfx::Image* img = nullptr; int w = 0, h = 0; uint64_t seq = ~0ull; };
    std::map<int, Tex> tex;
    std::map<int, float> paneH;
};
Ui U;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }

void say(const std::string& s) { U.msg = s; U.msgAt = ImGui::GetTime(); }

void load() {
    if (U.loaded) return;
    U.loaded = true;
    auto& p = plat::prefs();
    U.group = (int)std::clamp(p.getI("sw.group", 0), 0L, 3L);
    auto rd = [&](const char* nm) { const double v = p.getD((std::string("sw.last.") + nm).c_str(), 0); if (v > 0) U.lastKhz[nm] = v; };
    size_t n = 0;
    const dect2::SwBand* sb = dect2::swBroadcastBands(n);
    for (size_t i = 0; i < n; i++) rd(sb[i].name);
    for (const auto& b : kHam) rd(b.name);
    for (const auto& b : kUtil) rd(b.name);
    for (const auto& b : kDig) rd(b.name);
}

// the band a frequency (kHz) is in, or "" (broadcast bands, then ham, utility, digital)
std::string bandOf(double khz) {
    if (const dect2::SwBand* b = dect2::swBandAt(khz)) return b->name;
    for (const auto& b : kHam) if (khz >= b.loKhz && khz <= b.hiKhz) return b.name;
    return "";
}

dect2::ShortwaveReceiver& rx(App& a) { return a.engine.shortwave(); }
const dect2::ShortwaveTelemetry& T(const App& a) { return a.rx.shortwave; }
bool live(const App& a) { return a.rx.standard == 212; }

const dect2::SwDecoderStatus* statusOf(const App& a, Dec d) {
    if (!live(a)) return nullptr;
    for (const auto& s : T(a).decoders) if (s.id == (int)d) return &s;
    return nullptr;
}

// Pressing a decoder: it starts on the channel; the listening is put in a sideband that suits it when it is not one yet
void pressDecoder(App& a, Dec d) {
    auto& r = rx(a);
    if (r.decoderActive(d)) { r.stopDecoder(d); return; }
    r.startDecoder(d);
    const dect2::swr::DecoderInfo& info = dect2::swr::decoderInfo(d);
    const int cur = generalApiDemod();
    if (d == Dec::Cw) {
        if (cur != dect2::GenCw && cur != dect2::GenUsb && cur != dect2::GenLsb) { generalApiSetDemod(dect2::GenCw); generalApiSetWidthHz(500); }
    } else if (d != Dec::TimeSig && d != Dec::HfId && d != Dec::Navtex) {
        if (cur != dect2::GenUsb && cur != dect2::GenLsb) {
            generalApiSetDemod(info.sideband < 0 ? dect2::GenLsb : dect2::GenUsb);
            generalApiSetWidthHz(2700);
        }
    }
}

void bandButtons(App& a) {
    load();
    static const char* const kG[] = {"Broadcast", "Ham", "Utility", "Digital"};
    ImGui::TextDisabled("bands");
    for (int g = 0; g < 4; g++) {
        ImGui::SameLine(0, 4 * gUi);
        const bool sel = U.group == g;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(kG[g])) { U.group = g; plat::prefs().setI("sw.group", g); }
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 12 * gUi);
    ImGui::TextDisabled("kHz");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(88 * gUi);
    if (ImGui::InputText("##swkhz", U.enterBuf, sizeof U.enterBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
        const double khz = atof(U.enterBuf);
        if (khz > 0) generalSetListenHz(a, khz * 1e3);
        U.enterBuf[0] = 0;
    }
    tip("Tune by typing a frequency in kHz and pressing Enter (7040, 10130.5 ...). The decoders that run follow it.");
    // the buttons of the group
    auto jump = [&](const char* name, double khz) {
        const double cur = generalListenHz(a) / 1e3;
        const std::string was = bandOf(cur);
        if (!was.empty() && was != name) U.lastKhz[was] = cur;   // leaving a band keeps where we were in it
        double go = khz;
        auto it = U.lastKhz.find(name);
        if (it != U.lastKhz.end() && U.group != 3 && U.group != 2) go = it->second;
        generalSetListenHz(a, go * 1e3);
        plat::prefs().setD((std::string("sw.last.") + name).c_str(), go);
    };
    const std::string here = bandOf(generalListenHz(a) / 1e3);
    auto draw = [&](const char* name, double khz, const char* tipText) {
        const bool on = here == name;
        ImGui::PushID(name);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (flowNext(3 * gUi), ImGui::SmallButton(name)) jump(name, khz);
        if (on) ImGui::PopStyleColor();
        if (*tipText) tip(tipText);
        ImGui::PopID();
    };
    flowBreak();
    if (U.group == 0) {
        size_t n = 0;
        const dect2::SwBand* sb = dect2::swBroadcastBands(n);
        for (size_t i = 0; i < n; i++) {
            char t[160];
            snprintf(t, sizeof t, "%.0f - %.0f kHz%s. Opens at the last frequency you used in this band.", sb[i].startKhz, sb[i].endKhz, sb[i].tropical ? " (tropical zone)" : "");
            draw(sb[i].name, dect2::swBandCentreKhz(sb[i]), t);
        }
    } else if (U.group == 1) for (const auto& b : kHam) { char t[96]; snprintf(t, sizeof t, "%.1f - %.1f kHz. Opens at the last frequency you used in this band.", b.loKhz, b.hiKhz); draw(b.name, b.tuneKhz, t); }
    else if (U.group == 2) for (const auto& b : kUtil) draw(b.name, b.tuneKhz, "A published station: time signal, NAVTEX or weather fax.");
    else for (const auto& b : kDig) draw(b.name, b.tuneKhz, "A calling frequency (upper sideband dial). The marker sits 1.5 kHz above the dial, in the middle of the signals.");
    flowEnd();
    // remember the frequency of the band we are in
    const double khz = generalListenHz(a) / 1e3;
    if (!here.empty() && std::fabs(U.lastKhz[here] - khz) > 0.4) {
        U.lastKhz[here] = khz;
        plat::prefs().setD(("sw.last." + here).c_str(), khz);
    }
}

ImVec4 stateColor(int st) { return st >= 2 ? ImVec4(0.35f, 0.85f, 0.45f, 1) : st == 1 ? ImVec4(0.92f, 0.72f, 0.25f, 1) : ImVec4(0.55f, 0.58f, 0.62f, 1); }

void decoderStrip(App& a) {
    auto& r = rx(a);
    int n = 0;
    const dect2::swr::DecoderInfo* tab = dect2::swr::decoderTable(n);
    const int detected = live(a) ? T(a).detected : -1;
    ImGui::TextUnformatted("Decoders");
    ImGui::SameLine(0, 8 * gUi);
    const bool anyOn = !r.running().empty();
    ImGui::BeginDisabled(!anyOn);
    if (ImGui::SmallButton("None")) r.stopAllDecoders();
    ImGui::EndDisabled();
    tip("Stop every decoder (the listening goes on)");
    ImGui::SameLine(0, 6 * gUi);
    const bool autoOn = r.autoOn();
    if (autoOn) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton(autoOn ? "Auto: trying..." : "Auto")) r.setAuto(!autoOn);
    if (autoOn) ImGui::PopStyleColor();
    tip("Tries the likely text decoders (RTTY, CW, PSK31, Olivia, MFSK, ALE) on this channel at once and keeps the one that produces valid text. The others stop.");
    if (live(a) && T(a).autoPhase == 2 && T(a).autoPick >= 0) { ImGui::SameLine(0, 6 * gUi); ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "-> %s", dect2::swr::decoderInfo((Dec)T(a).autoPick).name); }
    else if (live(a) && T(a).autoPhase == 2) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("nothing decoded"); }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("press one to decode what you are tuned to");
    static const char* const kGroups[] = {"Text", "Weak signal", "Images", "Data links", "Other"};
    for (const char* g : kGroups) {
        bool first = true;
        for (int i = 0; i < n; i++) {
            if (strcmp(tab[i].group, g) != 0) continue;
            const Dec d = tab[i].id;
            if (first) { flowBreak(); ImGui::TextDisabled("%-11s", g); ImGui::SameLine(0, 4 * gUi); first = false; }
            const bool on = r.decoderActive(d);
            const auto* st = statusOf(a, d);
            ImGui::PushID(i);
            flowNext(3 * gUi);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool hot = detected == i && !on;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.46f, 0.30f, 1));
            if (ImGui::Button(tab[i].name)) pressDecoder(a, d);
            if (on) ImGui::PopStyleColor();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (on) dl->AddCircleFilled(ImVec2(p1.x - 5 * gUi, p0.y + 5 * gUi), 3.2f * gUi, ImGui::GetColorU32(stateColor(st ? st->state : 0)));
            if (hot) dl->AddRect(p0, p1, IM_COL32(90, 220, 220, 255), 3.f, 0, 2.f);
            if (ImGui::IsItemHovered()) {
                std::string t = std::string(tab[i].blurb);
                if (tab[i].needs) t += std::string("\n\nNeeds ") + tab[i].needs + ".";
                if (hot) t += "\n\nThe signal identifier thinks this signal is " + std::string(tab[i].name) + ".";
                ImGui::SetTooltip("%s", t.c_str());
            }
            ImGui::PopID();
        }
    }
    flowEnd();
    // decoders that need something from the user
    std::string needs;
    for (int i = 0; i < n; i++) if (tab[i].needs) needs += (needs.empty() ? "" : "; ") + std::string(tab[i].name) + " needs " + tab[i].needs;
    if (!needs.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Decoders available: %d. %s. OnAir does not ship it; see docs/CODECS.md.", n, needs.c_str()); ImGui::PopTextWrapPos(); }
}

void advanced(App& a) {
    auto& r = rx(a);
    if (!ImGui::TreeNode("Advanced##swadv")) return;
    const auto run = r.running();
    if (run.empty()) ImGui::TextDisabled("Start a decoder to set its options. Everything is automatic by default.");
    for (Dec d : run) {
        const auto& info = dect2::swr::decoderInfo(d);
        dect2::swr::Opts o = r.decoderOpts(d);
        bool ch = false;
        ImGui::PushID((int)d);
        ImGui::TextColored(ImVec4(0.92f, 0.78f, 0.46f, 1), "%s", info.name);
        int sb = o.sideband == 0 ? 0 : o.sideband > 0 ? 1 : 2;
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::Combo("##sb", &sb, "Sideband: default\0Upper\0Lower\0")) { o.sideband = sb == 0 ? 0 : sb == 1 ? 1 : -1; ch = true; }
        tip("The sideband the signal is sent in. Lower: the decoder gets the mirrored capture.");
        float pitch = o.pitchHz < 0 ? (float)info.pitchHz : (float)o.pitchHz;
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::SliderFloat("##pitch", &pitch, 0, 3000, "signal at %.0f Hz")) { o.pitchHz = pitch; ch = true; }
        tip("Where the marker's signal sits in the decoder's audio. The decoder's dial is put this far below the marker.");
        if (d == Dec::Rtty) {
            int baud = o.rttyBaud + 1, shift = o.rttyShift + 1;
            ImGui::SetNextItemWidth(110 * gUi);
            if (ImGui::Combo("##baud", &baud, "Baud: auto\00045.45\00050\00075\000100\000")) { o.rttyBaud = baud - 1; ch = true; }
            ImGui::SameLine(0, 6 * gUi);
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::Combo("##shift", &shift, "Shift: auto\000170 Hz\000200 Hz\000425 Hz\000450 Hz\000850 Hz\000")) { o.rttyShift = shift - 1; ch = true; }
            ImGui::SameLine(0, 6 * gUi);
            if (ImGui::Checkbox("Reverse", &o.rttyReverse)) ch = true;
            tip("Mark is the upper tone. Only used when baud and shift are fixed (automatic tries both polarities).");
        } else if (d == Dec::Psk) {
            ImGui::SetNextItemWidth(130 * gUi);
            if (ImGui::Combo("##pm", &o.pskMode, "BPSK31\000BPSK63\000QPSK31\000QPSK63\000BPSK125\000BPSK250\000QPSK125\000")) ch = true;
        }
        if (ch) r.setDecoderOpts(d, o);
        ImGui::PopID();
    }
    ImGui::TreePop();
}

std::string logDir() {
    std::error_code ec;
    const std::string dir = plat::dataDir() + "/shortwave-logs";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void updateTexture(App& a, int id, const dect2::SwImage& im, uint64_t seq) {
    if (!gGfx || im.w <= 0 || im.h <= 0) return;
    Ui::Tex& t = U.tex[id];
    if (t.seq == seq && t.img) return;
    if (!t.img || t.w != im.w || t.h != im.h) {
        delete t.img;
        t.img = gGfx->createImage(im.w, im.h, 0xFF000000u);
        t.w = im.w; t.h = im.h;
    }
    if (!t.img) return;
    std::vector<uint32_t> px((size_t)im.w * im.h);
    for (size_t i = 0; i < px.size(); i++) {
        if (im.gray) { const uint32_t g = i < im.px.size() ? im.px[i] : 0; px[i] = 0xFF000000u | (g << 16) | (g << 8) | g; }
        else if (i * 3 + 2 < im.px.size()) px[i] = 0xFF000000u | ((uint32_t)im.px[i * 3 + 2] << 16) | ((uint32_t)im.px[i * 3 + 1] << 8) | im.px[i * 3];
    }
    t.img->update(0, 0, im.w, im.h, px.data());
    t.seq = seq;
    (void)a;
}

void pane(App& a, Dec d, const dect2::SwDecoderStatus* st, float height) {
    auto& r = rx(a);
    const auto& info = dect2::swr::decoderInfo(d);
    ImGui::PushID((int)d);
    ImGui::BeginChild("##swpane", ImVec2(0, height), true);
    ImGui::TextColored(ImVec4(0.92f, 0.78f, 0.46f, 1), "%s", info.name);
    ImGui::SameLine(0, 8 * gUi);
    const int state = st ? st->state : 0;
    ImGui::TextColored(stateColor(state), "%s", st && !st->status.empty() ? st->status.c_str() : "starting");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x > 220 * gUi ? ImGui::GetWindowContentRegionMax().x - 215 * gUi : 0);
    if (ImGui::SmallButton("Clear")) r.clearDecoder(d);
    tip("Empty this pane");
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::SmallButton("Save")) {
        const std::string path = logDir() + "/" + info.key + "_" + std::to_string((long long)std::time(nullptr)) + ".txt";
        std::string err;
        say(r.saveDecoderText(d, path, &err) ? "Saved " + path : "Not saved: " + err);
    }
    tip("Save what this pane holds to a text file in the shortwave-logs folder of the data folder");
    ImGui::SameLine(0, 3 * gUi);
    bool lg = U.logOn[(int)d];
    if (ImGui::Checkbox("Log", &lg)) { U.logOn[(int)d] = lg; r.setDecoderLog(d, lg, logDir()); }
    tip("Append everything this decoder produces to a text file (shortwave-logs folder) as it arrives");
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::SmallButton("Stop")) r.stopDecoder(d);
    tip("Stop this decoder");
    if (st && !st->note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", st->note.c_str()); ImGui::PopTextWrapPos(); }
    if (st && st->image && st->image->w > 0) {
        updateTexture(a, (int)d, *st->image, st->imageSeq);
        auto it = U.tex.find((int)d);
        if (it != U.tex.end() && it->second.img) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float z = std::min(avail.x / (float)st->image->w, std::max(40.f, avail.y) / (float)st->image->h);
            ImGui::Image(it->second.img->texture(), ImVec2(st->image->w * z, st->image->h * z));
        }
    }
    if (st && !st->text.empty()) {
        ImGui::BeginChild("##swtext", ImVec2(0, 0), false);
        ImGui::PushFont(a.mono, 0);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(st->text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);   // follow the end unless scrolled back
        ImGui::EndChild();
    } else if (!(st && st->image)) ImGui::TextDisabled("%s", info.out == dect2::swr::Out::Status ? "status only: see the line above" : "nothing decoded yet");
    ImGui::EndChild();
    ImGui::PopID();
}

void panes(App& a) {
    auto& r = rx(a);
    const auto run = r.running();
    if (run.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Tune to a signal on the waterfall (click it), then press a decoder above. The text appears here and follows the marker when you retune.");
        ImGui::PopTextWrapPos();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float minH = 120 * gUi;
    const bool wide = avail.x >= 760 * gUi && run.size() > 1;
    if (wide) {
        const int cols = std::min<int>((int)run.size(), avail.x >= 1100 * gUi ? 3 : 2);
        const float w = (avail.x - (cols - 1) * 6 * gUi) / (float)cols;
        const float h = std::max(minH, avail.y);
        int k = 0;
        for (Dec d : run) {
            if (k % cols) ImGui::SameLine(0, 6 * gUi);
            ImGui::BeginChild(("##swcol" + std::to_string((int)d)).c_str(), ImVec2(w, std::max(minH, h / (float)((run.size() + cols - 1) / cols))), false);
            pane(a, d, statusOf(a, d), ImGui::GetContentRegionAvail().y);
            ImGui::EndChild();
            k++;
        }
    } else {
        const float h = std::max(minH, (avail.y - (run.size() - 1) * 4 * gUi) / (float)run.size());
        for (Dec d : run) pane(a, d, statusOf(a, d), h);
    }
}

void tab(App& a) {
    load();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool twoCol = avail.x >= 980 * gUi;
    if (twoCol) {
        const float lw = std::max(430.f * gUi, avail.x * 0.42f);
        ImGui::BeginChild("##swleft", ImVec2(lw, 0), false);
        generalControlsPanel(a);
        ImGui::EndChild();
        ImGui::SameLine(0, 8 * gUi);
        ImGui::BeginChild("##swright", ImVec2(0, 0), false);
        bandButtons(a);
        ImGui::Separator();
        decoderStrip(a);
        advanced(a);
        if (!U.msg.empty() && ImGui::GetTime() - U.msgAt < 8) ImGui::TextDisabled("%s", U.msg.c_str());
        ImGui::Separator();
        panes(a);
        ImGui::EndChild();
    } else {
        bandButtons(a);
        ImGui::Separator();
        decoderStrip(a);
        advanced(a);
        if (!U.msg.empty() && ImGui::GetTime() - U.msgAt < 8) ImGui::TextDisabled("%s", U.msg.c_str());
        ImGui::Separator();
        const float ph = std::max(220.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
        ImGui::BeginChild("##swpanes", ImVec2(0, ph), false);
        panes(a);
        ImGui::EndChild();
        if (ImGui::CollapsingHeader("Receiver controls", ImGuiTreeNodeFlags_DefaultOpen)) generalControlsPanel(a);
    }
    if (!gSpectrumInShell) spectrumWaterfall(a, specWaterfallSplit(), false);
}

void receiver(App& a) {
    const auto& t = T(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::TextWrapped("%s", dect2::shortwaveSummary(t).c_str());
    ImGui::Text("Listening on %.4f MHz, channel %+.3f kHz from the capture centre", generalListenHz(a) / 1e6, t.channelOffsetHz / 1e3);
    ImGui::Text("Input %.3f Msps", t.inputRate / 1e6);
    for (const auto& d : t.decoders) ImGui::Text("%s: %s", dect2::swr::decoderInfo((Dec)d.id).name, d.status.c_str());
}

void list(App& a) {
    ImGui::TextDisabled("%.4f MHz", generalListenHz(a) / 1e6);
    const auto run = rx(a).running();
    if (run.empty()) ImGui::TextDisabled("no decoder");
    for (Dec d : run) {
        const auto* st = statusOf(a, d);
        ImGui::TextColored(stateColor(st ? st->state : 0), "%s", dect2::swr::decoderInfo(d).name);
    }
}

void status(App& a) {
    StatusPanel panel;
    const bool on = live(a);
    lamp("RX", on ? (T(a).general.state == 2 ? 1 : T(a).general.state == 1 ? 2 : 3) : 0, (int)Ic::Speaker);
    flowNext(12 * gUi);
    ImGui::TextUnformatted(on ? dect2::shortwaveSummary(T(a)).c_str() : (a.engine.running() ? "starting" : "stopped"));
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    char b[96];
    snprintf(b, sizeof b, "%s %.4f MHz", dect2::generalDemodName(generalApiDemod()), generalListenHz(a) / 1e6);
    l1 = b;
    if (live(a)) l2 = dect2::shortwaveSummary(T(a));
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const auto& t = T(a).general;
    out.push_back({"LEVEL  dBFS", "%.0f", (double)t.levelDb, -130, 0, t.squelchOpen ? 1 : 0});
    out.push_back({"SNR  dB", "%.0f", (double)t.snrDb, 0, 40, !t.dataValid ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 6 ? 2 : 3});
}

// dev / test only: DECT2_SW_DECODERS="rtty,cw" presses those buttons once the radio runs (hidden --shot runs), through the same code as a click
void devPress(App& a) {
    static const char* env = getenv("DECT2_SW_DECODERS");
    static bool done = false;
    if (done || !env || !*env || !live(a)) return;
    done = true;
    std::string s = env;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = std::min(s.find(',', i), s.size());
        bool ok = false;
        const Dec d = dect2::swr::decoderFromKey(s.substr(i, j - i), &ok);
        if (ok) pressDecoder(a, d);
        i = j + 1;
    }
}

void tick(App& a) {
    load();
    generalTick(a);
    devPress(a);   // the same push to the receiver as the General mode (demodulator, filter, squelch, channel marker, volume, recording)
    auto& r = rx(a);
    for (int i = 0; i < dect2::swr::kDecCount; i++)   // a log switch that survived a mode change: the receiver starts with none
        if (U.logOn[i] && !r.decoderLogging((Dec)i)) r.setDecoderLog((Dec)i, true, logDir());
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("a slice of an HF band: an AM station 25 kHz above the dial, RTTY (centre 51.36 kHz), a CW carrier on it and a BPSK31 signal, over noise");
    ImGui::PopTextWrapPos();
    int w = std::clamp(sc.modeOpt[0], 0, 5);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::Combo("Signals##swsyn", &w, "All four\0AM only\0RTTY only\0CW only\0PSK31 only\0Noise only\0")) { sc.modeOpt[0] = w; changed = true; }
}

}  // namespace

extern const ModeUi kShortwaveUi;
const ModeUi kShortwaveUi = {
    .sideTitle = "DECODERS",
    .tabName = "Shortwave",
    .tabIcon = Ic::Speaker,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
