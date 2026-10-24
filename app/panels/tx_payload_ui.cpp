// The signal's own options in step 2 of the transmit panel: what to say (callsign, text, IDs), where the audio / video / file comes from, and the standard's
// own choices, one block per kind of signal (TxPayloadKind). Empty fields fall back to the mode's demo signal; nothing here limits what is typed.
#include "tx_state.h"
#include "../ui/typography.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2;

namespace txui {

bool isVoiceId(const std::string& id) {
    return id == "fm" || id == "airband" || id == "qo100" ||
           id == "m17" || id == "freedv" || id == "dmr" || id == "p25" || id == "nxdn" ||
           id == "dpmr" || id == "dstar" || id == "tetra" || id == "kaal";
}
bool isFtxId(const std::string& id) {
    return id == "ft8" || id == "ft4" || id == "wspr" || id == "ft2" || id == "jt65" ||
           id == "jt9" || id == "q65" || id == "fst4" || id == "msk144";
}
bool isKbdId(const std::string& id) {
    return id == "olivia" || id == "contestia" || id == "mfsk" || id == "thor" || id == "dominoex" || id == "hell";
}
bool isTvId(const std::string& id) {
    return id == "dvbt2" || id == "dvbt" || id == "atsc" || id == "atsc3" || id == "dab" ||
           id == "isdbt" || id == "dvbs" || id == "dtmb" || id == "dvbc";
}

void audioSourceRow(TxUi& s, bool running) {
    const char* src[] = {tr("Microphone"), tr("WAV / MP3 file"), tr("1 kHz test tone"), tr("Silence")};
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo(tr("Audio input"), src[std::max(0, std::min(3, s.audioSrc))])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(src[i], s.audioSrc == i)) s.audioSrc = i;
        ImGui::EndCombo();
    }
    if (s.audioSrc == 0) {
        const auto devs = listAudioInputs();
        const char* cur = s.audioDevice[0] ? s.audioDevice : tr("Default input");
        ImGui::SetNextItemWidth(280 * gUi);
        if (ImGui::BeginCombo(tr("Input device"), cur)) {
            for (const auto& d : devs) {
                const bool sel = std::string(s.audioDevice) == d.id;
                if (ImGui::Selectable(d.name.c_str(), sel)) snprintf(s.audioDevice, sizeof s.audioDevice, "%s", d.id.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox(tr("Mute"), &s.audioMute);
        ImGui::SetNextItemWidth(180 * gUi);
        ImGui::SliderFloat(tr("Mic level"), &s.micGain, 0.f, 2.f, "%.2f");
        ImGui::TextDisabled("%s", tr("The microphone starts only when you press START TRANSMITTING."));
    } else if (s.audioSrc == 1) {
        ImGui::SetNextItemWidth(320 * gUi);
        ImGui::InputText(tr("Audio file"), s.file, sizeof s.file);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse…##audio"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str()); }
    }
    if (running) {
        const float pk = s.session.stats().audioPeak;
        ImGui::ProgressBar(pk, ImVec2(220 * gUi, 0), tr("Mic level"));
    }
}

// Hellschreiber: the font file for characters the built-in glyphs cannot draw (Chinese, Japanese, Korean, symbols). Under Advanced; empty = built in.
void hellFontRow() {
    static bool init = false;
    static char path[512] = "";
    static std::string msg;
    auto apply = [&](const std::string& p) {
        std::string err;
        if (dect2::kbd::hellLoadUserFont(p, err)) { msg = p.empty() ? std::string() : std::string(tr("Font loaded.")); plat::prefs().setS("hellFont", p); }
        else { msg = tr(err.c_str()); }
    };
    if (!init) { init = true; snprintf(path, sizeof path, "%s", plat::prefs().getS("hellFont", "").c_str()); if (path[0]) apply(path); }
    if (!ImGui::TreeNode(tr("Advanced: font for other scripts"))) return;
    ImGui::TextWrapped("%s", tr("Letters, digits, accented Latin, Cyrillic and Greek are built in. For Chinese, Japanese, Korean or symbols choose a .ttf or .otf font file that has them "
                                "(for example GNU Unifont or Noto Sans CJK, downloaded from their own sites; OnAir does not ship it). Characters the font lacks stay '?'."));
    ImGui::SetNextItemWidth(320 * gUi);
    ImGui::InputText(tr("Font file"), path, sizeof path);
    ImGui::SameLine();
    if (ImGui::Button(tr("Browse…##hellfont"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(path, sizeof path, "%s", p.c_str()); }
    if (ImGui::Button(tr("Use this font"))) apply(path);
    ImGui::SameLine();
    if (ImGui::Button(tr("Built-in glyphs only"))) { path[0] = 0; apply(""); }
    if (!msg.empty()) ImGui::TextDisabled("%s", msg.c_str());
    ImGui::TreePop();
}

void browseFile(TxUi& s, const char* id) {
    const std::string p = openFileDialog();
    if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str());
    (void)id;
}

void fileRow(TxUi& s, const char* label, const char* btnId) {
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputText(label, s.file, sizeof s.file);
    ImGui::SameLine();
    if (ImGui::Button(btnId)) browseFile(s, btnId);
}

void analogNfmBox(TxUi& s, bool running) {
    ImGui::Checkbox(tr("Send analog NFM of the microphone or a file instead of the test scene"), &s.analogVoice);
    if (s.analogVoice) audioSourceRow(s, running);
}

void muxIdentity(TxUi& s) {
    typo::separatorText(tr("Multiplex"));
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputText(tr("Network name"), s.network, sizeof s.network);
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputText(tr("Provider"), s.provider, sizeof s.provider);
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputInt(tr("First service ID"), &s.serviceId);
    if (s.serviceId < 1) s.serviceId = 1;
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::SliderInt(tr("Services"), &s.nsvc, 1, 4)) s.nsvc = std::max(1, std::min(4, s.nsvc));
    ImGui::TextDisabled("%s", tr("Each service is its own programme (own video and audio PIDs in the PAT / PMT / SDT). The whole-mux MPEG-TS above, if set, replaces this."));
    const auto cams = listVideoInputs();
    for (int i = 0; i < s.nsvc; i++) {
        char lb[48];
        snprintf(lb, sizeof lb, "%s %d", tr("Service name"), i + 1);
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(lb, s.svc[i], sizeof s.svc[i]);
        const char* srcn[] = {tr("Test card"), tr("Video file"), tr("Camera / capture card")};
        snprintf(lb, sizeof lb, "%s %d", tr("Programme source"), i + 1);
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(lb, srcn[std::max(0, std::min(2, s.svcSrc[i]))])) {
            for (int k = 0; k < 3; k++) if (ImGui::Selectable(srcn[k], s.svcSrc[i] == k)) s.svcSrc[i] = k;
            ImGui::EndCombo();
        }
        if (s.svcSrc[i] == 1) {
            snprintf(lb, sizeof lb, "%s %d", tr("Video file"), i + 1);
            ImGui::SetNextItemWidth(220 * gUi);
            ImGui::InputText(lb, s.svcFile[i], sizeof s.svcFile[i]);
            ImGui::SameLine();
            snprintf(lb, sizeof lb, "%s##svcfile%d", tr("Browse…"), i);
            if (ImGui::Button(lb)) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.svcFile[i], sizeof s.svcFile[i], "%s", p.c_str()); }
        } else if (s.svcSrc[i] == 2) {
            snprintf(lb, sizeof lb, "%s %d", tr("Camera"), i + 1);
            const char* cur = tr("Default camera");
            for (const auto& d : cams) if (s.svcCam[i][0] && std::string(s.svcCam[i]) == d.id) { cur = d.name.c_str(); break; }
            ImGui::SetNextItemWidth(280 * gUi);
            if (ImGui::BeginCombo(lb, cur)) {
                if (ImGui::Selectable(tr("Default camera"), s.svcCam[i][0] == 0)) s.svcCam[i][0] = 0;
                for (const auto& d : cams) {
                    const bool sel = std::string(s.svcCam[i]) == d.id;
                    if (ImGui::Selectable(d.name.c_str(), sel)) snprintf(s.svcCam[i], sizeof s.svcCam[i], "%s", d.id.c_str());
                }
                ImGui::EndCombo();
            }
        }
    }
}

void tvFilePanel(TxUi& s, const TxModeSpec& mode, bool running) {
    (void)running;
    const std::string id = mode.id ? mode.id : "";
    fileRow(s, tr("MPEG-TS / video file"), tr("Browse…##ts"));
    ImGui::TextDisabled("%s", tr("Your .ts / .m2ts is sent as-is (service names come from that file). Other video is remuxed when FFmpeg can. Empty: OnAir encodes a test card with the names below."));
    muxIdentity(s);
    if (!s.fileNote.empty()) ImGui::TextWrapped("%s", s.fileNote.c_str());
    if (id == "atsc3") ImGui::TextDisabled("%s", tr("ATSC 3.0 uses a ROUTE / fMP4 demo; a custom MPEG-TS is not the payload path."));
    if (id == "dvbt2" || id == "dvbt") {
        const char* names[] = {"8 MHz", "7 MHz", "6 MHz", "5 MHz"};
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::BeginCombo(tr("Channel width"), names[s.bwIdx])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(names[i], i == s.bwIdx)) s.bwIdx = i;
            ImGui::EndCombo();
        }
        if (id == "dvbt2" || s.family == 0) {
            const char* dvb[] = {"DVB-T2", "DVB-T"};
            const int vi = s.dvbVariant == 2 ? 1 : 0;
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::BeginCombo(tr("DVB standard"), dvb[vi])) {
                if (ImGui::Selectable("DVB-T2", vi == 0)) { s.dvbVariant = 1; s.modeId = "dvbt2"; }
                if (ImGui::Selectable("DVB-T", vi == 1)) { s.dvbVariant = 2; s.modeId = "dvbt"; }
                ImGui::EndCombo();
            }
        }
    }
    if (id == "dvbs") {
        if (s.synth.modeVal[0] <= 0) s.synth.modeVal[0] = 5e6;
        ImGui::InputDouble(tr("Symbol rate (Hz)"), &s.synth.modeVal[0], 0, 0, "%.0f");
        const char* stdn[] = {"DVB-S2", "DVB-S", "DVB-S2X"};
        int st = std::max(0, std::min(2, s.synth.modeOpt[0]));
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::BeginCombo(tr("Satellite standard"), stdn[st])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(stdn[i], st == i)) s.synth.modeOpt[0] = i;
            ImGui::EndCombo();
        }
        const char* modn[] = {"QPSK", "8PSK", "16APSK", "32APSK"};
        int md = std::max(0, std::min(3, s.synth.modeOpt[1]));
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::BeginCombo(tr("Modulation"), modn[md])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(modn[i], md == i)) s.synth.modeOpt[1] = i;
            ImGui::EndCombo();
        }
        ImGui::InputInt(tr("Code rate / MODCOD index"), &s.synth.modeOpt[2]);
        const char* ro[] = {"0.35", "0.25", "0.20", "0.15", "0.10", "0.05"};
        int ri = std::max(0, std::min(5, s.synth.modeOpt[3]));
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::BeginCombo(tr("Roll-off"), ro[ri])) {
            for (int i = 0; i < 6; i++) if (ImGui::Selectable(ro[i], ri == i)) s.synth.modeOpt[3] = i;
            ImGui::EndCombo();
        }
    }
}

void payloadPanel(TxUi& s, const TxModeSpec& mode, bool running) {
    loadPayloadPrefs(s);
    const std::string id = mode.id ? mode.id : "";
    typo::separatorText(tr("What to send"));
    const TxPayloadKind k = txPayloadKind(id);
    switch (k) {
    case TxPayloadKind::TvFile:
        tvFilePanel(s, mode, running);
        break;
    case TxPayloadKind::AnalogTv: {
        ImGui::TextWrapped("%s", tr("Composite analog television: vision carrier plus a separate sound carrier. The picture can be the test card, a video file, or a live camera / capture card."));
        {
            const char* vsrc[] = {tr("Test card / bars / ramp"), tr("Video file"), tr("Camera / capture card")};
            ImGui::SetNextItemWidth(240 * gUi);
            if (ImGui::BeginCombo(tr("Picture source"), vsrc[std::max(0, std::min(2, s.videoSrc))])) {
                for (int i = 0; i < 3; i++) if (ImGui::Selectable(vsrc[i], s.videoSrc == i)) s.videoSrc = i;
                ImGui::EndCombo();
            }
            if (s.videoSrc == 1) {
                ImGui::SetNextItemWidth(220 * gUi);
                ImGui::InputText(tr("Video file"), s.file, sizeof s.file);
                ImGui::SameLine();
                if (ImGui::Button(tr("Browse…##atvvid"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str()); }
            } else if (s.videoSrc == 2) {
                const auto cams = listVideoInputs();
                const char* cur = tr("Default camera");
                for (const auto& d : cams) if (s.videoDevice[0] && std::string(s.videoDevice) == d.id) { cur = d.name.c_str(); break; }
                ImGui::SetNextItemWidth(280 * gUi);
                if (ImGui::BeginCombo(tr("Camera / capture card"), cur)) {
                    if (ImGui::Selectable(tr("Default camera"), s.videoDevice[0] == 0)) s.videoDevice[0] = 0;
                    for (const auto& d : cams) {
                        const bool sel = std::string(s.videoDevice) == d.id;
                        if (ImGui::Selectable(d.name.c_str(), sel)) snprintf(s.videoDevice, sizeof s.videoDevice, "%s", d.id.c_str());
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("%s", tr("The camera starts only when you press START TRANSMITTING."));
            }
        }
        const char* sysn[] = {"B/G", "B (7 MHz)", "I", "D/K", "M (NTSC)", "N", "A (405)", "C", "E (819)", "H", "K'", "L", "L'"};
        int so = std::max(0, std::min(12, s.synth.modeOpt[0]));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::BeginCombo(tr("System"), sysn[so])) {
            for (int i = 0; i < 13; i++) if (ImGui::Selectable(sysn[i], i == so)) s.synth.modeOpt[0] = i;
            ImGui::EndCombo();
        }
        const char* coln[] = {"usual for the system", "PAL", "NTSC", "SECAM", "monochrome"};
        int co = std::max(0, std::min(4, s.synth.modeOpt[1]));
        ImGui::SetNextItemWidth(180 * gUi);
        if (ImGui::BeginCombo(tr("Colour"), coln[co])) {
            for (int i = 0; i < 5; i++) if (ImGui::Selectable(coln[i], i == co)) s.synth.modeOpt[1] = i;
            ImGui::EndCombo();
        }
        const char* picn[] = {"test card", "colour bars", "grey ramp"};
        int po = std::max(0, std::min(2, s.synth.modeOpt[2]));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::BeginCombo(tr("Picture"), picn[po])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(picn[i], i == po)) s.synth.modeOpt[2] = i;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputText(tr("Ident on the card"), s.name, sizeof s.name);
        ImGui::TextDisabled("%s", tr("Empty: ONAIR TEST. This is the station name drawn on the picture, not a file."));
        const char* sndn[] = {"1 kHz tone (with gaps)", "melody", "sound carrier only", "no sound carrier", "microphone", "WAV / MP3 file"};
        int snd = s.audioSrc == 0 ? 4 : s.audioSrc == 1 ? 5 : std::max(0, std::min(3, s.synth.modeOpt[3]));
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("Sound carrier"), sndn[snd])) {
            for (int i = 0; i < 6; i++) if (ImGui::Selectable(sndn[i], i == snd)) {
                if (i < 4) { s.audioSrc = 2; s.synth.modeOpt[3] = i; }
                else { s.audioSrc = i == 4 ? 0 : 1; s.synth.modeOpt[3] = 0; }
            }
            ImGui::EndCombo();
        }
        if (s.audioSrc == 0 || s.audioSrc == 1) audioSourceRow(s, running);
        break;
    }
    case TxPayloadKind::BroadcastAudio:
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputText(tr("Ensemble / station name"), s.network, sizeof s.network);
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputText(tr("Label / first service"), s.name, sizeof s.name);
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputText(tr("Text (DRM)"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderInt(tr("Named services"), &s.nsvc, 1, 4)) s.nsvc = std::max(1, std::min(4, s.nsvc));
        for (int i = 0; i < s.nsvc; i++) {
            char lb[40]; snprintf(lb, sizeof lb, "%s %d", tr("Service"), i + 1);
            ImGui::SetNextItemWidth(220 * gUi);
            ImGui::InputText(lb, s.svc[i], sizeof s.svc[i]);
        }
        ImGui::TextDisabled("%s", tr("Names go out in the signalling (DAB FIGs, DRM SDC). The audio is still the built-in tones / melody: this encoder does not remux an arbitrary playlist."));
        break;
    case TxPayloadKind::Ftx:
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::InputText(tr("Grid"), s.grid, sizeof s.grid)) saveStr("tx.grid", s.grid);
        if (id == "wspr") ImGui::InputInt(tr("Power (dBm)"), &s.dbm);
        else {
            const char* kinds[] = {tr("CQ"), tr("Report"), tr("Free text")};
            ImGui::SetNextItemWidth(140 * gUi);
            if (ImGui::BeginCombo(tr("Message"), kinds[std::max(0, std::min(2, s.kind))])) {
                for (int i = 0; i < 3; i++) if (ImGui::Selectable(kinds[i], s.kind == i)) s.kind = i;
                ImGui::EndCombo();
            }
            if (s.kind == 1) ImGui::InputText(tr("To"), s.dest, sizeof s.dest);
            if (s.kind == 2) {
                if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
            }
        }
        break;
    case TxPayloadKind::Rtty:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        {
            const char* bauds[] = {"45.45", "50", "75", "100"};
            const char* shifts[] = {"170 Hz", "200 Hz", "425 Hz", "450 Hz", "850 Hz"};
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::BeginCombo(tr("Baud"), bauds[std::max(0, std::min(3, s.baudIdx))])) {
                for (int i = 0; i < 4; i++) if (ImGui::Selectable(bauds[i], s.baudIdx == i)) s.baudIdx = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::BeginCombo(tr("Shift"), shifts[std::max(0, std::min(4, s.shiftIdx))])) {
                for (int i = 0; i < 5; i++) if (ImGui::Selectable(shifts[i], s.shiftIdx == i)) s.shiftIdx = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80 * gUi);
            if (ImGui::BeginCombo(tr("Sideband"), s.variant == 1 ? "LSB" : "USB")) {
                if (ImGui::Selectable("USB", s.variant == 0)) s.variant = 0;
                if (ImGui::Selectable("LSB", s.variant == 1)) s.variant = 1;
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox(tr("Reverse (mark is the upper tone)"), &s.reverse);
        break;
    case TxPayloadKind::Keyboard:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        if (mode.id && std::string(mode.id) == "hell") hellFontRow();
        break;
    case TxPayloadKind::Cw:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::InputInt(tr("WPM"), &s.wpm);
        break;
    case TxPayloadKind::Psk:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::InputInt(tr("PSK variant"), &s.variant);
        break;
    case TxPayloadKind::Js8:
        ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign);
        ImGui::InputText(tr("Grid"), s.grid, sizeof s.grid);
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        break;
    case TxPayloadKind::Hamdrm:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        fileRow(s, tr("File to send"), tr("Browse…##hamdrm"));
        ImGui::TextDisabled("%s", tr("The file bytes go out as a HamDRM object with this callsign. Empty file: the test picture."));
        break;
    case TxPayloadKind::Sstv:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        {
            const int nModes = sstvModeCount();
            const char* cur = sstvModeInfo(std::max(0, std::min(nModes - 1, s.variant))).name;
            ImGui::SetNextItemWidth(220 * gUi);
            if (ImGui::BeginCombo(tr("SSTV mode"), cur)) {
                for (int i = 0; i < nModes; i++) if (ImGui::Selectable(sstvModeInfo(i).name, s.variant == i)) s.variant = i;
                ImGui::EndCombo();
            }
        }
        fileRow(s, tr("Image (PPM)"), tr("Browse…##sstv"));
        ImGui::TextDisabled("%s", tr("A P6 PPM is sent as the picture; any other or empty file uses the test picture. The callsign is stamped on it."));
        break;
    case TxPayloadKind::Freedv: {
        const std::string note = txVocoderNote(mode);
        if (!note.empty()) ImGui::TextWrapped("%s", tr(note.c_str()));
        audioSourceRow(s, running);
        break;
    }
    case TxPayloadKind::Pager:
        ImGui::InputScalar(tr("Capcode"), ImGuiDataType_U32, &s.address);
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        {
            const char* sp[] = {"512 baud", "1200 baud", "2400 baud", "FLEX"};
            int si = std::max(0, std::min(3, s.baudIdx));
            ImGui::SetNextItemWidth(140 * gUi);
            if (ImGui::BeginCombo(tr("Speed"), sp[si])) {
                for (int i = 0; i < 4; i++) if (ImGui::Selectable(sp[i], i == si)) s.baudIdx = i;
                ImGui::EndCombo();
            }
        }
        ImGui::InputInt(tr("Function bits"), &s.function);
        break;
    case TxPayloadKind::Packet:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::InputText(tr("Destination"), s.dest, sizeof s.dest);
        ImGui::InputText(tr("Path"), s.path, sizeof s.path);
        if (ImGui::InputText(tr("Comment"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::InputDouble(tr("Latitude"), &s.lat, 0, 0, "%.5f");
        ImGui::InputDouble(tr("Longitude"), &s.lon, 0, 0, "%.5f");
        {
            const char* br[] = {"1200 baud", "9600 baud"};
            if (ImGui::BeginCombo(tr("Baud"), br[s.baudIdx == 1 ? 1 : 0])) {
                if (ImGui::Selectable(br[0], s.baudIdx == 0)) s.baudIdx = 0;
                if (ImGui::Selectable(br[1], s.baudIdx == 1)) s.baudIdx = 1;
                ImGui::EndCombo();
            }
        }
        break;
    case TxPayloadKind::Ais:
        ImGui::InputScalar(tr("MMSI"), ImGuiDataType_U32, &s.address);
        ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign);
        ImGui::InputText(tr("Vessel name"), s.name, sizeof s.name);
        ImGui::InputDouble(tr("Latitude"), &s.lat, 0, 0, "%.5f");
        ImGui::InputDouble(tr("Longitude"), &s.lon, 0, 0, "%.5f");
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted(tr("Warning: AIS simulates vessels. Send it only into a dummy load or a shielded setup, never to an antenna: false ships can confuse real receivers, which is a safety risk and illegal in most places."));
        ImGui::PopStyleColor();
        break;
    case TxPayloadKind::AdsB:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::InputScalar(tr("ICAO address"), ImGuiDataType_U32, &s.address, nullptr, nullptr, "%06X", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted(tr("Warning: ADS-B simulates aircraft. Send it only into a dummy load or a shielded setup, never to an antenna."));
        ImGui::PopStyleColor();
        break;
    case TxPayloadKind::VoiceFm:
        ImGui::InputText(tr("Station name (PS)"), s.name, sizeof s.name);
        ImGui::InputText(tr("Radio text (RT)"), s.text, sizeof s.text);
        ImGui::InputInt(tr("RDS PI"), &s.rdsPi, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
        audioSourceRow(s, running);
        break;
    case TxPayloadKind::VoiceAir:
        s.kind = 1;
        audioSourceRow(s, running);
        break;
    case TxPayloadKind::VoiceDigital: {
        const std::string note = txVocoderNote(mode);
        if (!note.empty()) ImGui::TextWrapped("%s", tr(note.c_str()));
        ImGui::InputText(tr("Callsign / my ID"), s.callsign, sizeof s.callsign);
        ImGui::InputText(tr("Destination ID"), s.dest, sizeof s.dest);
        if (id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "tetra") {
            ImGui::InputScalar(tr("Radio ID"), ImGuiDataType_U32, &s.address);
            ImGui::InputInt(tr("Talkgroup"), &s.talkgroup);
            ImGui::InputInt(tr("Colour code"), &s.colorCode);
            if (id == "dmr") ImGui::InputInt(tr("Timeslot"), &s.slot);
        }
        if (!txCanEncodeVoice(mode)) ImGui::Checkbox(tr("Send analog NFM of the audio instead of the digital test pattern"), &s.analogVoice);
        audioSourceRow(s, running);
        break;
    }
    case TxPayloadKind::Qo100: {
        const char* paths[] = {tr("SSB (microphone)"), tr("DATV (DVB-S/S2 file)")};
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("QO-100 path"), paths[s.variant == 1 ? 1 : 0])) {
            if (ImGui::Selectable(paths[0], s.variant == 0)) s.variant = 0;
            if (ImGui::Selectable(paths[1], s.variant == 1)) s.variant = 1;
            ImGui::EndCombo();
        }
        if (s.variant == 1) tvFilePanel(s, mode, running);
        else audioSourceRow(s, running);
        break;
    }
    case TxPayloadKind::Marine:
        if (std::string(s.subId) == "wefax") {
            ImGui::TextDisabled("%s", tr("test weather-fax chart (custom image not wired)"));
            analogNfmBox(s, running);
        } else {
            if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
            ImGui::InputScalar(tr("MMSI"), ImGuiDataType_U32, &s.address);
            ImGui::TextDisabled("%s", tr("NAVTEX sends the text (ZCZC / NNNN are added when missing). DSC (when that service is selected) uses the MMSI."));
        }
        break;
    case TxPayloadKind::AcarsFamily:
        ImGui::InputText(tr("Aircraft registration"), s.callsign, sizeof s.callsign);
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::TextDisabled("%s", tr("The registration and free-text go on the air as an H1 / ACARS downlink. Empty: the demo traffic."));
        break;
    case TxPayloadKind::Sonde:
        if (ImGui::InputText(tr("Serial / ID"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::InputDouble(tr("Latitude"), &s.lat, 0, 0, "%.5f");
        ImGui::InputDouble(tr("Longitude"), &s.lon, 0, 0, "%.5f");
        ImGui::TextDisabled("%s", tr("An RS41 with this serial. Empty: the demo sondes."));
        break;
    case TxPayloadKind::Same:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::TextDisabled("%s", tr("A SAME header. ZCZC- is added when missing."));
        break;
    case TxPayloadKind::Ale:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::InputText(tr("Destination / ID"), s.dest, sizeof s.dest);
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        break;
    case TxPayloadKind::VorIls:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        ImGui::TextDisabled("%s", tr("Morse identification, up to four letters."));
        break;
    case TxPayloadKind::LoraRaw:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        break;
    case TxPayloadKind::Dect:
        ImGui::InputScalar(tr("RFPI"), ImGuiDataType_U32, &s.address, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::TextDisabled("%s", tr("RFPI of the base station."));
        break;
    case TxPayloadKind::CubeSat:
        if (ImGui::InputText(tr("Callsign"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
        if (ImGui::InputText(tr("Payload text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        {
            const char* br[] = {"1200 baud AFSK", "9600 baud G3RUH"};
            if (ImGui::BeginCombo(tr("Baud"), br[s.baudIdx == 1 ? 1 : 0])) {
                if (ImGui::Selectable(br[0], s.baudIdx == 0)) s.baudIdx = 0;
                if (ImGui::Selectable(br[1], s.baudIdx == 1)) s.baudIdx = 1;
                ImGui::EndCombo();
            }
        }
        ImGui::TextDisabled("%s", tr("AX.25 UI frame. Empty: the demo CubeSat cycle."));
        break;
    case TxPayloadKind::WeatherScene:
        fileRow(s, tr("Image (PNG / PPM)"), tr("Browse…##wx"));
        ImGui::TextDisabled("%s", tr("test LRPT/HRIT scene (custom image not wired)"));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::SatScene:
        ImGui::TextDisabled("%s", tr("test downlink scene (this generator does not take a custom message)"));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Flarm:
        ImGui::InputScalar(tr("Device ID"), ImGuiDataType_U32, &s.address, nullptr, nullptr, "%06X", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::TextDisabled("%s", tr("Empty: the demo FLARM scene. This ID is not packed into the unpublished newer protocol."));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Zigbee:
        ImGui::InputScalar(tr("PAN / short address"), ImGuiDataType_U32, &s.address);
        ImGui::TextDisabled("%s", tr("Empty: the demo 802.15.4 scene."));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Dji:
        ImGui::TextDisabled("%s", tr("test DJI DroneID scene (custom serial is not packed)"));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::HfBeacon:
        ImGui::TextDisabled("%s", tr("test NCDXF-style beacon scene"));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Vdes:
        ImGui::InputScalar(tr("MMSI"), ImGuiDataType_U32, &s.address);
        ImGui::TextDisabled("%s", tr("Empty: the demo VDES scene."));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Uplink:
        ImGui::InputScalar(tr("ICAO address"), ImGuiDataType_U32, &s.address, nullptr, nullptr, "%06X", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::TextDisabled("%s", tr("Empty: the demo Mode S uplink scene."));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Mesh:
        if (ImGui::InputText(tr("Text"), s.text, sizeof s.text)) saveStr("tx.text", s.text);
        ImGui::TextDisabled("%s", tr("Empty: the demo LoRa / mesh scene."));
        analogNfmBox(s, running);
        break;
    case TxPayloadKind::Kaal:
        kaalPanel(s, running);
        break;
    case TxPayloadKind::Kait:
        kaitPanel(s, running);
        break;
    case TxPayloadKind::Kavb:
        kavbPanel(s, running);
        break;
    case TxPayloadKind::Kapp:
        kappPanel(s, running);
        break;
    case TxPayloadKind::Kaft:
        kaftPanel(s, running);
        break;
    case TxPayloadKind::Kaws:
        kawsPanel(s, running);
        break;
    case TxPayloadKind::Mac:
        ImGui::TextDisabled("%s", tr("D2-MAC test scene (custom video is not wired into this analogue generator)."));
        analogNfmBox(s, running);
        break;
    }
}

} // namespace txui
