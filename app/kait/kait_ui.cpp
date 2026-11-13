// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT screens: the receiver tab (the picture as it decodes, layer by layer, with the link numbers) and the transmit options (callsign, picture file, profile)
// that the TX panel draws for the KAIT signal. English texts only.
#include "app.h"
#include "../panels/enhance_ui.h"
#include "../panels/tx_state.h"
#include "dect2/kait/kait_frame.h"
#include "dect2/kait/kait_image.h"
#include "dect2/kait/kait_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool live(const App& a) { return a.rx.standard == 132; }   // the engine reports its standard code minus one

struct Pic {
    gfx::Image* img = nullptr;
    int w = 0, h = 0;
    uint64_t seq = 0;
    std::string saveMsg;
    enhui::LiveTex enh;   // the Enhance control on the picture
} P;

void uploadPicture(const dect2::KaitTelemetry& t) {
    if (!gGfx || !t.image || t.imageSeq == 0 || t.image->w <= 0 || t.image->h <= 0) return;
    if (P.seq == t.imageSeq && P.img) return;
    if (!P.img || P.w != t.image->w || P.h != t.image->h) {
        delete P.img;
        P.img = gGfx->createImage(t.image->w, t.image->h, 0xFF000000u);
        P.w = t.image->w;
        P.h = t.image->h;
    }
    std::vector<uint32_t> px((size_t)P.w * (size_t)P.h);
    for (size_t i = 0; i < px.size(); i++) {
        const uint8_t* c = &t.image->rgb[3 * i];
        px[i] = 0xFF000000u | ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0];
    }
    P.img->update(0, 0, P.w, P.h, px.data());
    P.enh.e.setSource("KAIT picture");
    P.enh.feed(px.data(), P.w, P.h, t.state >= 3);
    P.seq = t.imageSeq;
}

void savePicture(const dect2::KaitTelemetry& t) {
    if (!t.image) return;
    const std::string path = saveFileDialog("kait.png");
    if (path.empty()) return;
    std::vector<uint8_t> rgba((size_t)t.image->w * (size_t)t.image->h * 4);
    for (size_t i = 0; i < (size_t)t.image->w * (size_t)t.image->h; i++) {
        rgba[4 * i] = t.image->rgb[3 * i];
        rgba[4 * i + 1] = t.image->rgb[3 * i + 1];
        rgba[4 * i + 2] = t.image->rgb[3 * i + 2];
        rgba[4 * i + 3] = 255;
    }
    P.saveMsg = gfx::writePng(path.c_str(), rgba.data(), t.image->w, t.image->h) ? "saved " + path : "could not write " + path;
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAIT sends a 1080p picture in a 10 s burst, 190 kHz wide, in layers: a coarse picture first in the most robust coding, then more detail in less robust "
                        "codings. The picture shown is what the layers that decoded give, so a weak signal still shows an outline. Tune to the signal and wait for a burst; "
                        "the picture appears when the burst ends and builds up layer by layer. To transmit, switch to TX and pick KAIT.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }

    const dect2::KaitTelemetry& t = a.rx.kait;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    const ImVec4 col = t.state >= 3 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 2 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1);
    ImGui::TextColored(col, "%s", dect2::kaitSummary(t).c_str());
    if (t.state == 2) ImGui::ProgressBar(std::max(0.f, std::min(1.f, (float)(t.captureSec / 10.0))), ImVec2(260 * gUi, 0), "");
    ImGui::Spacing();

    uploadPicture(t);
    if (P.img && t.image) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float maxH = std::max(120.f * gUi, avail.y * 0.62f);
        float sc = std::min(avail.x / (float)P.w, maxH / (float)P.h);
        sc = std::max(0.05f, sc);
        ImGui::Image(P.enh.texture(P.img)->texture(), ImVec2((float)P.w * sc, (float)P.h * sc));
        P.enh.controls("kaitenh");
    } else {
        ImGui::TextDisabled("%s", "no picture yet");
    }
    if (t.image) {
        if (ImGui::Button("Save picture...")) savePicture(t);
        if (!P.saveMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", P.saveMsg.c_str()); }
    }
    ImGui::Spacing();

    char b[200];
    if (t.headerOk) {
        row("station", t.callsign);
        snprintf(b, sizeof b, "%d x %d", t.width, t.height);
        row("picture", b);
        const dect2::kait::Profile* pr = dect2::kait::profileById(t.profile);
        row("profile", pr ? pr->name : "?");
        snprintf(b, sizeof b, "%.1f dB (in 190 kHz)", (double)t.snrDb);
        row("SNR", b);
        snprintf(b, sizeof b, "%+.1f Hz, clock %+.1f ppm", t.cfoHz, t.clockPpm);
        row("CFO", b);
        snprintf(b, sizeof b, "%zu of %zu bytes", t.bytesGood, t.bytesTotal);
        row("stream", b);
        snprintf(b, sizeof b, "%d of %d", t.layersComplete, t.nLayers);
        row("layers complete", b);
        ImGui::Spacing();
        for (size_t i = 0; i < t.layerBlocks.size() && i < t.layerBlocksOk.size(); i++) {
            const int n = t.layerBlocks[i], ok = t.layerBlocksOk[i];
            snprintf(b, sizeof b, "layer %zu", i);
            ImGui::TextDisabled("%s", b);
            kvColumn(130 * gUi);
            snprintf(b, sizeof b, "%d / %d blocks", ok, n);
            ImGui::ProgressBar(n > 0 ? (float)ok / (float)n : 0.f, ImVec2(200 * gUi, 0), b);
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%llu burst%s seen, %llu with a good header; input %.0f Hz", (unsigned long long)t.bursts, t.bursts == 1 ? "" : "s", (unsigned long long)t.burstsDecoded, t.inputRate);
}

void receiver(App& a) {
    if (!live(a)) return;
    ImGui::Text("%s", dect2::kaitSummary(a.rx.kait).c_str());
}

void list(App&) {}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::kaitSummary(a.rx.kait).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAIT";
    if (live(a)) l2 = dect2::kaitSummary(a.rx.kait);
}

} // namespace

extern const ModeUi kKaitUi;
const ModeUi kKaitUi = {
    .sideTitle = "BURSTS",
    .tabName = "KAIT",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
bool endsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
    return true;
}
const char* const kProfiles[4] = {"Balanced (layers from -12 dB to 10 dB)", "Robust (weak link: nothing above QPSK 2/3)", "Quality (link above 6 dB, up to 64QAM)", "Direct (link holds 8 dB: one big layer)"};
}

// The file the generator reads: a PNG or PPM goes as it is, any other picture the system can decode is written as a PPM in the temporary folder.
std::string kaitPictureFile(const std::string& path) {
    if (path.empty() || endsWithNoCase(path, ".png") || endsWithNoCase(path, ".ppm")) return path;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!plat::decodeImage(path, w, h, px) || w < 1 || h < 1 || px.size() < (size_t)w * (size_t)h) return path;   // the generator reports it
    dect2::kait::Image im;
    im.w = w; im.h = h;
    im.rgb.resize((size_t)w * (size_t)h * 3);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        im.rgb[3 * i] = (uint8_t)(px[i] & 0xFF);
        im.rgb[3 * i + 1] = (uint8_t)((px[i] >> 8) & 0xFF);
        im.rgb[3 * i + 2] = (uint8_t)((px[i] >> 16) & 0xFF);
    }
    const std::string out = (std::filesystem::temp_directory_path() / "onair_kait_picture.ppm").string();
    return dect2::kait::writePpm(out, dect2::kait::fitPicture(im)) ? out : path;
}

// KAIT transmit options in the TX panel. The callsign, the picture file and the profile are saved in the settings.
void kaitPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kaitLoaded) {
        s.kaitLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kait.callsign", s.callsign).c_str());
        snprintf(s.file, sizeof s.file, "%s", p.getS("tx.kait.file", "").c_str());
        s.kaitProfile = std::max(0, std::min(3, (int)p.getI("tx.kait.profile", 0)));
    }
    ImGui::TextWrapped("%s", "KAIT: a picture in a 10 s burst, 190 kHz wide. The picture is shrunk to fit 1920 x 1080 and coded in layers, the coarse picture in the most "
                              "robust coding first. Empty file: a demo picture. Empty callsign: ONAIR. OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (9 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 9);
    ImGui::SetNextItemWidth(std::max(200.f * gUi, ImGui::GetContentRegionAvail().x - 260 * gUi));
    ImGui::InputText("Picture file (PNG, PPM, JPEG ...)", s.file, sizeof s.file);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kait")) {
        const std::string f = openFileDialog();
        if (!f.empty()) snprintf(s.file, sizeof s.file, "%s", f.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Demo picture##kait")) s.file[0] = 0;
    ImGui::SetNextItemWidth(360 * gUi);
    if (ImGui::BeginCombo("Profile", kProfiles[s.kaitProfile])) {
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(kProfiles[i], i == s.kaitProfile)) s.kaitProfile = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", "One burst is 10 s, then 2 s of silence; it repeats while the transmission runs. Receivers show what their link decodes.");

    char sig[700];
    snprintf(sig, sizeof sig, "%s|%s|%d", s.callsign, s.file, s.kaitProfile);
    if (s.kaitSaved != sig) {
        s.kaitSaved = sig;
        p.setS("tx.kait.callsign", s.callsign);
        p.setS("tx.kait.file", s.file);
        p.setI("tx.kait.profile", s.kaitProfile);
    }
}

} // namespace txui
