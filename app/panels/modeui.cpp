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
extern const ModeUi kSitorUi;
extern const ModeUi kDgpsUi;
extern const ModeUi kElLoranUi;
extern const ModeUi kMpt1327Ui;
extern const ModeUi kFusionUi;
extern const ModeUi kLtrUi;
extern const ModeUi kEdacsUi;
extern const ModeUi kMilhfUi;
extern const ModeUi kMercuryUi;

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
    case 120: return &kDgpsUi;
    case 127: return &kMpt1327Ui;
    case 128: return &kFusionUi;
    case 129: return &kLtrUi;
    case 130: return &kEdacsUi;
    case 145: return &kMilhfUi;
    case 230: return &kMercuryUi;
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
    default: return family >= 999 ? &kPluginUi : nullptr;   // the Plugins row and every plugin mode (plugin/plugin_app.cpp)
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
static std::set<int>& wideFamilies() {
    static std::set<int> s = {9, 31, 33, 38, 45, 50, 23, 24};   // DMR, M17, D-STAR, NXDN, dPMR, P25, pagers, APRS / packet: one decoder per carrier (core/wide_modes.h)
    return s;
}
bool modeSupportsWide(int family) { return modeSupportsWideHf(family) || wideFamilies().count(family) > 0; }
void registerWideMode(int family) { wideFamilies().insert(family); }
bool wideActive(const App& a) { return a.wideDecode && modeSupportsWide(a.family) && a.engine.running() && Engine::wideSupported(a.family + 1) && a.rx.standard == a.family + 1; }

void wideSync(App& a) {
    static int lastFam = -1; static bool lastOn = false, lastMuted = false; static int lastSel = -1;
    if (a.family != lastFam) {   // the switch is kept per mode
        lastFam = a.family;
        a.wideDecode = a.family >= 0 && a.family < 64 && ((a.wideFamMask >> a.family) & 1);
        a.wideSel = 0; lastSel = -1;
    }
    const bool on = a.wideDecode && modeSupportsWide(a.family) && Engine::wideSupported(a.family + 1);
    if (on != lastOn) { a.engine.setWideVoice(on); lastOn = on; a.wideSel = 0; lastSel = -1; }
    if (!on) return;
    if (a.wideSel != lastSel) { a.engine.setWideSelected(a.wideSel); lastSel = a.wideSel; }
    if (a.muted != lastMuted) { a.engine.setWideMuted(a.muted); lastMuted = a.muted; }
}

void wideCarrierCell(const App& a, int carrierId, double relHz) {
    if (carrierId <= 0) { ImGui::TextDisabled("-"); return; }
    ImGui::Text("%.4f", radioCenterMhz(a) + relHz / 1e6);
}

void wideCarriersTable(App& a, float h) {
    if (!wideActive(a)) return;
    ImGui::TextDisabled("%s", tr("Carriers in the span (click: listen and show; double click: tune to it)"));
    if (!ImGui::BeginTable("##widecars", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h > 0 ? h : 140 * gUi))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Carrier MHz")); ImGui::TableSetupColumn(tr("Level dB")); ImGui::TableSetupColumn(tr("SNR dB"));
    ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Details"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    static const char* kState[3] = {"searching", "partial", "locked"};
    for (const WideCarrier& c : a.rx.wide) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char b[48]; snprintf(b, sizeof b, "%.4f##wc%d", radioCenterMhz(a) + c.hz / 1e6, c.id);
        if (ImGui::Selectable(b, c.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            a.wideSel = c.id;
            if (ImGui::IsMouseDoubleClicked(0)) tuneFreq(a, radioCenterMhz(a) + c.hz / 1e6);
        }
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.snrDb);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(kState[std::max(0, std::min(2, c.state))]));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.active ? tr("on air") : tr("held"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.info.c_str());
    }
    ImGui::EndTable();
}
