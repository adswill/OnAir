// The transmitter: the RX / TX switch at the top left of the interface and the Transmit panel that fills the main area in TX view. What can be sent is the
// table in core/include/dect2/tx.h; the session, the radios' sinks and the rules are in core/src/tx.cpp and the radios' sources.
//
// The transmitter is independent of the receivers: it has its own radio choice and its own state (not an App's), so a receiver goes on receiving while it
// transmits (the owner's example: a HackRF Pro transmits, an RTL-SDR receives and decodes it). A radio is never used by both unless it is genuinely full
// duplex: a radio a receiver holds is refused with a plain sentence (txClaimRadio, device_claims.h), never by silently stopping the receiver.
//
// Rules this file keeps: transmitting never starts by itself (only the big button), is never saved in the settings (only "the notice was read" is; the
// view itself opens as RX), and stops when it is stopped, the radio fails or the program ends. Nothing is limited here: frequency and gain go to the
// driver as typed, and the driver's own refusal is shown as it came.
#include "app.h"
#include "dect2/hfdig_sstv.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2;

namespace {
// The texts the core gives (refusal reasons, the radios' gain notes) come through tr() above; listed here so the translation files can be checked against the sources
[[maybe_unused]] const char* const kCoreTexts[] = {
    N_("GNSS is not transmitted: its generator is a satellite-signal simulator (spoofing)"),
    N_("TETRAPOL is not transmitted: receive only, control-channel system information"),
    N_("cellular signals are not transmitted: the generators model a network cell for testing the decoder"),
    N_("this is an analysis mode: there is nothing to transmit"),
    N_("Bluetooth is not transmitted: it would impersonate a Bluetooth advertiser"),
    N_("Wi-Fi is not transmitted: the generator models access-point beacons for the decoder"),
    N_("Drone Remote ID is not transmitted: it rides on Wi-Fi and Bluetooth advertisements"),
    N_("ANT+ is not transmitted: it would impersonate a sensor network"),
    N_("this is a receive-only sensor harvester: there is nothing meaningful to transmit"),
    N_("this is a spectrum analyser: there is nothing to transmit"),
    N_("this is a general listening mode: there is nothing specific to transmit"),
    N_("time signals are not transmitted: the generator imitates broadcast clocks (spoofing)"),
    N_("this is a signal workbench: it is for measurement, not a radio service"),
    N_("Multitone paging is not transmitted: the generator is not the real (unpublished) format, only a detector test"),
    N_("this mode has no transmit generator"),
    N_("TX VGA 0 to 47 dB; the +14 dB amplifier has its own switch"),
    N_("the AD9361's TX attenuation shown as a gain: -89.75 dB (least power) to 0 dB (most)"),
    N_("the driver's own TX gain scale (known after the radio has been opened once)"),
    N_("Live voice is encoded with Codec 2 (libcodec2)."),
    N_("libcodec2 is not loaded: live encode is off. Destination callsigns still go on the air; voice bits are the known test pattern. A 1 kHz tone, silence or a WAV/MP3 can still be sent as analog NFM."),
    N_("Live voice is encoded with Codec 2 (FreeDV)."),
    N_("libcodec2 is not loaded: live encode is off. A 1 kHz tone, silence or a WAV/MP3 can still be sent."),
    N_("This vocoder cannot encode: OnAir loads mbelib for decode only (AMBE/IMBE). Destination ID / talkgroup / colour code still go on the air; voice bits are the test pattern. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead."),
    N_("TETRA ACELP is not encoded here. Colour code and IDs are shown; the downlink test pattern is sent. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead."),
};
}

namespace {

struct TxUi {
    TxSession session;
    std::string modeId, radioKey;            // the transmitter's own mode and radio
    int family = -1;                         // mode-list family (same numbers as RX); -1: follow the receiver once
    char subId[32] = {};
    int dvbVariant = 1;                      // 1 DVB-T2, 2 DVB-T
    SynthConfig synth;                       // the mode's own signal options (the same fields as the receiver's built-in test signal)
    int bwIdx = 0;                           // DVB-T2 / DVB-T channel width: 8, 7, 6, 5 MHz
    double freqMhz = 0;                      // typed by the user, any value; pre-filled from the receiver when the mode is chosen
    double offsetMhz = 0;                    // added only to the frequency sent to the radio; the typed frequency stays freqMhz
    float gainDb = 0;
    bool gainSet = false, ampOn = false, loop = true;
    int txCh = 0;                            // 0 = TX1. Shown when the radio has, or may have, more than one output
    int onceSeconds = 10;
    bool view = false;                       // TX view (never saved)
    bool claimed = false, askNotice = false;
    std::string error, radioName, modeName, fileNote;
    double freqSent = 0;
    int tickFrame = -1;
    char callsign[16] = {}, grid[8] = {}, dest[24] = {}, name[16] = {}, path[64] = {}, file[512] = {}, text[256] = {};
    char rdsRt[128] = {}, audioDevice[64] = {};
    char provider[64] = {}, network[64] = {}, svc[4][48] = {}, svcFile[4][512] = {}, svcCam[4][96] = {}, videoDevice[96] = {};
    int nsvc = 1, serviceId = 1, videoSrc = 0, svcSrc[4] = {};
    int kind = 0, baudIdx = 0, shiftIdx = 0, variant = 0, wpm = 20, dbm = 37, function = 3;
    unsigned address = 0;
    double toneHz = 1000, lat = 0, lon = 0;
    bool reverse = false, analogVoice = false, audioMute = false, prefsLoaded = false;
    int audioSrc = 2;                        // 0 mic, 1 file, 2 tone, 3 silence
    float micGain = 1.f;
    double deviationHz = 0, preemphUs = 50;
    int rdsPi = 0x4A21, slot = 1, colorCode = 1, talkgroup = 0;
    bool carrierHeld = false, carrierOnly = false;
    Waterfall wf;
    bool wfInit = false;
    float specFrac = 0.45f;
    std::vector<double> specX, specY;
};

TxUi& ui() {
    static TxUi u;
    return u;
}

const double kTxBw[] = {8, 7, 6, 5};

// the engine family of a transmit mode (the inverse of engineStd), for the mode's own synthetic-signal controls
int familyOf(const TxModeSpec& m) {
    switch (m.stdMode) {
    case 0: return 0;
    case 3: return 1;
    case 4: return 2;
    case 5: return 3;
    case 6: return 4;
    case 7: return 5;
    default: return m.stdMode - 2;
    }
}

const TxModeSpec* rxMode(const App& a) { return txModeFor(engineStd(a), a.stdMode == 2 ? 2 : 1); }

void ensureTxMode(App& a, TxUi& s) {
    if (s.family >= 0) return;
    s.family = a.family;
    s.dvbVariant = a.stdMode == 2 ? 2 : 1;
    s.subId[0] = 0;
    int n = 0;
    const SubMode* t = subModes(n);
    if (a.sub >= 1 && a.sub <= n) snprintf(s.subId, sizeof s.subId, "%s", t[a.sub - 1].id);
    const TxModeSpec* m = txModeForList(s.family, s.subId[0] ? s.subId : nullptr, s.dvbVariant);
    if (m) { s.modeId = m->id; s.synth = (rxMode(a) == m) ? a.tune.synth : SynthConfig(); s.freqMhz = a.freqMhz; }
}

// the mode chosen for transmitting: its options and frequency start from the receiver's when it is the receiver's own mode
void chooseMode(App& a, TxUi& s, const TxModeSpec& m) {
    s.modeId = m.id;
    s.bwIdx = 0;
    const TxModeSpec* rm = rxMode(a);
    if (rm && rm == &m) { s.synth = a.tune.synth; s.freqMhz = a.freqMhz; }
    else {
        s.synth = SynthConfig();
        const ModeTuning* mt = modeTuning(m.stdMode);
        s.freqMhz = mt && mt->defMhz > 0 ? mt->defMhz : a.freqMhz;
    }
}

const ImVec4 kRed(0.90f, 0.12f, 0.12f, 1.f);

std::vector<int> txRadios(const App& a) {
    std::vector<int> v;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[(size_t)i].isRadio() && a.devices[(size_t)i].canTransmit) v.push_back(i);
    return v;
}

// the receiver holding a radio the transmitter wants (0: none, or the radio can do both)
int rxHolder(const DeviceInfo& d) {
    const int h = deviceHolder(d);
    return h != 0 && h != kTxClaimOwner && !d.txFullDuplex ? h : 0;
}

void release(TxUi& s) {
    if (s.claimed) { txReleaseRadio(); s.claimed = false; }
}

void loadPayloadPrefs(TxUi& s) {
    if (s.prefsLoaded) return;
    s.prefsLoaded = true;
    plat::Prefs& p = plat::basePrefs();
    snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.callsign", "").c_str());
    snprintf(s.grid, sizeof s.grid, "%s", p.getS("tx.grid", "").c_str());
    snprintf(s.text, sizeof s.text, "%s", p.getS("tx.text", "").c_str());
    s.offsetMhz = p.getD("tx.offsetMhz", 0);
}

void saveStr(const char* key, const char* v) {
    plat::basePrefs().setS(key, v);
}

TxRequest makeReq(const TxUi& s, const TxModeSpec& mode, bool carrier) {
    TxRequest r;
    r.modeId = mode.id;
    r.synth = s.synth;
    r.bandwidthMhz = kTxBw[s.bwIdx];
    r.loop = s.loop;
    r.onceSeconds = s.onceSeconds;
    r.tune.centerHz = s.freqMhz * 1e6;
    r.tune.offsetHz = s.offsetMhz * 1e6;
    r.tune.gainDb = s.gainDb;
    r.tune.ampOn = s.ampOn;
    r.tune.channel = s.txCh < 0 ? 0 : s.txCh;
    r.callsign = s.callsign;
    r.grid = s.grid;
    r.text = s.text[0] ? s.text : s.rdsRt;
    r.dest = s.dest;
    r.path = s.path;
    r.file = s.file;
    r.name = s.name;
    r.provider = s.provider;
    r.network = s.network;
    r.serviceId = s.serviceId > 0 ? s.serviceId : 1;
    for (int i = 0; i < std::max(1, std::min(4, s.nsvc)); i++) if (s.svc[i][0]) r.services.push_back(s.svc[i]);
    if (r.services.empty() && s.name[0]) r.services.push_back(s.name);
    r.kind = s.kind;
    r.baudIdx = s.baudIdx;
    r.shiftIdx = s.shiftIdx;
    r.variant = s.variant;
    r.wpm = s.wpm;
    r.dbm = s.dbm;
    r.function = s.function;
    r.address = s.address;
    r.toneHz = s.toneHz;
    r.lat = s.lat;
    r.lon = s.lon;
    r.reverse = s.reverse;
    r.carrierTune = carrier;
    r.audioSrc = s.audioSrc;
    r.audioDevice = s.audioDevice;
    r.audioMute = s.audioMute;
    r.micGain = s.micGain;
    r.deviationHz = s.deviationHz;
    r.preemphUs = s.preemphUs;
    r.rdsPi = s.rdsPi;
    r.slot = s.slot;
    r.colorCode = s.colorCode;
    r.talkgroup = s.talkgroup;
    r.analogVoice = s.analogVoice;
    r.videoSrc = s.videoSrc;
    r.videoDevice = s.videoDevice;
    for (int i = 0; i < std::max(1, std::min(4, s.nsvc)); i++) {
        DemoTsService p;
        p.name = s.svc[i][0] ? s.svc[i] : (s.name[0] && i == 0 ? s.name : std::string("OnAir ") + std::to_string(i + 1));
        p.picture = i % 3;
        if (s.svcSrc[i] == 1) p.file = s.svcFile[i];
        if (s.svcSrc[i] == 2) { p.liveCamera = true; p.camera = s.svcCam[i]; }
        r.mux.push_back(std::move(p));
    }
    return r;
}

bool isVoiceId(const std::string& id) {
    return id == "fm" || id == "airband" || id == "qo100" ||
           id == "m17" || id == "freedv" || id == "dmr" || id == "p25" || id == "nxdn" ||
           id == "dpmr" || id == "dstar" || id == "tetra";
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
    ImGui::SeparatorText(tr("Multiplex"));
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
    ImGui::SeparatorText(tr("What to send"));
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
        if (s.deviationHz <= 0) s.deviationHz = 75000;
        { float d = (float)s.deviationHz; if (ImGui::SliderFloat(tr("Deviation (Hz)"), &d, 20000, 90000, "%.0f")) s.deviationHz = d; }
        {
            const char* pe[] = {"50 µs", "75 µs"};
            const int pei = s.preemphUs >= 70 ? 1 : 0;
            if (ImGui::BeginCombo(tr("Pre-emphasis"), pe[pei])) {
                if (ImGui::Selectable("50 µs", pei == 0)) s.preemphUs = 50;
                if (ImGui::Selectable("75 µs", pei == 1)) s.preemphUs = 75;
                ImGui::EndCombo();
            }
        }
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
        else { s.kind = 2; audioSourceRow(s, running); }
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
    case TxPayloadKind::Mac:
        ImGui::TextDisabled("%s", tr("D2-MAC test scene (custom video is not wired into this analogue generator)."));
        analogNfmBox(s, running);
        break;
    }
}

bool startTx(App& a, TxUi& s, const DeviceInfo* radio, const TxModeSpec* mode, bool carrier) {
    s.error.clear();
    if (!plat::basePrefs().getB("txNoticeAccepted", false)) { s.askNotice = true; return false; }
    std::string err;
    TxRequest req = makeReq(s, *mode, carrier);
    std::unique_ptr<TxSink> sink;
    if (!txClaimRadio(*radio, err)) { s.error = err; return false; }
    s.claimed = true;
    sink = makeTxSink(*radio);
    if (!sink) { s.error = tr("This radio has no transmit driver in OnAir."); release(s); return false; }
    mapPauseForTransmit(a);   // before the radio is keyed, so this frame cannot post what the receiver hears from it
    if (s.session.start(std::move(sink), req, TxRateLimits{radio->txMinRateHz, radio->txMaxRateHz}, err)) {
        s.radioName = radio->name; s.modeName = mode->name; s.freqSent = req.tune.centerHz;
        s.fileNote = s.session.stats().fileNote;
        s.carrierOnly = carrier;
        a.engine.log(std::string("transmit: ") + (carrier ? "carrier tune " : "") + mode->name + " on " + radio->name + " at " + std::to_string(s.freqMhz) + " MHz");
        return true;
    }
    s.error = err;
    if (!s.session.running()) { release(s); mapTransmitAbort(a); }
    return false;
}

} // namespace

bool& txViewFlag() { return ui().view; }
bool txActive() { return ui().session.running(); }

void txShutdown() {
    TxUi& s = ui();
    s.session.stop();
    release(s);
}

// every frame (the first call of a frame does the work): a transmission that ended by itself or failed gives its radio back; the on-screen indicator
void txTick(App&) {
    TxUi& s = ui();
    if (s.tickFrame == ImGui::GetFrameCount()) return;
    s.tickFrame = ImGui::GetFrameCount();
    if (!s.session.running() && s.claimed) {
        const TxSession::Stats st = s.session.stats();
        if (st.failed && s.error.empty()) s.error = st.error;
        s.session.stop();
        release(s);
        s.radioName.clear();
        mapTransmitEnded();
    }
    if (!s.session.running()) return;
    char b[220];
    const TxSession::Stats st = s.session.stats();
    snprintf(b, sizeof b, "%s   %s   %.6f MHz   %.0f s", tr("TRANSMITTING"), s.modeName.c_str(), s.freqSent / 1e6, st.seconds);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(b);
    const float pad = 8 * gUi;
    const float x = (ImGui::GetIO().DisplaySize.x - ts.x) * 0.5f - pad;
    const bool blink = std::fmod(ImGui::GetTime(), 1.2) < 0.8;
    dl->AddRectFilled(ImVec2(x, 4 * gUi), ImVec2(x + ts.x + 2 * pad, 4 * gUi + ts.y + 2 * pad), blink ? IM_COL32(200, 20, 20, 245) : IM_COL32(140, 10, 10, 245), 6 * gUi);
    dl->AddText(ImVec2(x + pad, 4 * gUi + pad), IM_COL32(255, 255, 255, 255), b);
}

// The RX / TX slider at the top left. Switching the view starts and stops nothing.
void txViewSwitch(App&) {
    TxUi& s = ui();
    const float h = ImGui::GetFrameHeight(), w = h * 3.2f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##rxtx", ImVec2(w, h));
    if (ImGui::IsItemClicked()) s.view = !s.view;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Switch between the receiver (RX) and the transmitter (TX). The transmitter has its own radio, so a receiver can keep receiving while you transmit."));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool on = s.view, tx = s.session.running();
    const ImU32 bg = tx ? IM_COL32(190, 25, 25, 255) : on ? IM_COL32(150, 60, 40, 255) : IM_COL32(60, 70, 85, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    const float kr = h * 0.5f - 2 * gUi;
    const ImVec2 kc(on ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(kc, kr, IM_COL32(245, 245, 245, 255));
    const char* lab = on ? tr("TX") : tr("RX");
    const ImVec2 ts = ImGui::CalcTextSize(lab);
    const float cx = on ? p.x + (w - h) * 0.5f : p.x + h + (w - h) * 0.5f;   // the label sits in the free half of the track
    dl->AddText(ImVec2(cx - ts.x * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), lab);
    if (tx && !on) { ImGui::SameLine(); ImGui::TextColored(kRed, "%s", tr("TRANSMITTING")); }
    ImGui::Dummy(ImVec2(w, 1.f));   // grow ##root so SetCursorPos of the slider does not extend past the window
}

const DeviceInfo* bindRadio(App& a, TxUi& s, int& conflict) {
    const std::vector<int> radios = txRadios(a);
    const DeviceInfo* radio = nullptr;
    for (int i : radios) if (deviceClaimKey(a.devices[(size_t)i]) == s.radioKey) radio = &a.devices[(size_t)i];
    if (!radio && s.radioKey.empty()) {
        for (int i : radios) if (!rxHolder(a.devices[(size_t)i])) { radio = &a.devices[(size_t)i]; s.radioKey = deviceClaimKey(*radio); break; }
    }
    conflict = radio ? rxHolder(*radio) : 0;
    return radio;
}

void txNoticePopup(TxUi& s) {
    if (s.askNotice) { ImGui::OpenPopup("##txnotice"); s.askNotice = false; }
    ImGui::SetNextWindowSize(ImVec2(480 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##txnotice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextColored(pal::heading(), "%s", tr("Before you transmit"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Transmitting needs a licence or permission where you are. You are responsible for what you emit and for not interfering with others "
                                  "(including services that people rely on for safety). Connect an antenna, an attenuator or a dummy load to the radio before you start. "
                                  "OnAir does not limit the frequency or the power: the radio's own limits are the only ones."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("I understand"), ImVec2(-1, 0))) { plat::basePrefs().setB("txNoticeAccepted", true); plat::basePrefs().flush(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void txSelectMode(App& a, int family, const char* subId) {
    TxUi& s = ui();
    s.family = family;
    snprintf(s.subId, sizeof s.subId, "%s", subId ? subId : "");
    const TxModeSpec* m = txModeForList(family, subId, s.dvbVariant);
    if (m) chooseMode(a, s, *m);
    else { s.modeId.clear(); s.modeName.clear(); }
}

bool txModeHighlighted(const ModeDef& m) {
    TxUi& s = ui();
    if (s.family < 0) return false;
    if (m.family != s.family) return false;
    if (!m.subId || !*m.subId) return s.subId[0] == 0;
    return std::string(s.subId) == m.subId;
}

void txBar(App& a) {
    TxUi& s = ui();
    ensureTxMode(a, s);
    const bool running = s.session.running();
    const TxModeSpec* mode = txModeById(s.modeId);
    int conflict = 0;
    const DeviceInfo* radio = bindRadio(a, s, conflict);
    const std::vector<int> radios = txRadios(a);

    ImGui::BeginDisabled(running);
    loadPayloadPrefs(s);
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputDouble(tr("Frequency (MHz)"), &s.freqMhz, 0.0, 0.0, "%.6f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::InputDouble(tr("Offset (MHz)"), &s.offsetMhz, 0.0, 0.0, "%.6f")) plat::basePrefs().setD("tx.offsetMhz", s.offsetMhz);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("This is added to the frequency sent to the radio, for a radio that radiates off frequency."));
    if (s.offsetMhz != 0) {
        ImGui::SameLine();
        ImGui::Text(tr("radio is told %.6f MHz"), s.freqMhz + s.offsetMhz);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240 * gUi);
    if (ImGui::BeginCombo(tr("Transmit radio"), radio ? radio->name.c_str() : tr("Choose a radio to transmit with."))) {
        for (int i : radios) {
            const DeviceInfo& d = a.devices[(size_t)i];
            const int h = rxHolder(d);
            char nm[256];
            if (h) snprintf(nm, sizeof nm, tr("%s (in use by RX %d)"), d.name.c_str(), h); else snprintf(nm, sizeof nm, "%s", d.name.c_str());
            if (ImGui::Selectable(nm, radio == &d)) { s.radioKey = deviceClaimKey(d); radio = &d; s.gainSet = false; }
        }
        ImGui::EndCombo();
    }
    if (radio) {
        const bool hasRange = radio->txGainMaxDb > radio->txGainMinDb;
        if (!s.gainSet || s.gainDb < radio->txGainMinDb - 1e-3 || (hasRange && s.gainDb > radio->txGainMaxDb + 1e-3)) {
            s.gainDb = (float)(hasRange ? radio->txGainMinDb : 0);
            s.gainSet = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180 * gUi);
        if (hasRange) ImGui::SliderFloat(tr("TX gain (dB)"), &s.gainDb, (float)radio->txGainMinDb, (float)radio->txGainMaxDb, "%.1f dB");
        else ImGui::InputFloat(tr("TX gain (dB)"), &s.gainDb, 0, 0, "%.1f");
        if (radio->kind == DeviceInfo::HackRF) { ImGui::SameLine(); ImGui::Checkbox(tr("+14 dB amplifier"), &s.ampOn); }
        // txChannels 0: not opened yet (LimeSDR-USB and bladeRF 2.0 have two, a USRP may have four). 1: no choice.
        int nOut = radio->txChannels;
        if (nOut <= 0) nOut = radio->board == "uhd" ? 4 : 2;
        if (!running) {
            if (radio->txChannels == 1) s.txCh = 0;
            else if (s.txCh < 0 || s.txCh >= nOut) s.txCh = 0;
        }
        if (radio->txChannels != 1) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80 * gUi);
            char cur[16];
            snprintf(cur, sizeof cur, "TX%d", s.txCh + 1);
            if (ImGui::BeginCombo(tr("TX channel"), cur)) {
                for (int c = 0; c < nOut; c++) {
                    char lab[32];
                    snprintf(lab, sizeof lab, "TX%d", c + 1);
                    if (ImGui::Selectable(lab, s.txCh == c)) s.txCh = c;
                }
                ImGui::EndCombo();
            }
            if (radio->txChannels <= 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("The exact number of outputs is known after this radio has transmitted once. An output it does not have is refused."));
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Repeat until stopped"), &s.loop);
    if (!s.loop) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        ImGui::InputInt(tr("seconds"), &s.onceSeconds);
        s.onceSeconds = std::max(1, std::min(3600, s.onceSeconds));
    }
    ImGui::EndDisabled();

    const bool can = radio && conflict == 0 && mode;
    if (running) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.10f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.15f, 0.15f, 1));
        if (ImGui::Button(tr("STOP TRANSMITTING"))) { s.session.stop(); release(s); s.radioName.clear(); s.carrierOnly = false; s.carrierHeld = false; a.engine.log("transmit: stopped by the user"); }
        ImGui::PopStyleColor(2);
        const TxSession::Stats st = s.session.stats();
        ImGui::SameLine();
        ImGui::TextColored(kRed, "%s", st.carrierTune ? tr("CARRIER TUNE") : tr("TRANSMITTING"));
        ImGui::SameLine();
        ImGui::Text("%.6f MHz  %.2f Msps  %.0f s", s.freqSent / 1e6, st.rateHz / 1e6, st.seconds);
    } else {
        ImGui::SameLine();
        ImGui::BeginDisabled(!can);
        if (ImGui::Button(tr("START TRANSMITTING"))) startTx(a, s, radio, mode, false);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!can && !running);
    if (ImGui::Button(tr("Carrier tune"))) {}
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Unmodulated carrier at this frequency and gain, for adjusting the transmission (drive, filter, SWR, dummy-load level) — not a message. Hold the button. Stop still applies. Not saved as transmitting."));
    const bool hold = ImGui::IsItemActive();
    if (hold && !s.carrierHeld) {
        s.carrierHeld = true;
        if (running) s.session.setCarrierTune(true);
        else startTx(a, s, radio, mode, true);
    } else if (!hold && s.carrierHeld) {
        s.carrierHeld = false;
        if (s.carrierOnly) { s.session.stop(); release(s); s.radioName.clear(); s.carrierOnly = false; a.engine.log("transmit: carrier tune released"); }
        else if (s.session.running()) s.session.setCarrierTune(false);
    }
    ImGui::EndDisabled();
    if (radios.empty()) ImGui::TextColored(pal::heading(), "%s", tr("No radio that can transmit was found. Plug one in (HackRF, PlutoSDR, or a SoapySDR radio with a transmitter) and refresh the radio list."));
    else if (!radio) ImGui::TextColored(pal::heading(), "%s", tr("Choose a radio to transmit with."));
    else if (conflict) ImGui::TextColored(kRed, tr("%s is in use by RX %d, and this radio cannot receive and transmit at the same time. Choose another radio for transmitting, or stop that receiver first."), radio->name.c_str(), conflict);
    if (!s.error.empty()) ImGui::TextColored(kRed, "%s", s.error.c_str());
    txNoticePopup(s);
}

void txSpectrum(App& a) {
    TxUi& s = ui();
    ensureTxMode(a, s);
    if (!s.wfInit && gGfx) { s.wf.init(gGfx); s.wfInit = true; }
    const bool running = s.session.running();
    std::vector<cf32> iq;
    double rate = 0;
    if (running) s.session.copyTap(iq, rate);
    constexpr int kFft = 2048;
    std::vector<float> db;
    if (iq.size() >= 64) txSpectrumDb(iq.data() + (iq.size() > (size_t)kFft ? iq.size() - kFft : 0), std::min(iq.size(), (size_t)kFft), kFft, db);
    const double fc = (running && s.freqSent > 0 ? s.freqSent : s.freqMhz * 1e6) / 1e6;
    const double bw = rate > 0 ? rate / 1e6 : 2;
    s.specX.resize(db.empty() ? 2 : db.size());
    s.specY.resize(s.specX.size());
    if (db.empty()) {
        s.specX[0] = fc - bw / 2; s.specX[1] = fc + bw / 2;
        s.specY[0] = s.specY[1] = -120.f;
    } else {
        const int n = (int)db.size();
        for (int i = 0; i < n; i++) {
            const int k = (i + n / 2) % n;   // DC in the middle of the plot
            s.specX[(size_t)i] = fc + ((i - n / 2) / (double)n) * (rate / 1e6);
            s.specY[(size_t)i] = db[(size_t)k];
        }
        if (s.wfInit) {
            std::vector<float> row(s.specY.size());
            for (size_t i = 0; i < s.specY.size(); i++) row[i] = (float)s.specY[i];
            s.wf.offer(row, ImGui::GetTime());
        }
    }
    ImGui::TextDisabled("%s", running ? tr("TX spectrum (IQ going to the radio)") : tr("Idle — nothing is being sent"));
    if (running) { ImGui::SameLine(); ImGui::Text("%.6f MHz   %.3f Msps", fc, rate / 1e6); }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float hs = std::max(80.f, avail.y * s.specFrac);
    if (plt::BeginPlot("##txspec", ImVec2(-1, hs), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_OpenBottom)) {
        plt::SetupAxes("MHz", "dB");
        plt::SetupAxisLimits(plt::X1, fc - bw / 2, fc + bw / 2, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -100, 5, plt::Cond_Once);
        plt::Spec ln; ln.LineColor = pal::accent(0.95f);
        if (!db.empty()) plt::PlotLine("tx", s.specX.data(), s.specY.data(), (int)s.specX.size(), ln);
        plt::EndPlot();
    }
    const float hf = std::max(60.f, avail.y - hs - 4 * gUi);
    if (s.wfInit && s.wf.img && plt::BeginPlot("##txwf", ImVec2(-1, hf), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_OpenTop)) {
        plt::SetupAxes("MHz", nullptr);
        plt::SetupAxisLimits(plt::X1, fc - bw / 2, fc + bw / 2, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -(double)Waterfall::H, 0, plt::Cond_Always);
        if (running && s.wf.filled > 0) {
            const double x0 = fc - bw / 2, x1 = fc + bw / 2;
            ImTextureID tex = s.wf.img->texture();
            const int w = s.wf.writeRow, H = Waterfall::H;
            plt::PlotImage("a", tex, plt::Point(x0, -(H - w)), plt::Point(x1, 0), ImVec2(0, (float)w / H), ImVec2(1, 1));
            if (w > 0) plt::PlotImage("b", tex, plt::Point(x0, -(double)H), plt::Point(x1, -(H - w)), ImVec2(0, 0), ImVec2(1, (float)w / H));
        }
        plt::EndPlot();
    }
    (void)a;
}

void txRight(App& a) {
    TxUi& s = ui();
    ensureTxMode(a, s);
    const bool running = s.session.running();
    const TxModeSpec* mode = txModeById(s.modeId);
    ImGui::PushTextWrapPos(0);
    if (!mode) {
        const std::string why = txRefusalReason(txEngineStd(s.family));
        ImGui::TextColored(pal::heading(), "%s", tr("This mode cannot transmit"));
        ImGui::TextWrapped("%s", tr(why.empty() ? "this mode has no transmit generator" : why.c_str()));
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextColored(pal::heading(), "%s", mode->name);
    payloadPanel(s, *mode, running);
    if (const ModeUi* mu = modeUi(familyOf(*mode)); mu && mu->synth && !isVoiceId(mode->id ? mode->id : "") && !isFtxId(mode->id ? mode->id : "") && std::string(mode->id) != "rtty" && std::string(mode->id) != "atv") {
        if (ImGui::TreeNode(tr("Receiver-style options (demo signal)"))) {
            const SynthConfig saved = a.tune.synth;
            a.tune.synth = s.synth;
            bool changed = false;
            mu->synth(a, changed);
            s.synth = a.tune.synth;
            a.tune.synth = saved;
            ImGui::TreePop();
        }
    }
    ImGui::SeparatorText(tr("Log"));
    const std::vector<std::string> log = s.session.log();
    if (ImGui::BeginChild("##txlog", ImVec2(0, 110 * gUi), true)) {
        for (const auto& l : log) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    ImGui::PopTextWrapPos();
}

void txPanel(App& a) {
    txBar(a);
    const float rightW = 330 * gUi;
    const float mainW = ImGui::GetContentRegionAvail().x - rightW - 8 * gUi;
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##txspecmain", ImVec2(mainW, h));
    txSpectrum(a);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##txright", ImVec2(0, h));
    txRight(a);
    ImGui::EndChild();
}
