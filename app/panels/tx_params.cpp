// The advanced parameters of the transmit panel: ONE declarative table per kind of signal (paramsFor), drawn by one function (advancedPanel) in the sections
// Modulation, Audio, Filters, Output and Timing. A parameter is one row of the table: its key, label, tooltip, unit, range and default. Adding a mode's
// parameters means adding rows (or choosing an existing group of rows) in paramsFor(); the panel, the saving of the last values, the presets, "Reset to default"
// and the hand-over to the generator (TxRequest::opt / the legacy request fields) need no other change. A value only reaches a generator that reads its key.
// Nothing here limits a value to what is lawful or safe: a slider's range is what the generator can make.
#include "tx_state.h"
#include "dect2/audio/audio_control.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace txui;

namespace {

using P = PDef;
constexpr double kNoWhen = 0;

// one row; `when` is "key=value" or "key>value" over another parameter of the same table (the row is shown only then)
P F(const char* key, Sec sec, const char* label, const char* tip, double def, double lo, double hi, const char* fmt, const char* when = nullptr, Bind b = Bind::Opt) {
    P p; p.key = key; p.sec = sec; p.type = PType::Float; p.label = label; p.tip = tip; p.def = def; p.lo = lo; p.hi = hi; p.fmt = fmt; p.when = when; p.bind = b;
    return p;
}
P I(const char* key, Sec sec, const char* label, const char* tip, double def, double lo, double hi, const char* when = nullptr) {
    P p = F(key, sec, label, tip, def, lo, hi, "%.0f", when);
    p.type = PType::Int;
    return p;
}
P B(const char* key, Sec sec, const char* label, const char* tip, bool def, const char* when = nullptr) {
    P p = F(key, sec, label, tip, def ? 1 : 0, 0, 1, "", when);
    p.type = PType::Bool;
    return p;
}
P C(const char* key, Sec sec, const char* label, const char* tip, double def, std::vector<const char*> choices, double base = 0, Bind b = Bind::Opt, const char* when = nullptr) {
    P p = F(key, sec, label, tip, def, 0, (double)choices.size() - 1, "", when, b);
    p.type = PType::Choice; p.choices = std::move(choices); p.base = base;
    return p;
}

// Off, then the 50 standard CTCSS tones (kCtcssHz); the choice index n > 0 is kCtcssHz[n - 1] (read in tx_voice.cpp as fm.ctcssIdx)
std::vector<const char*> ctcssChoices() {
    static std::vector<std::string> names;
    static std::vector<const char*> ptrs;
    if (ptrs.empty()) {
        names.reserve(51);
        names.push_back("Off");
        for (float f : dect2::actl::kCtcssHz) { char b[24]; snprintf(b, sizeof b, "%.1f Hz", f); names.push_back(b); }
        for (const auto& n : names) ptrs.push_back(n.c_str());
    }
    return ptrs;
}

void add(std::vector<P>& v, std::initializer_list<P> l) { v.insert(v.end(), l.begin(), l.end()); }

// the audio chain every analog voice shares: the test tone, the filters, the speech compressor and the VOX
void voiceAudio(std::vector<P>& v, double hp, double lp) {
    add(v, {
        F("audio.tone", Sec::Audio, N_("Test tone"), N_("The pitch of the test tone, used when the audio input is the test tone."), 1000, 100, 5000, "%.0f Hz", nullptr, Bind::ToneHz),
        F("audio.toneLevelPct", Sec::Audio, N_("Test tone level"), N_("How loud the test tone is, as a share of full audio."), 50, 0, 100, "%.0f %%"),
        F("audio.tone2Hz", Sec::Audio, N_("Second test tone"), N_("A second tone added to the first (0 = off). Two tones make a two-tone linearity test."), 0, 0, 5000, "%.0f Hz"),
        B("audio.compOn", Sec::Audio, N_("Speech compressor"), N_("Evens out loud and quiet speech so the signal stays strong without over-modulating."), false),
        F("audio.compThreshDb", Sec::Audio, N_("Threshold"), N_("Audio above this level is turned down."), -18, -40, 0, "%.0f dB", "audio.compOn=1"),
        F("audio.compRatio", Sec::Audio, N_("Ratio"), N_("How strongly audio above the threshold is turned down; 20 is close to a hard limiter."), 4, 1, 20, "%.1f : 1", "audio.compOn=1"),
        F("audio.compMakeupDb", Sec::Audio, N_("Make-up gain"), N_("Gain added after the compressor."), 0, 0, 20, "%.0f dB", "audio.compOn=1"),
        F("audio.compReleaseMs", Sec::Audio, N_("Release"), N_("How fast the compressor lets go after a loud passage."), 200, 20, 2000, "%.0f ms", "audio.compOn=1"),
        F("audio.hpHz", Sec::Filters, N_("High-pass (low cut)"), N_("Removes rumble and hum below this pitch (0 = off)."), hp, 0, 1000, "%.0f Hz"),
        F("audio.lpHz", Sec::Filters, N_("Low-pass (audio bandwidth)"), N_("Cuts the audio above this pitch, which sets how wide the signal is (0 = off)."), lp, 0, 12000, "%.0f Hz"),
        B("vox.on", Sec::Timing, N_("VOX (transmit on voice)"), N_("The signal goes out only while there is sound on the audio input."), false),
        F("vox.threshDb", Sec::Timing, N_("VOX threshold"), N_("How loud the audio must be to open the VOX."), -35, -60, 0, "%.0f dB", "vox.on=1"),
        F("vox.hangMs", Sec::Timing, N_("VOX hang time"), N_("How long the VOX stays open after the sound stops."), 500, 50, 3000, "%.0f ms", "vox.on=1"),
    });
}

void outputCommon(std::vector<P>& v) {
    add(v, {
        F("out.trimDb", Sec::Output, N_("Output trim"), N_("Gain applied to the whole signal just before the radio. Lower it when the meter shows clipping; the radio's own gain is set above."), 0, -40, 6, "%.1f dB"),
        B("out.swapIq", Sec::Output, N_("Mirror the spectrum"), N_("Swaps I and Q so the signal is turned over: an upper sideband becomes a lower one. For a radio or mixer that inverts."), false),
        F("out.rampMs", Sec::Timing, N_("Key-up fade-in"), N_("The signal fades in over this time after Start, so the key-up does not click."), kTxDefaultRampMs, 0, 500, "%.0f ms"),   // also the fade at Stop and at the end of one pass
    });
}

// the table of one signal: kind of signal, and the few choices that change it (an analog fall-back, the QO-100 path)
std::vector<P> build(const TxModeSpec& m, bool analog, int variant) {
    std::vector<P> v;
    const std::string id = m.id ? m.id : "";
    const TxPayloadKind k = txPayloadKind(id);
    const char* const sideband[] = {N_("Upper sideband (USB)"), N_("Lower sideband (LSB)")};
    switch (k) {
    case TxPayloadKind::VoiceAir:
        add(v, {
            C("am.type", Sec::Modulation, N_("Type"), N_("AM sends the carrier and both sidebands. Double sideband with suppressed carrier sends only the sidebands."), 0, {N_("AM (carrier and sidebands)"), N_("DSB (suppressed carrier)")}),
            F("am.depthPct", Sec::Modulation, N_("Modulation depth"), N_("How far the audio swings the signal. Full modulation is at 100 percent; more than that over-modulates and distorts."), 85, 0, 120, "%.0f %%"),
            F("am.carrierLevel", Sec::Output, N_("Carrier level"), N_("The size of the carrier as a share of full scale. At a depth of 85 percent the peaks reach about full scale."), 0.55, 0.05, 1.0, "%.2f"),
        });
        voiceAudio(v, 0, 4500);
        break;
    case TxPayloadKind::Qo100:
        if (variant == 1) break;   // the DATV path has the TV settings
        add(v, {
            C("ssb.sideband", Sec::Modulation, N_("Sideband"), N_("Which sideband is sent."), 0, {sideband[0], sideband[1]}, 2, Bind::Kind),
            F("ssb.carrierPct", Sec::Modulation, N_("Carrier injection"), N_("0 is the normal suppressed carrier. A little carrier helps a receiver lock on; the top of the range is as strong as the loudest audio."), 0, 0, 100, "%.0f %%"),
            F("ssb.level", Sec::Output, N_("Peak level"), N_("The largest signal as a share of full scale."), 0.4, 0.05, 1.0, "%.2f"),
        });
        voiceAudio(v, 300, 2700);
        break;
    case TxPayloadKind::VoiceFm:
        add(v, {
            F("fm.deviation", Sec::Modulation, N_("Deviation"), N_("How far the audio swings the frequency. Broadcast FM uses 75 kHz."), 75000, 5000, 100000, "%.0f Hz", nullptr, Bind::Deviation),
            F("fm.preemph", Sec::Modulation, N_("Pre-emphasis"), N_("Boosts the high notes before sending. 50 µs in Europe and most of the world, 75 µs in the Americas."), 50, 25, 100, "%.0f µs", nullptr, Bind::Preemph),
            B("bfm.stereo", Sec::Modulation, N_("Stereo"), N_("Sends the 19 kHz pilot and the stereo difference signal."), true),
            F("bfm.pilotPct", Sec::Modulation, N_("Pilot level"), N_("The 19 kHz stereo pilot, as a share of the full deviation."), 9, 0, 20, "%.1f %%", "bfm.stereo=1"),
            B("bfm.rds", Sec::Modulation, N_("RDS"), N_("Sends the station name, radio text and programme type on a 57 kHz subcarrier."), true),
            F("bfm.rdsDevKhz", Sec::Modulation, N_("RDS level"), N_("How far the RDS subcarrier deviates the carrier."), 2, 0.2, 7.5, "%.1f kHz", "bfm.rds=1"),
            I("bfm.pty", Sec::Modulation, N_("Programme type"), N_("The RDS programme type code (0 to 31; 10 is pop music)."), 10, 0, 31, "bfm.rds=1"),
            B("bfm.tp", Sec::Modulation, N_("Traffic programme"), N_("The RDS flag for a station that carries traffic announcements."), true, "bfm.rds=1"),
            B("bfm.ta", Sec::Modulation, N_("Traffic announcement now"), N_("The RDS flag that a traffic announcement is on the air."), false, "bfm.rds=1"),
            B("bfm.music", Sec::Modulation, N_("Music (not speech)"), N_("The RDS flag for music instead of speech."), true, "bfm.rds=1"),
            F("audio.tone", Sec::Audio, N_("Left test tone"), N_("The pitch on the left channel, used when the audio input is the test tone."), 1000, 100, 15000, "%.0f Hz", nullptr, Bind::ToneHz),
            F("bfm.rightHz", Sec::Audio, N_("Right test tone"), N_("The pitch on the right channel (0 = three times the left)."), 0, 0, 15000, "%.0f Hz"),
            F("bfm.leftAmp", Sec::Audio, N_("Left level"), N_("The left test tone, as a share of full audio."), 0.5, 0, 1, "%.2f"),
            F("bfm.rightAmp", Sec::Audio, N_("Right level"), N_("The right test tone, as a share of full audio."), 0.5, 0, 1, "%.2f"),
            F("bfm.level", Sec::Output, N_("RF level"), N_("The size of the signal as a share of full scale."), 0.3, 0.05, 1.0, "%.2f"),
        });
        break;
    case TxPayloadKind::VoiceDigital:
    case TxPayloadKind::WeatherScene: case TxPayloadKind::SatScene: case TxPayloadKind::Flarm: case TxPayloadKind::Zigbee: case TxPayloadKind::Dji:
    case TxPayloadKind::HfBeacon: case TxPayloadKind::Vdes: case TxPayloadKind::Uplink: case TxPayloadKind::Mesh: case TxPayloadKind::Mac:
        if (!analog) break;   // the analog voice settings apply when the analog fall-back is on
        add(v, {
            F("fm.deviation", Sec::Modulation, N_("Deviation"), N_("How far the audio swings the frequency. 2.5 kHz is a 12.5 kHz channel, 5 kHz a 25 kHz channel."), 5000, 500, 25000, "%.0f Hz", nullptr, Bind::Deviation),
            F("fm.preemphUs", Sec::Modulation, N_("Pre-emphasis"), N_("Boosts the high notes before sending (0 = off)."), 50, 0, 150, "%.0f µs"),
            C("fm.ctcssIdx", Sec::Modulation, N_("CTCSS tone"), N_("A sub-audible tone that opens a receiver's squelch. Pick one of the standard tones (67.0 to 254.1 Hz); Off sends none. Its level is the usual 12 percent of the deviation."), 0, ctcssChoices()),
            F("fm.level", Sec::Output, N_("RF level"), N_("The size of the signal as a share of full scale."), 0.4, 0.05, 1.0, "%.2f"),
        });
        voiceAudio(v, 300, 3000);
        break;
    case TxPayloadKind::Ftx:
        add(v, {
            F("audio.tone", Sec::Modulation, N_("Audio frequency"), N_("Where the signal sits in the audio passband, as an offset above the dial frequency."), 1500, 200, 3000, "%.0f Hz", nullptr, Bind::ToneHz),
            C("hf.sideband", Sec::Modulation, N_("Sideband"), N_("Which sideband is sent."), 0, {sideband[0], sideband[1]}, 0, Bind::Variant),
        });
        break;
    case TxPayloadKind::Keyboard:
        add(v, {
            F("audio.tone", Sec::Modulation, N_("Centre frequency"), N_("Where the middle of the signal sits in the audio passband, as an offset above the dial frequency."), 1500, 200, 3000, "%.0f Hz", nullptr, Bind::ToneHz),
            C("hf.sideband", Sec::Modulation, N_("Sideband"), N_("Which sideband is sent."), 0, {sideband[0], sideband[1]}, 0, Bind::Variant),
        });
        break;
    case TxPayloadKind::Cw:
        add(v, {F("audio.tone", Sec::Modulation, N_("Pitch"), N_("The pitch of the Morse tone, as an offset above the dial frequency."), 700, 200, 2000, "%.0f Hz", nullptr, Bind::ToneHz)});
        break;
    case TxPayloadKind::Psk:
        add(v, {F("audio.tone", Sec::Modulation, N_("Audio frequency"), N_("Where the signal sits in the audio passband, as an offset above the dial frequency."), 1000, 200, 3000, "%.0f Hz", nullptr, Bind::ToneHz)});
        break;
    default:
        break;
    }
    outputCommon(v);
    return v;
}

// "key=value" / "key>value": is the row shown?
bool shown(const TxUi& s, const P& d) {
    if (!d.when) return true;
    const char* w = d.when;
    const char* op = strpbrk(w, "=>");
    if (!op) return true;
    const std::string key(w, (size_t)(op - w));
    const double want = atof(op + 1);
    const auto it = s.pv.find(key);
    const double have = it == s.pv.end() ? 0 : it->second;
    return *op == '=' ? std::fabs(have - want) < 1e-9 : have > want;
}

std::string sectionName(Sec sec) {
    switch (sec) {
    case Sec::Modulation: return tr("Modulation");
    case Sec::Audio: return tr("Audio");
    case Sec::Filters: return tr("Filters");
    case Sec::Output: return tr("Output");
    case Sec::Timing: return tr("Timing");
    }
    return "";
}

// "k=v,k=v" of the values, and back
std::string pack(const TxUi& s) {
    std::string o;
    char b[96];
    for (const auto& kv : s.pv) { snprintf(b, sizeof b, "%s=%.6g", kv.first.c_str(), kv.second); if (!o.empty()) o += ','; o += b; }
    return o;
}
void unpack(TxUi& s, const std::string& txt) {
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find(',', pos);
        if (e == std::string::npos) e = txt.size();
        const std::string item = txt.substr(pos, e - pos);
        pos = e + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq);
        if (key[0] == '@') continue;
        if (s.pv.count(key)) s.pv[key] = atof(item.c_str() + eq + 1);
    }
}

std::string lastKey(const TxUi& s) { return "tx.pv." + s.modeId; }
std::string presetKey(const TxUi& s) { return "tx.presets." + s.modeId; }

struct Preset { std::string name, data; };
std::vector<Preset> loadPresets(const TxUi& s) {
    std::vector<Preset> v;
    const std::string txt = plat::basePrefs().getS(presetKey(s).c_str(), "");
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find('|', pos);
        if (e == std::string::npos) e = txt.size();
        const std::string item = txt.substr(pos, e - pos);
        pos = e + 1;
        const size_t t = item.find('~');
        if (t == std::string::npos || t == 0) continue;
        v.push_back({item.substr(0, t), item.substr(t + 1)});
    }
    return v;
}
void savePresets(const TxUi& s, const std::vector<Preset>& v) {
    std::string o;
    for (const auto& p : v) { if (!o.empty()) o += '|'; o += p.name + "~" + p.data; }
    plat::basePrefs().setS(presetKey(s).c_str(), o);
}

void defaults(TxUi& s) {
    for (const auto& d : s.params) s.pv[d.key] = d.def;
}

} // namespace

namespace txui {

const std::vector<PDef>& paramsFor(TxUi& s, const TxModeSpec& m) {
    const std::string key = std::string(m.id) + (s.analogVoice ? "+a" : "") + (s.variant == 1 ? "+v" : "");
    if (key != s.paramsKey) {
        s.paramsKey = key;
        s.params = build(m, s.analogVoice, s.variant);
        for (const auto& d : s.params) if (!s.pv.count(d.key)) s.pv[d.key] = d.def;
    }
    return s.params;
}

// the mode was chosen: its parameters start from the defaults and then from the values it had last time
void paramsEnter(TxUi& s, const TxModeSpec& m) {
    s.pv.clear();
    s.paramsKey.clear();
    paramsFor(s, m);
    defaults(s);
    unpack(s, plat::basePrefs().getS(lastKey(s).c_str(), ""));
    s.pvMode = s.modeId;
}

// the values into the request: the generator's own keys, and the request fields the older parameters live in
void paramsApply(const TxUi& s, TxRequest& r) {
    for (const auto& d : s.params) {
        const auto it = s.pv.find(d.key);
        if (it == s.pv.end()) continue;
        const double v = d.type == PType::Choice ? d.base + std::round(it->second) : it->second;
        switch (d.bind) {
        case Bind::Opt: r.opt[d.key] = v; break;
        case Bind::ToneHz: r.toneHz = v; break;
        case Bind::Deviation: r.deviationHz = v; break;
        case Bind::Preemph: r.preemphUs = v; break;
        case Bind::Kind: r.kind = (int)v; break;
        case Bind::Variant: r.variant = (int)v; break;
        }
    }
    for (const auto& d : s.params) if (d.bind == Bind::Opt && !r.opt.count(d.key)) r.opt[d.key] = d.def;
}

namespace {
// one row: the label on the left (tooltip: what it does and the default), the control on the right; true when the value changed
bool paramRow(TxUi& s, const PDef& d, bool running) {
    double& v = s.pv[d.key];
    const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::AlignTextToFramePadding();
    const bool changed0 = std::fabs(v - d.def) > 1e-9;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(changed0 ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(tr(d.label));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        char def[64];
        if (d.type == PType::Choice) snprintf(def, sizeof def, "%s", tr(d.choices[(size_t)std::max(0.0, std::min(d.hi, d.def))]));
        else if (d.type == PType::Bool) snprintf(def, sizeof def, "%s", d.def > 0.5 ? tr("on") : tr("off"));
        else snprintf(def, sizeof def, d.fmt, d.def);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320 * gUi);
        ImGui::TextUnformatted(tr(d.tip));
        ImGui::TextDisabled(tr("Default: %s"), def);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(labW);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(d.key);
    bool ch = false;
    (void)running;
    if (d.type == PType::Float) {
        float f = (float)v;
        if (ImGui::SliderFloat("##v", &f, (float)d.lo, (float)d.hi, d.fmt, ImGuiSliderFlags_AlwaysClamp)) { v = f; ch = true; }
    } else if (d.type == PType::Int) {
        int n = (int)std::lround(v);
        if (ImGui::SliderInt("##v", &n, (int)d.lo, (int)d.hi)) { v = n; ch = true; }
    } else if (d.type == PType::Bool) {
        bool b = v > 0.5;
        if (ImGui::Checkbox("##v", &b)) { v = b ? 1 : 0; ch = true; }
    } else {
        const int cur = std::max(0, std::min((int)d.hi, (int)std::lround(v)));
        if (ImGui::BeginCombo("##v", tr(d.choices[(size_t)cur]))) {
            for (int i = 0; i <= (int)d.hi; i++) if (ImGui::Selectable(tr(d.choices[(size_t)i]), i == cur)) { v = i; ch = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::PopID();
    return ch;
}
} // namespace

// The Advanced block of the panel: presets and reset on top, then the sections Modulation, Audio, Filters, Output, Timing
void advancedPanel(TxUi& s, const TxModeSpec& mode, bool running) {
    if (s.pvMode != s.modeId) paramsEnter(s, mode);
    paramsFor(s, mode);
    bool changed = false;

    // presets of this signal (the frequency and level are part of a preset)
    {
        std::vector<Preset> pr = loadPresets(s);
        ImGui::SetNextItemWidth(std::max(140.f * gUi, ImGui::GetContentRegionAvail().x - 240 * gUi));
        if (ImGui::BeginCombo("##txpreset", s.presetName[0] ? s.presetName : tr("Presets"))) {
            for (const auto& p : pr) {
                if (ImGui::Selectable(p.name.c_str(), p.name == s.presetName)) {
                    defaults(s);
                    unpack(s, p.data);
                    const size_t a = p.data.find("@mhz="), g = p.data.find("@gain=");
                    if (a != std::string::npos) s.freqMhz = atof(p.data.c_str() + a + 5);
                    if (g != std::string::npos) { s.gainDb = (float)atof(p.data.c_str() + g + 6); s.gainSet = true; }
                    snprintf(s.presetName, sizeof s.presetName, "%s", p.name.c_str());
                    changed = true;
                }
            }
            if (pr.empty()) ImGui::TextDisabled("%s", tr("No presets saved for this signal yet."));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset to default"))) { defaults(s); s.presetName[0] = 0; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Puts every setting below back to its default."));
        ImGui::SetNextItemWidth(std::max(120.f * gUi, ImGui::GetContentRegionAvail().x - 190 * gUi));
        ImGui::InputTextWithHint("##txpname", tr("Name for a new preset"), s.presetNew, sizeof s.presetNew);
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.presetNew[0]);
        if (ImGui::Button(tr("Save preset"))) {
            std::string nm = s.presetNew;
            for (char& c : nm) if (c == '|' || c == '~' || c == ',' || c == '=') c = '-';
            char extra[96];
            snprintf(extra, sizeof extra, ",@mhz=%.6f,@gain=%.2f", s.freqMhz, (double)s.gainDb);
            const std::string data = pack(s) + extra;
            bool found = false;
            for (auto& p : pr) if (p.name == nm) { p.data = data; found = true; }
            if (!found) pr.push_back({nm, data});
            savePresets(s, pr);
            snprintf(s.presetName, sizeof s.presetName, "%s", nm.c_str());
            s.presetNew[0] = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Saves the settings below, the frequency and the level under this name."));
        if (s.presetName[0]) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Delete"))) {
                pr.erase(std::remove_if(pr.begin(), pr.end(), [&](const Preset& p) { return p.name == s.presetName; }), pr.end());
                savePresets(s, pr);
                s.presetName[0] = 0;
            }
        }
    }

    ImGui::BeginDisabled(running);
    for (Sec sec : {Sec::Modulation, Sec::Audio, Sec::Filters, Sec::Output, Sec::Timing}) {
        bool any = sec == Sec::Output || sec == Sec::Timing;
        for (const auto& d : s.params) if (d.sec == sec) any = true;
        if (!any) continue;
        ImGui::Spacing();
        ImGui::TextDisabled("%s", sectionName(sec).c_str());
        ImGui::Separator();
        for (const auto& d : s.params) if (d.sec == sec && shown(s, d)) changed |= paramRow(s, d, running);
        if (sec == Sec::Output) {   // the offset belongs to the radio, not to the signal
            const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("Frequency offset"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("This is added to the frequency sent to the radio, for a radio that radiates off frequency."));
            ImGui::SameLine(labW);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##txoff", &s.offsetMhz, 0.0, 0.0, "%.6f MHz")) plat::basePrefs().setD("tx.offsetMhz", s.offsetMhz);
            const TxModeSpec* tm = txModeById(s.modeId);
            const double modeOff = tm ? txModeTuneOffsetHz(*tm) / 1e6 : 0;   // the radio is tuned this far above the dial, the signal sits that far below the radio's centre (#351)
            if (s.offsetMhz != 0 || modeOff != 0) ImGui::TextDisabled(tr("radio is told %.6f MHz"), s.freqMhz + s.offsetMhz + modeOff);
        }
        if (sec == Sec::Timing) {
            const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("Repeat until stopped"));
            ImGui::SameLine(labW);
            ImGui::Checkbox("##txloop", &s.loop);
            if (!s.loop) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr("seconds"));
                ImGui::SameLine(labW);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##txonce", &s.onceSeconds, 1, 3600);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("TX sample rate"));
            ImGui::SameLine(labW);
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::BeginCombo("##txfsmode", s.autoSampleRate ? tr("Auto") : tr("Manual"))) {
                if (ImGui::Selectable(tr("Auto"), s.autoSampleRate)) s.autoSampleRate = true;
                if (ImGui::Selectable(tr("Manual"), !s.autoSampleRate)) s.autoSampleRate = false;
                ImGui::EndCombo();
            }
            if (!s.autoSampleRate) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140 * gUi);
                ImGui::InputDouble("##txfs", &s.sampleRateMsps, 0, 0, "%.6f Msps");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Requested transmit IQ sample rate. For a shared RX/TX radio, set the same hardware rate on both sides. Stop transmitting before changing it."));
            }
        }
    }
    ImGui::EndDisabled();
    if (changed) plat::basePrefs().setS(lastKey(s).c_str(), pack(s));
}

} // namespace txui
