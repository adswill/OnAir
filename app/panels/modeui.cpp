// The screens of the modes added after FM: the table that maps a family to its ModeUi, and small helpers for the shell.
#include "../app.h"

extern const ModeUi kDvbsUi;
extern const ModeUi kDtmbUi;
extern const ModeUi kAtvUi;
extern const ModeUi kDmrUi;
extern const ModeUi kDrmUi;
extern const ModeUi kAdsbUi;
extern const ModeUi kGnssUi;
extern const ModeUi kSondeUi;
extern const ModeUi kAisUi;
extern const ModeUi kMarineUi;
extern const ModeUi kAcarsUi;
extern const ModeUi kInmcUi;
extern const ModeUi kAeroUi;
extern const ModeUi kIridiumUi;
extern const ModeUi kMeshUi;
extern const ModeUi kHdrUi;
extern const ModeUi kCdrUi;
extern const ModeUi kPagerUi;
extern const ModeUi kPacketUi;
extern const ModeUi kHfdigUi;
extern const ModeUi kAirbandUi;
extern const ModeUi kVdl2Ui;
extern const ModeUi kVdl4Ui;
extern const ModeUi kIsmUi;
extern const ModeUi kVorUi;
extern const ModeUi kNdbUi;
extern const ModeUi kM17Ui;
extern const ModeUi kCellUi;
extern const ModeUi kCwUi;
extern const ModeUi kDstarUi;
extern const ModeUi kHfdlUi;
extern const ModeUi kNxdnUi;
extern const ModeUi kDpmrUi;
extern const ModeUi kMacUi;
extern const ModeUi kGsmrUi;
extern const ModeUi kDvbcUi;
extern const ModeUi kIsdbsUi;
extern const ModeUi kOrbcommUi;
extern const ModeUi kTetraUi;
extern const ModeUi kTetrapolUi;
extern const ModeUi kElektroUi;
extern const ModeUi kUatUi;
extern const ModeUi kPskUi;
extern const ModeUi kJs8Ui;
extern const ModeUi kSpecxUi;
extern const ModeUi kSpecanUi;
extern const ModeUi kUplinkUi;
extern const ModeUi kAleUi;
extern const ModeUi kAstroUi;
extern const ModeUi kP25Ui;
extern const ModeUi kBleUi;
extern const ModeUi kIonoUi;
extern const ModeUi kQo100Ui;
extern const ModeUi kWifiUi;
extern const ModeUi kGeneralUi;
extern const ModeUi kTimeUi;
extern const ModeUi kCubesatUi;
extern const ModeUi kGlobalstarUi;
extern const ModeUi kPluginUi;
extern const ModeUi kModeBuilderUi;
extern const ModeUi kVdesUi;
extern const ModeUi kZigbeeUi;
extern const ModeUi kFlarmUi;
extern const ModeUi kHfbeaconUi;
extern const ModeUi kDjiUi;
extern const ModeUi kNbiotUi;
extern const ModeUi kHalowUi;
extern const ModeUi kCodarUi;
extern const ModeUi kWbUi;
extern const ModeUi kDatawellUi;
extern const ModeUi kSameUi;
extern const ModeUi kAntUi;
extern const ModeUi kDectUi;
extern const ModeUi kMultitoneUi;
extern const ModeUi kLoraRawUi;
extern const ModeUi kWisunUi;
extern const ModeUi kEczasUi;
extern const ModeUi kFpvUi;
extern const ModeUi kWmbusUi;
extern const ModeUi kTonesUi;
extern const ModeUi kSarsatUi;
extern const ModeUi kKaalUi;
extern const ModeUi kKaitUi;
extern const ModeUi kKavbUi;
extern const ModeUi kKappUi;
extern const ModeUi kKaftUi;
extern const ModeUi kKawsUi;
extern const ModeUi kMt63Ui;
extern const ModeUi kFsqUi;
extern const ModeUi kIfkpUi;
extern const ModeUi kCquamUi;
extern const ModeUi kModeAcUi;
extern const ModeUi kZwaveUi;
extern const ModeUi kMscatterUi;
extern const ModeUi kArdopUi;
extern const ModeUi kGoesDcsUi;
extern const ModeUi kBletrkUi;
extern const ModeUi kSatbeaconUi;
extern const ModeUi kRidUi;
extern const ModeUi kMeteorUi;
extern const ModeUi kNrUi;
extern const ModeUi kGoesUi;
extern const ModeUi kDmeUi;
extern const ModeUi kHrptUi;
extern const ModeUi kLbjUi;
extern const ModeUi kKlwUi;
extern const ModeUi kTdcsUi;
extern const ModeUi kHorusUi;
extern const ModeUi kSitorUi;
extern const ModeUi kDgpsUi;
extern const ModeUi kElLoranUi;
extern const ModeUi kNfcUi;
extern const ModeUi kMpt1327Ui;
extern const ModeUi kFusionUi;
extern const ModeUi kLtrUi;
extern const ModeUi kEdacsUi;
extern const ModeUi kP25p2Ui;
extern const ModeUi kMilhfUi;
extern const ModeUi kSbasUi;
extern const ModeUi kAsterixUi;
extern const ModeUi kArgosUi;
extern const ModeUi kSigfoxUi;
extern const ModeUi kMarineVhfUi;
extern const ModeUi kLorawanUi;
extern const ModeUi kLoraSatUi;
extern const ModeUi kPactorUi;
extern const ModeUi kPskdUi;

// ---- whole-span decoding ("wide decode"): the families whose receivers can decode everything in the visible span (widespan.h) ----
bool modeSupportsWideHf(int family) { return family == 25 || family == 34 || family == 36; }   // HF digital, CW, PSK: widespan.h inside the receiver

const ModeUi* modeUi(int family) {
    switch (family) {
    case 6: return &kDvbsUi;
    case 7: return &kDtmbUi;
    case 8: return &kAtvUi;
    case 9: return &kDmrUi;
    case 10: return &kDrmUi;
    case 11: return &kAdsbUi;
    case 12: return &kGnssUi;
    case 13: return &kSondeUi;
    case 14: return &kAisUi;
    case 15: return &kMarineUi;
    case 16: return &kAcarsUi;
    case 17: return &kInmcUi;
    case 18: return &kAeroUi;
    case 19: return &kIridiumUi;
    case 20: return &kMeshUi;
    case 21: return &kHdrUi;
    case 22: return &kCdrUi;
    case 23: return &kPagerUi;
    case 24: return &kPacketUi;
    case 25: return &kHfdigUi;
    case 26: return &kAirbandUi;
    case 27: return &kVdl2Ui;
    case 82: return &kVdl4Ui;   // std 84 = family + 2
    case 28: return &kIsmUi;
    case 29: return &kNdbUi;
    case 30: return &kVorUi;
    case 31: return &kM17Ui;
    case 32: return &kCellUi;
    case 34: return &kCwUi;
    case 33: return &kDstarUi;
    case 35: return &kHfdlUi;
    case 38: return &kNxdnUi;
    case 45: return &kDpmrUi;
    case 47: return &kMacUi;
    case 39: return &kGsmrUi;
    case 41: return &kSpecxUi;
    case 40: return &kDvbcUi;
    case 154: return &kIsdbsUi;
    case 44: return &kOrbcommUi;
    case 46: return &kTetraUi;
    case 36: return &kPskUi;
    case 37: return &kJs8Ui;
    case 42: return &kSpecanUi;
    case 43: return &kUplinkUi;
    case 49: return &kAleUi;
    case 48: return &kAstroUi;
    case 50: return &kP25Ui;
    case 51: return &kBleUi;
    case 52: return &kIonoUi;
    case 53: return &kQo100Ui;
    case 54: return &kWifiUi;
    case 55: return &kGeneralUi;
    case 58: return &kTimeUi;
    case 61: return &kCubesatUi;
    case 62: return &kGlobalstarUi;
    case 63: return &kVdesUi;
    case 65: return &kZigbeeUi;
    case 66: return &kFlarmUi;
    case 67: return &kHfbeaconUi;
    case 68: return &kDjiUi;
    case 69: return &kNbiotUi;
    case 70: return &kHalowUi;
    case 72: return &kCodarUi;
    case 73: return &kWbUi;
    case 74: return &kDatawellUi;
    case 76: return &kSameUi;
    case 77: return &kAntUi;
    case 78: return &kDectUi;
    case 79: return &kTetrapolUi;
    case 80: return &kElektroUi;
    case 81: return &kLbjUi;
    case dect2::railway::kKlwFamily: return &kKlwUi;
    case dect2::railway::kTdcsFamily: return &kTdcsUi;
    case dect2::horus::kFamily: return &kHorusUi;
    case 120: return &kDgpsUi;
    case 127: return &kMpt1327Ui;
    case 128: return &kFusionUi;
    case 129: return &kLtrUi;
    case 130: return &kEdacsUi;
    case 180: return &kP25p2Ui;
    case 145: return &kMilhfUi;
    case 150: return &kSbasUi;
    case 151: return &kAsterixUi;
    case 152: return &kArgosUi;
    case 153: return &kSigfoxUi;
    case 155: return &kMarineVhfUi;
    case 200: return &kLorawanUi;
    case 201: return &kLoraSatUi;
    case 203: return &kPactorUi;
    case 202: return &kPskdUi;
    case 83: return &kUatUi;
    case 75: return &kMultitoneUi;
    case 71: return &kLoraRawUi;
    case 84: return &kWisunUi;
    case 85: return &kEczasUi;
    case 101: return &kFpvUi;
    case 86: return &kWmbusUi;
    case 89: return &kTonesUi;
    case 92: return &kSarsatUi;
    case 122: return &kKaalUi;
    case 131: return &kKaitUi;
    case 123: return &kKavbUi;
    case 124: return &kKappUi;
    case 125: return &kKaftUi;
    case 133: return &kKawsUi;
    case 95: return &kMt63Ui;
    case 113: return &kFsqUi;
    case 116: return &kIfkpUi;
    case 121: return &kDmeUi;
    case 126: return &kElLoranUi;
    case 148: return &kNfcUi;
    case 107: return &kCquamUi;
    case 142: return &kModeAcUi;
    case 98: return &kZwaveUi;
    case 104: return &kMscatterUi;
    case 140: return &kArdopUi;
    case 141: return &kGoesDcsUi;
    case 143: return &kBletrkUi;
    case 144: return &kSatbeaconUi;
    case 56: return &kRidUi;
    case 59: return &kMeteorUi;
    case 57: return &kNrUi;
    case 60: return &kGoesUi;
    case 64: return &kHrptUi;
    case 119: return &kSitorUi;
    default:
        if (isCustomFamily(family) || family == kCustomFamilyBase - 1) return &kModeBuilderUi;   // Mode Builder and the custom modes (modebuilder_ui.cpp)
        return family >= 999 ? &kPluginUi : nullptr;   // the Plugins row and every plugin mode (plugin/plugin_app.cpp)
    }
}

// the title of the list on the right: STATIONS for FM and DAB, the mode's own, or SERVICES
const char* listTitle(const App& a, bool upper) {
    if (const ModeUi* mu = modeUi(a.family)) {
        if (upper) return mu->sideTitle;
        static char b[8][32];
        static int k = 0;
        char* o = b[k++ & 7];
        snprintf(o, 32, "%s", mu->sideTitle);
        for (int i = 1; o[i]; i++) o[i] = (char)tolower((unsigned char)o[i]);
        return o;
    }
    const bool stations = a.fmMode || a.dabMode;
    return upper ? (stations ? "STATIONS" : "SERVICES") : (stations ? "Stations" : "Services");
}

// ---------------------------------------------------------------- second mode

namespace {
// The mode's own screens drawn for the second mode: a.rx holds its report meanwhile, and its sound is pushed muted unless asked for
struct SecondScope {
    App& a;
    bool muted;
    explicit SecondScope(App& app) : a(app), muted(app.muted) {
        std::swap(a.rx, a.second.rx);
        a.muted = muted || !a.second.audio;
        a.second.drawing = true;
    }
    ~SecondScope() {
        std::swap(a.rx, a.second.rx);
        a.muted = muted;
        a.second.drawing = false;
    }
};
}   // namespace

bool setSecondMode(App& a, int idx, double mhz) {
    if (idx < 0 || idx >= kNumModes) {
        a.engine.clearSecondary();
        a.second.modeIdx = -1;
        a.second.rx = RxTelemetry(); a.second.seq = 0;
        return true;
    }
    const ModeDef& m = kModes[idx];
    if (m.subId) if (const SubMode* sm = subModeById(m.subId)) applySubMode(a.engine, sm);   // one protocol: only its decoder
    a.engine.setSecondarySound(a.second.audio);
    const bool ok = a.engine.setSecondaryHz(m.family + 2, mhz * 1e6);
    a.second.rx = RxTelemetry(); a.second.seq = 0;
    a.second.modeIdx = ok ? idx : -1;
    a.second.mhz = mhz;
    return ok;
}

void secondModeTick(App& a) {
    if (a.second.modeIdx < 0) return;
    if (a.engine.secondaryStandard() == 0) { a.second.modeIdx = -1; return; }   // refused or turned off in the engine
    RxTelemetry t;
    if (a.engine.latestRx2(t, a.second.seq)) { a.second.seq = t.seq; a.second.rx = std::move(t); }
    const ModeUi* mu = modeUi(kModes[a.second.modeIdx].family);
    if (mu && mu->tick) { SecondScope s(a); mu->tick(a); }
}

void secondModeTabs(App& a) {
    if (a.second.modeIdx < 0) return;
    const ModeDef& m = kModes[a.second.modeIdx];
    char name[96];
    snprintf(name, sizeof name, "Second: %s", m.name);
    const ModeUi* mu = modeUi(m.family);
    if (!tabItem(name, mu ? mu->tabIcon : Ic::Layers)) return;
    if (!a.engine.secondaryRunning()) {
        const std::string note = a.engine.secondaryNote();
        ImGui::TextDisabled("%s at %.4f MHz is not running%s%s", m.name, a.second.mhz, note.empty() ? "" : ": ", note.c_str());
    }
    if (mu && mu->tab) { SecondScope s(a); mu->tab(a); }
    ImGui::EndTabItem();
}

// ---------------------------------------------------------------- whole-span decoding
#include <set>
#include <algorithm>
#include <filesystem>
#include <map>
#include <cctype>
#include "../platform/platform.h"
static std::set<int>& wideFamilies() {
    // DMR, M17, D-STAR, NXDN, dPMR, P25, pagers, APRS / packet, MPT1327, Yaesu Fusion, LTR, EDACS: one decoder per carrier (core/wide_modes.h)
    static std::set<int> s = {9, 31, 33, 38, 45, 50, 23, 24, 127, 128, 129, 130};
    return s;
}
bool modeSupportsWide(int family) { return modeSupportsWideHf(family) || wideFamilies().count(family) > 0; }
void registerWideMode(int family) { wideFamilies().insert(family); }
bool wideActive(const App& a) { return a.wideDecode && modeSupportsWide(a.family) && a.engine.running() && Engine::wideSupported(a.family + 1) && a.rx.standard == a.family + 1; }

bool wideFamOn(const App& a, int f) {
    if (f < 0 || f >= 192) return false;
    const uint64_t m = f < 64 ? a.wideFamMask : a.wideFamMaskHi[f / 64 - 1];
    return (m >> (f & 63)) & 1;
}
void setWideFam(App& a, int f, bool on) {
    if (f < 0 || f >= 192) return;
    uint64_t& m = f < 64 ? a.wideFamMask : a.wideFamMaskHi[f / 64 - 1];
    if (on) m |= 1ull << (f & 63); else m &= ~(1ull << (f & 63));
}

void wideSync(App& a) {
    static int lastFam = -1; static bool lastOn = false, lastMuted = false; static int lastSel = -1;
    if (a.family != lastFam) {   // the switch is kept per mode
        lastFam = a.family;
        a.wideDecode = wideFamOn(a, a.family);
        a.wideSel = 0; lastSel = -1;
    }
    if (a.wideTuningDirty) { a.engine.setWideTuning(a.wideTuning); a.wideTuningDirty = false; }
    const bool on = a.wideDecode && modeSupportsWide(a.family) && Engine::wideSupported(a.family + 1);
    if (on != lastOn) { a.engine.setWideVoice(on); lastOn = on; a.wideSel = 0; lastSel = -1; }
    if (!on) return;
    if (a.wideSel != lastSel) { a.engine.setWideSelected(a.wideSel); lastSel = a.wideSel; }
    if (a.muted != lastMuted) { a.engine.setWideMuted(a.muted); lastMuted = a.muted; }
    {   // the band plan's channels around the radio: known channels for the detector (the frequency database's are added in core)
        static std::string lastKey;
        const double c = radioCenterMhz(a) * 1e6;
        char key[128]; snprintf(key, sizeof key, "%d|%s|%.0f", a.family, a.bandPlan.c_str(), std::round(c / 1e5));
        if (lastKey != key) { lastKey = key; a.engine.setWideExtraPriors(bandPlanChannels(a, c - 10e6, c + 10e6)); }
    }
}

void wideCarrierCell(const App& a, int carrierId, double relHz) {
    if (carrierId <= 0) { ImGui::TextDisabled("-"); return; }
    ImGui::Text("%.4f", radioCenterMhz(a) + relHz / 1e6);
}

namespace {
// the detector's settings, in a popup from the table's header line
void wideSettingsPopup(App& a) {
    if (!ImGui::BeginPopup("##widecfg")) return;
    dect2::WideTuning& w = a.wideTuning;
    bool ch = false;
    float on = (float)w.onDb, off = (float)w.offDb, snr = (float)w.minSnrDb, dwell = (float)(w.dwellSec < 0 ? 0.3 : w.dwellSec), hold = (float)(w.holdSec < 0 ? 8 : w.holdSec);
    float cpu = (float)(w.cpuBudget * 100);
    ImGui::TextDisabled("%s", tr("Whole-span carrier detector"));
    ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Threshold dB"), &on, 4, 30, "%.1f")) { w.onDb = on; w.offDb = std::min(w.offDb, w.onDb - 0.5); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A carrier must be this far above the noise floor to be found"));
    ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Quiet below dB"), &off, 1, (float)w.onDb - 0.5f, "%.1f")) { w.offDb = off; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hysteresis: a found carrier counts as quiet only below this"));
    ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Minimum SNR dB"), &snr, 0, 40, "%.1f")) { w.minSnrDb = snr; ch = true; }
    bool own = w.dwellSec < 0;
    if (ImGui::Checkbox(tr("Mode's own dwell and hold"), &own)) { w.dwellSec = own ? -1 : 0.3; w.holdSec = own ? -1 : 8; ch = true; }
    if (!own) {
        ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Dwell s"), &dwell, 0.05f, 3, "%.2f")) { w.dwellSec = dwell; ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A new carrier must stay on this long before it is decoded"));
        ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("Hold s"), &hold, 1, 120, "%.0f")) { w.holdSec = hold; ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A quiet carrier keeps its decoder this long"));
    }
    ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderInt(tr("Carriers at most"), &w.maxCarriers, 1, 32)) ch = true;
    ImGui::SetNextItemWidth(160 * gUi); if (ImGui::SliderFloat(tr("CPU limit %"), &cpu, 0, 100, cpu <= 0 ? "off" : "%.0f")) { w.cpuBudget = cpu / 100.0; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When the decoders take longer than this share of real time, the weakest carrier is dropped (never the one you listen to)"));
    if (ImGui::Checkbox(tr("Known channels first"), &w.usePriors)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Channels of this mode in the frequency database are found at a 3 dB lower threshold"));
    if (w.usePriors) { ImGui::SameLine(); if (ImGui::Checkbox(tr("only"), &w.priorsOnly)) ch = true; }
    if (ImGui::SmallButton(tr("Defaults"))) { w = dect2::WideTuning(); ch = true; }
    if (ch) { a.wideTuningDirty = true; savePrefs(a); }
    ImGui::EndPopup();
}
} // namespace

void wideCarriersTable(App& a, float h) {
    if (!wideActive(a)) return;
    static char filter[48] = "";
    static bool onlyLocked = false, onlyOnAir = false;
    static std::map<int, std::string> recording;   // carrier id -> file
    const dect2::WideStatus& st = a.rx.wideStatus;
    // the header line: the floor, the load, the cap; the settings
    ImGui::TextDisabled("%s", tr("Carriers in the span (click: listen and show; double click or Tune: tune to it)"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Settings##wide"))) ImGui::OpenPopup("##widecfg");
    wideSettingsPopup(a);
    ImGui::TextDisabled(tr("noise %.1f dB, %zu carriers, CPU %.0f %% of real time, at most %d"), st.noiseDb, a.rx.wide.size(), st.load * 100, st.cap);
    if (st.tooSlow) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), tr("too slow: the weakest carriers were dropped (%llu); raise the CPU limit or narrow the span"), (unsigned long long)st.droppedForCpu);
    }
    ImGui::SetNextItemWidth(140 * gUi); ImGui::InputTextWithHint("##widefilt", tr("filter"), filter, sizeof filter);
    ImGui::SameLine(); ImGui::Checkbox(tr("locked only"), &onlyLocked);
    ImGui::SameLine(); ImGui::Checkbox(tr("on air only"), &onlyOnAir);
    const int flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (!ImGui::BeginTable("##widecars", 9, flags, ImVec2(0, h > 0 ? h : 160 * gUi))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Carrier MHz"), ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn(tr("Level dB")); ImGui::TableSetupColumn(tr("SNR dB")); ImGui::TableSetupColumn(tr("Width kHz"));
    ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Age"));
    ImGui::TableSetupColumn(tr("Details"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoSort);
    ImGui::TableHeadersRow();
    std::vector<const WideCarrier*> rows;
    const std::string f = filter;
    for (const WideCarrier& c : a.rx.wide) {
        if (onlyLocked && c.state < 2) continue;
        if (onlyOnAir && !c.active) continue;
        if (!f.empty()) {
            char mhz[32]; snprintf(mhz, sizeof mhz, "%.4f", radioCenterMhz(a) + c.hz / 1e6);
            auto lower = [](std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
            if (lower(c.info).find(lower(f)) == std::string::npos && std::string(mhz).find(f) == std::string::npos) continue;
        }
        rows.push_back(&c);
    }
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
        const int col = ss->Specs[0].ColumnIndex; const bool asc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        auto key = [col](const WideCarrier* c) -> double {
            switch (col) { case 1: return c->levelDb; case 2: return c->snrDb; case 3: return c->widthHz; case 4: return c->state; case 5: return c->active ? -1 : c->idleSec; case 6: return c->ageSec; default: return c->hz; }
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const WideCarrier* x, const WideCarrier* y) { return asc ? key(x) < key(y) : key(x) > key(y); });
    }
    static const char* kState[3] = {"searching", "partial", "locked"};
    for (auto it = recording.begin(); it != recording.end();) {   // a carrier that has gone: its file was closed by the engine
        bool live = false; for (const WideCarrier& c : a.rx.wide) if (c.id == it->first) live = true;
        it = live ? std::next(it) : recording.erase(it);
    }
    for (const WideCarrier* cp : rows) {
        const WideCarrier& c = *cp;
        const double mhz = radioCenterMhz(a) + c.hz / 1e6;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char b[48]; snprintf(b, sizeof b, "%.4f%s##wc%d", mhz, c.prior ? " *" : "", c.id);
        if (ImGui::Selectable(b, c.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
            a.wideSel = c.id;
            if (ImGui::IsMouseDoubleClicked(0)) tuneFreq(a, mhz);
        }
        if (c.prior && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("a known channel of this mode (frequency database)"));
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.snrDb);
        ImGui::TableNextColumn(); if (c.widthHz > 0) ImGui::Text("%.1f", c.widthHz / 1e3); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(kState[std::max(0, std::min(2, c.state))]));
        ImGui::TableNextColumn();
        if (c.active) ImGui::TextUnformatted(tr("on air")); else ImGui::TextDisabled(tr("held %.0f s"), c.idleSec);
        ImGui::TableNextColumn();
        if (c.ageSec < 120) ImGui::Text("%.0f s", c.ageSec); else ImGui::Text("%.0f min", c.ageSec / 60);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.info.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(c.id);
        if (ImGui::SmallButton(c.selected && a.wideSel == c.id ? tr("Auto") : tr("Listen"))) a.wideSel = (a.wideSel == c.id) ? 0 : c.id;
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Tune"))) tuneFreq(a, mhz);
        ImGui::SameLine();
        const bool rec = c.recording;
        if (ImGui::SmallButton(rec ? tr("Stop") : tr("Rec"))) {
            if (rec) { a.engine.stopWideRecording(c.id); auto r = recording.find(c.id); if (r != recording.end()) { a.engine.log("carrier IQ saved to " + r->second); recording.erase(r); } }
            else {
                std::error_code ec;
                const std::string dir = plat::dataDir() + "/carriers";
                std::filesystem::create_directories(dir, ec);
                const std::string path = a.engine.startWideRecording(c.id, dir);
                if (path.empty()) a.engine.log("carrier recording could not start in " + dir);
                else recording[c.id] = path;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rec ? tr("Stop the IQ recording of this carrier") : tr("Record this carrier's IQ (WAV, 4 samples per channel step) to the carriers folder"));
        ImGui::PopID();
    }
    ImGui::EndTable();
}
