// Smart CubeSat view (docs/CUBESAT-SMART.md): one button, "Auto decode cubesats". It tunes a span that holds the satellites about to pass, follows
// their Doppler, scans the span for narrow carriers, identifies each (CW, AFSK, FSK / GMSK, BPSK and its baud rate) and decodes it. The view
// shows the next passes, the carriers with what each is doing, the decoded packets with their telemetry, and the exports (SatNOGS-ready file,
// CSV, KISS). Everything manual is under Advanced.
#include "app.h"
#include "dect2/cubesat/cubesat_smart.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace dect2;
using namespace dect2::cubesat;

std::string modulationTextUi(const dect2::cubesat::SmartPacket& p);

namespace {

struct State {
    bool loaded = false, tlesLoaded = false;
    SmartSettings cfg;
    bool settingsPushed = false;
    // passes, computed on a worker thread
    std::mutex mu;
    std::vector<PlannedPass> passes;
    std::atomic<bool> busy{false};
    double passesAt = 0;
    double lastTleLoad = -1e9, obsLat = 1e9, obsLon = 1e9;
    int selPacket = -1;
    uint64_t selKey = 0;
    std::string note;
    // advanced: user transmitters
    std::vector<TxEntry> userTx;
    char newName[40] = "my satellite";
    int newNorad = 0;
    double newMhz = 437.0;
    int newMod = ModFsk, newBaud = 9600;
    bool showAdvanced = false;
};
State S;

double nowUnix() { return (double)std::time(nullptr); }

std::string tleDir() { return plat::dataDir() + "/tle"; }

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void saveCfg() {
    plat::Prefs& p = plat::prefs();
    p.setB("csmPasses", S.cfg.usePasses); p.setB("csmBlind", S.cfg.blind);
    p.setD("csmMinSnr", S.cfg.minSnrDb); p.setI("csmMaxCh", S.cfg.maxChannels); p.setD("csmMinEl", S.cfg.minElDeg); p.setD("csmSearch", S.cfg.searchHz);
    p.setI("csmForceMod", S.cfg.forceMod); p.setI("csmForceBaud", S.cfg.forceBaud); p.setD("csmChanHz", S.cfg.channelHz); p.setD("csmScanBin", S.cfg.scanBinHz);
    p.setS("csmUserTx", txToText(S.userTx));
    S.settingsPushed = false;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.cfg.usePasses = p.getB("csmPasses", true); S.cfg.blind = p.getB("csmBlind", true);
    S.cfg.minSnrDb = p.getD("csmMinSnr", 8.0); S.cfg.maxChannels = (int)p.getI("csmMaxCh", 4); S.cfg.minElDeg = p.getD("csmMinEl", 0.0); S.cfg.searchHz = p.getD("csmSearch", 5000.0);
    S.cfg.forceMod = (int)p.getI("csmForceMod", ModUnknown); S.cfg.forceBaud = (int)p.getI("csmForceBaud", 0);
    S.cfg.channelHz = p.getD("csmChanHz", 0.0); S.cfg.scanBinHz = p.getD("csmScanBin", 0.0);
    S.userTx = txFromText(p.getS("csmUserTx", ""));
}

// the observer, the TLEs the pass planner keeps (and the built-in snapshot underneath), the user's transmitters
void feedKnowledge(App& a) {
    SmartCubesat& sm = a.engine.smartCubeDecoder();
    double lat, lon;
    receiverLocation(lat, lon);
    const double now = nowUnix();
    if (lat != S.obsLat || lon != S.obsLon) {
        S.obsLat = lat; S.obsLon = lon;
        Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000.0;
        sm.setObserver(o);
        S.passesAt = 0;
    }
    if (!S.settingsPushed) {
        S.settingsPushed = true;
        sm.setSettings(S.cfg);
        sm.setUserTransmitters(S.userTx);
        S.passesAt = 0;
    }
    if (now - S.lastTleLoad > 120.0) {
        S.lastTleLoad = now;
        std::error_code ec;
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(tleDir(), ec)) {
            const std::string ext = e.path().extension().string();
            if (ext == ".txt" || ext == ".tle") n += sm.addTleText(readFile(e.path().string()));
        }
        if (n) S.passesAt = 0;
    }
}

void refreshPasses(App& a) {
    const double now = nowUnix();
    if (S.busy.load() || now - S.passesAt < 60.0) return;
    S.busy = true;
    S.passesAt = now;
    SmartCubesat* sm = &a.engine.smartCubeDecoder();
    const double minEl = std::max(5.0, S.cfg.minElDeg);
    std::thread([sm, now, minEl] {
        std::vector<PlannedPass> v = sm->upcoming(now, 6.0, minEl, 12);
        { std::lock_guard<std::mutex> lk(S.mu); S.passes = std::move(v); }
        S.busy = false;
    }).detach();
}

std::string clockText(double t) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string inText(double dt) {
    char b[24];
    if (dt <= 0) return tr("now");
    if (dt < 3600) snprintf(b, sizeof b, "%d:%02d", (int)dt / 60, (int)dt % 60);
    else snprintf(b, sizeof b, "%dh %02dm", (int)dt / 3600, ((int)dt % 3600) / 60);
    return b;
}

// the one button
void startAuto(App& a) {
    load();
    if (a.family != 61) selectMode(a, 61);
    SmartCubesat& sm = a.engine.smartCubeDecoder();
    feedKnowledge(a);
    if (!a.engine.running()) {
        const double maxSpan = std::max(2.0e6, a.tune.sampleRate);
        const SmartCubesat::TuneAdvice adv = sm.adviseTune(nowUnix(), maxSpan);
        if (adv.valid) {
            a.freqMhz = adv.centerHz / 1e6;
            const double need = std::min(10.0e6, std::ceil(adv.spanHz * 1.3 / 0.5e6) * 0.5e6);
            if (need > a.tune.sampleRate) a.tune.sampleRate = need;
            S.note = std::string(tr("Tuned for")) + ": " + adv.why;
        } else {
            S.note = tr("No satellite is expected soon: scanning this span for any carrier.");
        }
    }
    a.engine.setSmartCube(true);
    if (!a.engine.running()) startReceiver(a);
}

std::string stamp() {
    const time_t t = std::time(nullptr);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[32];
    snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

void exportAll(App& a, const std::vector<SmartPacket>& v) {
    const std::string dir = plat::dataDir() + "/cubesat";
    std::error_code ec;
    fs::create_directories(dir, ec);
    double lat, lon;
    receiverLocation(lat, lon);
    const std::string s = stamp();
    auto put = [&](const std::string& name, const std::string& body) { std::ofstream f(dir + "/" + name, std::ios::binary); f << body; };
    put("cubesat-" + s + "-satnogs.jsonl", exportSatnogsJson(v, "OnAir", lat, lon));
    put("cubesat-" + s + ".csv", exportCsv(v));
    put("cubesat-" + s + ".kiss", exportKiss(v));
    S.note = std::string(tr("Saved to")) + " " + dir + " (" + std::to_string(v.size()) + " " + tr("packets") + "). " + tr("Nothing was uploaded.");
    a.engine.log("Smart CubeSat: exported to " + dir);
}

void kv(const App& a, const char* k, const std::string& v) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

const ImVec4 stateColour(const std::string& s) {
    if (s == "decoding") return pal::okGreen();
    if (s == "no known signal" || s == "gone") return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

const char* stateText(const std::string& s) {
    if (s == "searching") return tr("searching");
    if (s == "carrier found") return tr("carrier found");
    if (s == "analysing") return tr("identifying");
    if (s == "decoding") return tr("decoding");
    if (s == "carrier only") return tr("carrier only");
    if (s == "no known signal") return tr("no known signal");
    return tr("searching");
}

void passesTable() {
    std::vector<PlannedPass> ps;
    { std::lock_guard<std::mutex> lk(S.mu); ps = S.passes; }
    ImGui::TextDisabled("%s", tr("Next passes"));
    if (ps.empty()) { ImGui::TextDisabled("%s", S.busy.load() ? tr("working out the passes ...") : tr("no pass of a satellite in the table in the next hours (set your location, refresh the orbits)")); return; }
    if (ImGui::BeginTable("##csm_passes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("In"));
        ImGui::TableSetupColumn(tr("Max el"));
        ImGui::TableSetupColumn(tr("Signal"));
        ImGui::TableHeadersRow();
        const double now = nowUnix();
        const std::vector<TxEntry> tx = {};
        (void)tx;
        int shown = 0;
        for (const PlannedPass& p : ps) {
            if (p.p.los < now) continue;
            if (shown++ >= 8) break;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(inText(p.p.aos - now).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f\xC2\xB0", p.p.maxElDeg);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%zu", p.tx.size());
        }
        ImGui::EndTable();
    }
}

void carriersTable(App& a, const SmartStatus& st) {
    ImGui::TextDisabled("%s", tr("Carriers"));
    if (st.carriers.empty()) { ImGui::TextDisabled("%s", tr("none yet")); return; }
    if (ImGui::BeginTable("##csm_car", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Frequency"));
        ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Signal"));
        ImGui::TableSetupColumn(tr("SNR"));
        ImGui::TableSetupColumn(tr("Status"));
        ImGui::TableSetupColumn(tr("Packets"));
        ImGui::TableHeadersRow();
        for (const SmartCarrier& c : st.carriers) {
            ImGui::PushID(c.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%.4f", c.freqHz / 1e6); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.sat.empty() ? tr("unknown") : c.sat.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.modText.c_str());
            ImGui::TableNextColumn(); if (c.snrDb > 0) ImGui::Text("%.0f dB", c.snrDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, stateColour(c.state)); ImGui::TextUnformatted(stateText(c.state)); ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.frames);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // the trials of the first carrier that has any (which framing is being tried and what validated)
    for (const SmartCarrier& c : st.carriers) {
        if (c.trials.empty() || c.state == "searching") continue;
        ImGui::PushID(c.id);
        char tl[64];
        snprintf(tl, sizeof tl, "%.4f MHz", c.freqHz / 1e6);
        ImGui::TextDisabled("%s", tr("Tried on"));
        ImGui::SameLine();
        if (ImGui::TreeNode(tl)) {
            for (const SmartTrial& t : c.trials) {
                if (!t.valid && !t.heard && &t - c.trials.data() > 6) continue;
                ImGui::TextDisabled("%s", t.label.c_str()); ImGui::SameLine();
                ImGui::Text("%llu %s, %llu %s", (unsigned long long)t.valid, tr("valid"), (unsigned long long)t.heard, tr("failed their checks"));
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void packetDetail(const App& a, const SmartPacket& p) {
    kv(a, tr("satellite"), (p.sat.empty() ? std::string(tr("unknown")) : p.sat) + (p.satGuess ? " (?)" : "") + (p.norad ? " (#" + std::to_string(p.norad) + ")" : ""));
    kv(a, tr("signal"), modulationTextUi(p));
    kv(a, tr("framing"), p.framing + ", " + tr("validated by") + " " + p.how + (p.rsErrors > 0 ? ", " + std::to_string(p.rsErrors) + " " + tr("byte errors put right") : ""));
    char b[96];
    snprintf(b, sizeof b, "%.4f MHz, SNR %.0f dB", p.freqHz / 1e6, p.snrDb);
    kv(a, tr("received"), clockText((double)p.wallTime) + ", " + b);
    if (!p.from.empty()) kv(a, "AX.25", p.from + " > " + p.to + (p.path.empty() ? "" : " via " + p.path) + ", " + p.type);
    if (p.hasCsp) { snprintf(b, sizeof b, "%d:%d > %d:%d", p.csp.src, p.csp.sport, p.csp.dst, p.csp.dport); kv(a, "CSP", b); }
    if (!p.tel.fields.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", (p.tel.kind.empty() ? std::string(tr("telemetry")) : p.tel.kind).c_str());
        if (ImGui::BeginTable("##csm_tel", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, std::min(260.f * gUi, ImGui::GetContentRegionAvail().y)))) {
            ImGui::TableSetupColumn(tr("Group")); ImGui::TableSetupColumn(tr("Field"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Value"));
            for (const TelField& f : p.tel.fields) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.group.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.name.c_str());
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.text.c_str()); ImGui::PopFont();
            }
            ImGui::EndTable();
        }
    } else if (!p.text.empty()) {
        ImGui::Spacing();
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(p.text.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    if (!p.data.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("hex (%zu)", p.data.size());
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string(tr("Copy")) + "##csm_copy").c_str())) ImGui::SetClipboardText(hexDump(p.data).c_str());
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(hexDump(p.data).c_str()); ImGui::PopFont();
    }
}

void packetsTable(App& a, const std::vector<SmartPacket>& v) {
    ImGui::TextDisabled("%s", tr("Decoded packets"));
    if (v.empty()) { ImGui::TextDisabled("%s", tr("nothing decoded yet")); return; }
    const float h = ImGui::GetContentRegionAvail().y;
    const float listH = S.selKey ? h * 0.4f : h;
    if (ImGui::BeginTable("##csm_pk", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Framing")); ImGui::TableSetupColumn(tr("Bytes"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < v.size() && i < 400; i++) {
            const SmartPacket& p = v[i];
            const uint64_t key = (uint64_t)(p.timeSec * 1000.0) * 31u + p.data.size();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(clockText((double)p.wallTime).c_str(), S.selKey == key, ImGuiSelectableFlags_SpanAllColumns)) S.selKey = S.selKey == key ? 0 : key;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.sat.empty() ? tr("unknown") : (p.sat + (p.satGuess ? " (?)" : "")).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.framing.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%zu", p.data.size());
        }
        ImGui::EndTable();
    }
    if (S.selKey) for (const SmartPacket& p : v) if ((uint64_t)(p.timeSec * 1000.0) * 31u + p.data.size() == S.selKey) { ImGui::Separator(); packetDetail(a, p); break; }
}

void advanced(App& a) {
    if (!ImGui::CollapsingHeader(trw("Advanced"))) return;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Follow satellites from the table (passes and Doppler)##csm_pass"), &S.cfg.usePasses);
    ch |= ImGui::Checkbox(tr("Look for carriers the table does not explain##csm_blind"), &S.cfg.blind);
    float snr = (float)S.cfg.minSnrDb;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    if (ImGui::SliderFloat(tr("Minimum SNR (dB)##csm_snr"), &snr, 3.f, 20.f, "%.0f")) { S.cfg.minSnrDb = snr; ch = true; }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputInt(tr("Carriers followed at once##csm_nch"), &S.cfg.maxChannels);
    S.cfg.maxChannels = std::max(1, S.cfg.maxChannels);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputDouble(tr("Lowest elevation (deg)##csm_el"), &S.cfg.minElDeg, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputDouble(tr("Search window around the predicted carrier (Hz)##csm_sw"), &S.cfg.searchHz, 0, 0, "%.0f");
    static const char* const kMods[] = {N_("automatic"), "CW / OOK", "AFSK", "FSK / GMSK", "BPSK"};
    int m = S.cfg.forceMod >= 0 && S.cfg.forceMod <= ModBpsk ? S.cfg.forceMod : 0;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    if (ImGui::BeginCombo(tr("Modulation of unknown carriers##csm_mod"), tr(kMods[m]))) {
        for (int i = 0; i <= ModBpsk; i++) if (ImGui::Selectable(tr(kMods[i]), i == m)) { S.cfg.forceMod = i; ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputInt(tr("Baud rate (0: automatic, any value)##csm_baud"), &S.cfg.forceBaud);
    S.cfg.forceBaud = std::max(0, S.cfg.forceBaud);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputDouble(tr("Channel width (Hz, 0: automatic, any value)##csm_cw"), &S.cfg.channelHz, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ch |= ImGui::InputDouble(tr("Scan resolution (Hz, 0: automatic)##csm_sb"), &S.cfg.scanBinHz, 0, 0, "%.0f");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Add a transmitter to the table"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputText(tr("Name##csm_nn"), S.newName, sizeof S.newName);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::InputInt(tr("NORAD number##csm_nd"), &S.newNorad);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::InputDouble(tr("Frequency (MHz)##csm_nf"), &S.newMhz, 0, 0, "%.4f");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    if (ImGui::BeginCombo(tr("Modulation##csm_nm"), tr(kMods[std::max(1, std::min((int)ModBpsk, S.newMod))]))) {
        for (int i = 1; i <= ModBpsk; i++) if (ImGui::Selectable(tr(kMods[i]), i == S.newMod)) S.newMod = i;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::InputInt(tr("Baud rate##csm_nb"), &S.newBaud);
    if (ImGui::Button(tr("Add##csm_add")) && S.newNorad > 0 && S.newMhz > 0) {
        TxEntry t; t.norad = S.newNorad; t.name = S.newName; t.hz = S.newMhz * 1e6; t.mod = S.newMod; t.baud = std::max(0, S.newBaud); t.desc = "user"; t.mode = "user";
        S.userTx.push_back(t);
        ch = true;
    }
    for (size_t i = 0; i < S.userTx.size(); i++) {
        ImGui::PushID((int)i + 1000);
        ImGui::TextDisabled("%s  %.4f MHz  %s %d", S.userTx[i].name.c_str(), S.userTx[i].hz / 1e6, modName(S.userTx[i].mod), S.userTx[i].baud);
        ImGui::SameLine();
        if (ImGui::SmallButton("x##csm_del")) { S.userTx.erase(S.userTx.begin() + (long)i); ch = true; ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ch) saveCfg();
    ImGui::TextDisabled(tr("orbits for %zu satellites, %zu table entries"), a.engine.smartCubeDecoder().satelliteCount(), a.engine.smartCubeDecoder().transmitters().size());
}

} // namespace

std::string modulationTextUi(const SmartPacket& p) {
    char b[64];
    switch (p.mod) {
    case ModCw: return "CW / OOK";
    case ModAfsk: return "AFSK 1200";
    case ModFsk: snprintf(b, sizeof b, "FSK / GMSK %d", p.baud); return b;
    case ModBpsk: snprintf(b, sizeof b, "BPSK %d", p.baud); return b;
    default: return "?";
    }
}

void cubesatSmartView(App& a) {
    load();
    SmartCubesat& sm = a.engine.smartCubeDecoder();
    feedKnowledge(a);
    refreshPasses(a);
    const bool on = a.engine.smartCube();
    ImGui::PushID("##csm_main");
    ImVec4 bg = on ? pal::accent(0.55f) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    const bool hit = ImGui::Button(on ? tr("Auto decode: on (click to stop)") : tr("Auto decode cubesats"));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tunes a span that holds the satellites about to pass, follows their Doppler shift, finds every narrow carrier, works out its modulation and baud rate, and decodes it with every framing that fits. Nothing to set up."));
    ImGui::PopID();
    if (hit) {
        if (on) a.engine.setSmartCube(false);
        else startAuto(a);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Refresh orbits##csm_tle"))) { passesDownloadTles(); S.lastTleLoad = -1e9; S.note = tr("Downloading orbits from CelesTrak (the built-in set is used meanwhile)."); }
    ImGui::SameLine();
    const std::vector<SmartPacket> pk = sm.packets();
    if (ImGui::SmallButton(tr("Export##csm_exp"))) exportAll(a, pk);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Writes the packets as a SatNOGS-ready file (one telemetry record per line), a CSV and a KISS file to the data folder. Nothing is uploaded."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear##csm_clr"))) { sm.clearPackets(); S.selKey = 0; }
    const SmartStatus st = sm.status();
    ImGui::AlignTextToFramePadding();
    if (!on) ImGui::TextDisabled("%s", tr("Press the button: it does the rest."));
    else {
        const char* w = st.state == 2 ? tr("decoding") : st.state == 1 ? tr("carrier found") : tr("searching");
        ImGui::Text("%s: %s%s%s", tr("State"), w, st.note.empty() ? "" : ", ", st.note.c_str());
    }
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    if (on && !a.engine.running()) ImGui::TextDisabled("%s", tr("Start the receiver to begin."));
    ImGui::Spacing();
    passesTable();
    ImGui::Spacing();
    if (on) { carriersTable(a, st); ImGui::Spacing(); }
    packetsTable(a, pk);
    ImGui::Spacing();
    advanced(a);
}
