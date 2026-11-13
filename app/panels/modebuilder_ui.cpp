// Tools > Mode Builder: a decoder built without code. The user describes a protocol (form or JSON, docs/MODE-BUILDER.md) and the
// receiver in core/src/modebuilder runs it on a recording, on a test signal made from the same description, or live as a mode of the
// mode list (group CUSTOM MODES, receiver 1). This file is the window, the custom modes' main tab and the live plumbing; the
// receiver, generator and the JSON are in the library dect2_modebuilder.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../ui/theme.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/protolab/protolab.h"
#include "dect2/source/source.h"
#include "platform/platform.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
namespace mb = dect2::modebuilder;
namespace fs = std::filesystem;

constexpr size_t kMaxFrames = 5000;

std::string modesDir() { return plat::dataDir() + "/modes"; }

int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}
bool inputString(const char* label, std::string& s, float w = 0) {
    if (w > 0) ImGui::SetNextItemWidth(w);
    return ImGui::InputText(label, s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}

bool saveText(const char* suggest, const std::string& text, std::string& msg) {
    const std::string p = saveFileDialog(suggest);
    if (p.empty()) return false;
    std::string err;
    if (!mb::writeText(p, text, err)) { msg = err; return false; }
    msg = std::string(tr("Saved")) + " " + p;
    return true;
}

// ---------------------------------------------------------------- registered custom modes
struct Registered { int family = 0; std::string id, name; };
struct Registry {
    bool loaded = false;
    std::vector<Registered> list;
    std::string file() const { return modesDir() + "/registered.txt"; }
    void load() {
        loaded = true;
        list.clear();
        std::string text, err;
        if (!mb::readText(file(), text, err)) return;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            Registered r;
            if (!(ls >> r.family >> r.id)) continue;
            if (!isCustomFamily(r.family)) continue;
            std::getline(ls, r.name);
            while (!r.name.empty() && r.name[0] == ' ') r.name.erase(0, 1);
            list.push_back(r);
        }
    }
    void save() const {
        std::string t = "# OnAir custom modes (Mode Builder): family id name; the definitions are <id>.json next to this file\n";
        for (const Registered& r : list) t += std::to_string(r.family) + " " + r.id + " " + r.name + "\n";
        std::string err;
        mb::writeText(file(), t, err);
    }
    const Registered* byFamily(int f) const { for (const auto& r : list) if (r.family == f) return &r; return nullptr; }
    const Registered* byId(const std::string& id) const { for (const auto& r : list) if (r.id == id) return &r; return nullptr; }
    int nextFamily() const { int f = kCustomFamilyBase; for (const auto& r : list) f = std::max(f, r.family + 1); return f; }
};
Registry R;

void addRow(const Registered& r, const mb::Definition& d) {
    std::string blurb = d.description.empty() ? r.name : d.description;
    if (blurb.size() > 200) blurb.resize(200);
    char sub[96];
    std::snprintf(sub, sizeof sub, "%s %d, %.0f Bd", mb::modName(d.mod), d.levels(), d.symbolRate);
    addCustomModeRow(r.family, r.name, blurb, sub);
    const double rate = std::max(d.sampleRate, std::max(4 * d.symbolRate, 2 * d.bandwidth()));
    dect2::registerPluginTuning(r.family + 2, "custom-" + r.id, r.name, 0.001, 300000, d.freqMhz, rate, std::max(0.001, d.bandwidth() / 1e6));
}

bool loadRegistered(const Registered& r, mb::Definition& d, std::string& err) {
    std::string text;
    if (!mb::readText(modesDir() + "/" + r.id + ".json", text, err)) return false;
    const mb::ParseResult p = mb::parseDefinition(text);
    if (!p.ok) { for (const mb::Issue& i : p.issues) if (i.error) { err = i.text(); break; } return false; }
    d = p.def;
    return true;
}

// ---------------------------------------------------------------- the live receiver of a custom mode (receiver 1)
struct Live {
    std::mutex mu;                                 // the sink (analysis thread) and the window
    std::unique_ptr<mb::Receiver> rx;
    double rate = 0;
    int family = 0;
    std::vector<mb::cf> buf;
};
Live L;

struct LiveView {
    int family = 0;
    mb::Definition def;
    std::string error;
    std::deque<mb::Frame> frames;
    mb::RxStats stats;
    std::string msg;
    mb::SignalView sig;            // the Signal view: filled only while its tab is on screen
    bool wantSig = false;
};
LiveView V;

// ---------------------------------------------------------------- the window
struct State {
    bool inited = false;
    std::string text;                              // the JSON being edited
    std::string lastParsed;
    mb::ParseResult parsed;
    mb::Definition def;                            // the form's copy (last valid parse)
    bool formDirty = false;
    int tab = 0;
    std::string msg;
    // recording
    char path[512] = "";
    int fmt = 0;
    double rate = 0, offset = 0;
    // test signal
    int genFrames = 8;
    float genSnr = 20, genCfo = 0, genPpm = 50;
    // worker
    std::thread worker;
    std::atomic<int> run{0};                       // 0 idle, 1 running, 2 done
    std::mutex mu;
    std::string workMsg;
    std::vector<mb::Frame> result;
    mb::Definition resultDef;
    bool haveResult = false;
    mb::RxStats resultStats;
    bool openRequest = false;
    // library
    std::vector<mb::Saved> saved;
    bool savedListed = false;
    ~State() { if (worker.joinable()) worker.join(); }
};
State S;

void setText(const std::string& t) { S.text = t; S.lastParsed.clear(); }

void reparse() {
    if (S.text == S.lastParsed) return;
    S.lastParsed = S.text;
    S.parsed = mb::parseDefinition(S.text);
    if (S.parsed.ok) S.def = S.parsed.def;
}

void init() {
    S.inited = true;
    const auto& ex = mb::examples();
    setText(ex.empty() ? std::string() : ex.front().text);
    reparse();
}

void startWork(std::function<void()> job) {
    if (S.worker.joinable()) S.worker.join();
    S.run = 1;
    { std::lock_guard<std::mutex> lk(S.mu); S.workMsg = tr("Decoding ..."); }
    S.worker = dect2::namedThread("onair-modebuilder", [job] {
        try { job(); } catch (const std::exception& e) { std::lock_guard<std::mutex> lk(S.mu); S.workMsg = e.what(); }
        S.run = 2;
    });
}

void setResult(const mb::Definition& d, std::vector<mb::Frame>&& f, const mb::RxStats& st, const std::string& msg) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.resultDef = d;
    S.result = std::move(f);
    S.resultStats = st;
    S.haveResult = true;
    S.workMsg = msg;
}

void testRecording() {
    const mb::Definition d = S.def;
    const std::string path = S.path;
    static const dect2::protolab::IqFormat fmts[] = {dect2::protolab::IqFormat::Cf32, dect2::protolab::IqFormat::Cs16, dect2::protolab::IqFormat::Cu8, dect2::protolab::IqFormat::Cs8};
    const auto f = fmts[std::clamp(S.fmt, 0, 3)];
    const double rate = S.rate > 0 ? S.rate : d.sampleRate, off = S.offset;
    startWork([=] {
        std::vector<std::complex<float>> iq;
        std::string err;
        if (!dect2::protolab::loadIq(path, f, iq, err, 60000000)) { std::lock_guard<std::mutex> lk(S.mu); S.workMsg = err; return; }
        mb::RxStats st;
        std::vector<mb::Frame> fr = mb::decodeIq(d, iq, rate, off, &st);
        char b[160];
        std::snprintf(b, sizeof b, tr("%zu frames from %.1f s of recording (%llu sync words, %llu symbols)"), fr.size(), iq.size() / rate, (unsigned long long)st.syncHits, (unsigned long long)st.symbols);
        setResult(d, std::move(fr), st, b);
    });
}

void testGenerated(bool save) {
    const mb::Definition d = S.def;
    mb::GenOptions g;
    g.snrDb = S.genSnr;
    g.cfoHz = S.genCfo;
    g.clockPpm = S.genPpm;
    g.seed = (uint32_t)std::time(nullptr);
    const int n = std::clamp(S.genFrames, 1, 500);
    std::string savePath;
    if (save) {
        char name[96];
        std::snprintf(name, sizeof name, "%s_%.0fsps.cf32", d.id.c_str(), d.sampleRate);
        savePath = saveFileDialog(name);
        if (savePath.empty()) return;
    }
    startWork([=] {
        uint32_t rng = g.seed | 1;
        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < n; i++) frames.push_back(mb::randomFrame(d, rng));
        const std::vector<mb::cf> iq = mb::generate(d, frames, g);
        if (!savePath.empty()) {
            FILE* fp = dect2::u8fopen(savePath, "wb");
            const bool ok = fp && std::fwrite(iq.data(), sizeof(mb::cf), iq.size(), fp) == iq.size();
            if (fp) std::fclose(fp);
            std::lock_guard<std::mutex> lk(S.mu);
            S.workMsg = ok ? std::string(tr("Wrote")) + " " + savePath + " (complex float32, " + std::to_string((long long)d.sampleRate) + " Hz)" : std::string(tr("Cannot write")) + " " + savePath;
            return;
        }
        mb::RxStats st;
        std::vector<mb::Frame> fr = mb::decodeIq(d, iq, d.sampleRate, 0, &st);
        int match = 0;
        for (const mb::Frame& f : fr) for (const auto& s : frames) if (f.good() && s == f.bytes) { match++; break; }
        char b[200];
        std::snprintf(b, sizeof b, tr("Test signal: %d frames sent, %d decoded correctly, %zu listed (SNR %.0f dB, offset %.0f Hz, clock %.0f ppm)"), n, match, fr.size(), g.snrDb, g.cfoHz, g.clockPpm);
        setResult(d, std::move(fr), st, b);
    });
}

// ---------------------------------------------------------------- the frame table (window and mode tab)
template <class C>
void framesTable(const char* id, const mb::Definition& d, const C& frames, float height) {
    const int nf = std::min<int>((int)d.fields.size(), 48);
    const int cols = 6 + nf;
    const ImGuiTableFlags fl = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, cols, fl, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time (s)"));
    ImGui::TableSetupColumn(tr("Check"));
    ImGui::TableSetupColumn(tr("SNR"));
    ImGui::TableSetupColumn(tr("Sync err"));
    ImGui::TableSetupColumn(tr("FEC fixed"));
    for (int i = 0; i < nf; i++) {
        const mb::Field& f = d.fields[(size_t)i];
        const std::string h = f.units.empty() ? f.name : f.name + " (" + f.units + ")";
        ImGui::TableSetupColumn(h.c_str());
    }
    ImGui::TableSetupColumn(tr("Display"));
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)frames.size());
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const mb::Frame& f = frames[frames.size() - 1 - (size_t)r];   // newest first
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", f.timeSec);
            if (ImGui::IsItemHovered()) {
                std::string hx;
                static const char* H = "0123456789ABCDEF";
                for (uint8_t b : f.bytes) { hx += H[b >> 4]; hx += H[b & 15]; hx += ' '; }
                ImGui::SetTooltip("%s", hx.c_str());
            }
            ImGui::TableNextColumn();
            if (!f.hasCrc) ImGui::TextDisabled("-");
            else if (f.good()) ImGui::TextColored(pal::okGreen(), "%s", tr("ok"));
            else ImGui::TextColored(pal::badRed(), "%s", f.fecFailed ? tr("FEC") : tr("bad"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", f.snrDb);
            ImGui::TableNextColumn();
            ImGui::Text("%d", f.syncErrors);
            ImGui::TableNextColumn();
            ImGui::Text("%d", f.fecCorrected);
            for (int i = 0; i < nf; i++) {
                ImGui::TableNextColumn();
                if ((size_t)i < f.fields.size()) ImGui::TextUnformatted(f.fields[(size_t)i].text.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.display.c_str());
        }
    }
    ImGui::EndTable();
}

template <class C>
void exportButtons(const mb::Definition& d, const C& frames, std::string& msg) {
    std::vector<mb::Frame> v(frames.begin(), frames.end());
    ImGui::BeginDisabled(v.empty());
    if (ImGui::Button(tr("Export CSV..."))) saveText((d.id + "-frames.csv").c_str(), mb::framesCsv(d, v), msg);
    ImGui::SameLine();
    if (ImGui::Button(tr("Export JSON..."))) saveText((d.id + "-frames.json").c_str(), mb::framesJson(d, v), msg);
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------- the form
template <class E>
bool combo(const char* label, E& v, const char* const* names, int n, float w) {
    int i = (int)v;
    ImGui::SetNextItemWidth(w);
    if (!ImGui::Combo(label, &i, names, n)) return false;
    v = (E)i;
    return true;
}

// a text field showing a value that the user edits as text: while it is being typed the text stays as typed
bool liveText(const char* label, const std::string& cur, std::string& out, float w) {
    struct E { std::string t; bool active = false; };
    static std::map<ImGuiID, E> m;
    E& e = m[ImGui::GetID(label)];
    if (!e.active) e.t = cur;
    const bool ch = inputString(label, e.t, w);
    e.active = ImGui::IsItemActive();
    if (ch) out = e.t;
    return ch;
}

bool patternInput(const char* label, mb::BitPattern& p, float w) {
    std::string t;
    bool changed = false;
    if (liveText(label, mb::patternText(p), t, w)) {
        mb::BitPattern np;
        std::string err;
        if (mb::parsePattern(t, 0, np, err)) { p = np; changed = true; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("\"0x2DD4\" (hex, 4 bits per digit), \"0b1011\" or \"1011\" (bits)"));
    return changed;
}

std::string tapsText(const std::vector<int>& t) { std::string s; for (size_t i = 0; i < t.size(); i++) s += (i ? "," : "") + std::to_string(t[i]); return s; }
std::vector<int> parseTaps(const std::string& s) {
    std::vector<int> v;
    int x = 0;
    bool any = false;
    for (char c : s + ",") {
        if (c >= '0' && c <= '9') { x = x * 10 + (c - '0'); any = true; }
        else if (any) { if (x >= 1 && x <= 32) v.push_back(x); x = 0; any = false; }
    }
    return v;
}
std::string tableText(const std::vector<std::pair<int64_t, std::string>>& t) { std::string s; for (const auto& e : t) s += (s.empty() ? "" : ", ") + std::to_string(e.first) + "=" + e.second; return s; }
std::string flagsText(const std::vector<std::pair<int, std::string>>& t) { std::string s; for (const auto& e : t) s += (s.empty() ? "" : ", ") + std::to_string(e.first) + "=" + e.second; return s; }
template <class K>
std::vector<std::pair<K, std::string>> parseTable(const std::string& s) {
    std::vector<std::pair<K, std::string>> v;
    std::istringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string k = item.substr(0, eq), n = item.substr(eq + 1);
        while (!k.empty() && k.front() == ' ') k.erase(0, 1);
        while (!n.empty() && n.front() == ' ') n.erase(0, 1);
        while (!n.empty() && n.back() == ' ') n.pop_back();
        char* e = nullptr;
        const long long kv = std::strtoll(k.c_str(), &e, 0);
        if (e == k.c_str() || n.empty()) continue;
        v.push_back({(K)kv, n});
    }
    return v;
}

bool formView(mb::Definition& d) {
    bool ch = false;
    const float w = 150 * gUi, wide = 340 * gUi;
    auto num = [&](const char* label, double& v, const char* fmt = "%.6g") { ImGui::SetNextItemWidth(w); if (ImGui::InputDouble(label, &v, 0, 0, fmt)) ch = true; };
    auto inum = [&](const char* label, int& v) { ImGui::SetNextItemWidth(w); if (ImGui::InputInt(label, &v)) ch = true; };
    auto chk = [&](const char* label, bool& v) { if (ImGui::Checkbox(label, &v)) ch = true; };
    if (ImGui::CollapsingHeader(trw("Mode"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (inputString(tr("Name"), d.name, wide)) { ch = true; }
        if (inputString(tr("Id (file name)"), d.id, wide)) ch = true;
        if (inputString(tr("Description"), d.description, wide)) ch = true;
        num(tr("Frequency (MHz)"), d.freqMhz, "%.6f");
        num(tr("Sample rate (Hz)"), d.sampleRate, "%.0f");
    }
    if (ImGui::CollapsingHeader(trw("Modulation"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* mods[] = {"OOK", "ASK", "FSK", "GFSK", "GMSK", "PSK"};
        if (combo(tr("Type"), d.mod, mods, 6, w)) ch = true;
        if (d.mod == mb::Mod::FSK || d.mod == mb::Mod::GFSK || d.mod == mb::Mod::PSK || d.mod == mb::Mod::ASK) {
            int oi = d.order >= 8 ? 2 : d.order >= 4 ? 1 : 0;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo(tr("Levels"), &oi, "2\0" "4\0" "8\0")) { d.order = oi == 2 ? 8 : oi == 1 ? 4 : 2; ch = true; }
        }
        num(tr("Symbol rate (Bd)"), d.symbolRate, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Symbols on the air per second; with Manchester and the other line codes this counts the chips"));
        if (d.mod == mb::Mod::FSK || d.mod == mb::Mod::GFSK) num(tr("Deviation (Hz, 0 = symbol rate / 2)"), d.deviationHz, "%.1f");
        if (d.mod == mb::Mod::GFSK || d.mod == mb::Mod::GMSK) num(tr("Gaussian BT"), d.bt, "%.2f");
        if (d.mod == mb::Mod::PSK) chk(tr("Differential (DPSK)"), d.differential);
        if (d.levels() > 2) chk(tr("Gray-coded symbols"), d.gray);
        num(tr("Offset from the tuned frequency (Hz)"), d.offsetHz, "%.1f");
        num(tr("Filter bandwidth (Hz, 0 = automatic)"), d.bandwidthHz, "%.0f");
        static const char* pulses[] = {"none", "rrc", "rc", "gaussian"};
        mb::Pulse p = d.pulseShape();
        if (combo(tr("Pulse shape"), p, pulses, 4, w)) { d.pulse = p; d.pulseSet = true; ch = true; }
        if (p == mb::Pulse::Rrc || p == mb::Pulse::Rc) num(tr("Roll-off"), d.rolloff, "%.2f");
    }
    if (ImGui::CollapsingHeader(trw("Bits"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* lines[] = {"NRZ", "NRZI", "Manchester", "Manchester-Thomas", "Differential-Manchester", "Miller", "4b5b", "8b10b"};
        if (combo(tr("Line code"), d.line, lines, 8, w)) ch = true;
        if (d.line == mb::Line::NRZI) { bool m = d.nrziToggleOn == 1; if (ImGui::Checkbox(tr("A 1 changes the level (NRZ-M)"), &m)) { d.nrziToggleOn = m ? 1 : 0; ch = true; } }
        static const char* pols[] = {"normal", "inverted", "auto"};
        if (combo(tr("Polarity"), d.polarity, pols, 3, w)) ch = true;
        int bo = d.lsbFirst ? 1 : 0;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo(tr("Bit order"), &bo, "MSB first\0LSB first\0")) { d.lsbFirst = bo == 1; ch = true; }
        if (patternInput(tr("Preamble"), d.preamble, w)) ch = true;
        if (patternInput(tr("Sync word"), d.sync, w)) ch = true;
        inum(tr("Sync bit errors allowed"), d.syncMaxErrors);
        static const char* scr[] = {"none", "additive", "multiplicative"};
        if (combo(tr("Scrambler"), d.scrambler, scr, 3, w)) ch = true;
        if (d.scrambler != mb::Scrambler::None) {
            std::string taps;
            if (liveText(tr("Taps (e.g. 9,5)"), tapsText(d.taps), taps, w)) { d.taps = parseTaps(taps); std::sort(d.taps.begin(), d.taps.end(), std::greater<int>()); ch = true; }
            int seed = (int)d.seed;
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("Seed"), &seed, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) { d.seed = (uint32_t)seed; ch = true; }
        }
    }
    if (ImGui::CollapsingHeader(trw("Frame"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* lm[] = {"fixed length", "length field", "end flag"};
        if (combo(tr("Length"), d.lenMode, lm, 3, w)) ch = true;
        if (d.lenMode == mb::LenMode::Fixed) inum(tr("Bytes"), d.fixedBytes);
        else if (d.lenMode == mb::LenMode::LengthField) {
            inum(tr("Field offset (bits)"), d.lenOffsetBits);
            inum(tr("Field width (bits)"), d.lenWidthBits);
            num(tr("Multiplier"), d.lenMultiplier);
            inum(tr("Adjust (bytes)"), d.lenAdjust);
            inum(tr("Shortest (bytes)"), d.minBytes);
            inum(tr("Longest (bytes)"), d.maxBytes);
        } else {
            if (patternInput(tr("End flag"), d.flag, w)) ch = true;
            chk(tr("HDLC bit stuffing"), d.bitStuffing);
            inum(tr("Shortest (bytes)"), d.minBytes);
            inum(tr("Longest (bytes)"), d.maxBytes);
        }
        static const char* fecs[] = {"none", "repetition", "Hamming", "convolutional", "Reed-Solomon"};
        if (combo(tr("FEC"), d.fec, fecs, 5, w)) ch = true;
        if (d.fec == mb::Fec::Repetition) inum(tr("Repeats"), d.repN);
        if (d.fec == mb::Fec::Hamming) {
            static const char* codes[] = {"7,4", "15,11", "31,26", "8,4", "16,11"};
            static const int ns[] = {7, 15, 31, 8, 16}, ks[] = {4, 11, 26, 4, 11};
            int ci = 0;
            for (int i = 0; i < 5; i++) if (ns[i] == d.hamN && ks[i] == d.hamK) ci = i;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo(tr("Code"), &ci, codes, 5)) { d.hamN = ns[ci]; d.hamK = ks[ci]; d.hamExtended = ci >= 3; ch = true; }
        }
        if (d.fec == mb::Fec::Convolutional) {
            inum(tr("Constraint length K"), d.convK);
            std::string cur, polys;
            for (uint32_t g : d.convPolys) { char b[16]; std::snprintf(b, sizeof b, "%s0%o", cur.empty() ? "" : ",", g); cur += b; }
            if (liveText(tr("Polynomials (octal, e.g. 0171,0133)"), cur, polys, w)) {
                std::vector<uint32_t> g;
                std::istringstream in(polys);
                std::string it;
                while (std::getline(in, it, ',')) { char* e = nullptr; const unsigned long v = std::strtoul(it.c_str(), &e, 8); if (e != it.c_str() && v) g.push_back((uint32_t)v); }
                if (g.size() == 2 || g.size() == 3) { d.convPolys = g; ch = true; }
            }
            chk(tr("Tail bits"), d.convTail);
        }
        if (d.fec == mb::Fec::ReedSolomon) {
            inum("n", d.rsN);
            inum("k", d.rsK);
            inum(tr("Symbol bits"), d.rsM);
            int poly = (int)d.rsPoly;
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("Field polynomial (hex)"), &poly, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) { d.rsPoly = (uint32_t)poly; ch = true; }
            inum(tr("First root"), d.rsFcr);
            inum(tr("Root spacing"), d.rsPrim);
        }
        inum(tr("Interleaver rows"), d.ilRows);
        inum(tr("Interleaver columns"), d.ilCols);
    }
    if (ImGui::CollapsingHeader(trw("Check value"), ImGuiTreeNodeFlags_DefaultOpen)) {
        std::vector<std::string> names = mb::crcPresetNames();
        names.insert(names.begin(), "none");
        names.push_back("custom");
        std::string cur = d.crcKind == mb::CrcKind::None ? "none" : d.crcKind == mb::CrcKind::Sum8 ? "SUM8" : d.crcKind == mb::CrcKind::Xor8 ? "XOR8" : d.crc.name.empty() ? "custom" : d.crc.name;
        ImGui::SetNextItemWidth(w * 1.4f);
        if (ImGui::BeginCombo(tr("Model"), cur.c_str())) {
            for (const std::string& n : names) {
                if (!ImGui::Selectable(n.c_str(), n == cur)) continue;
                ch = true;
                if (n == "none") d.crcKind = mb::CrcKind::None;
                else if (n == "SUM8") d.crcKind = mb::CrcKind::Sum8;
                else if (n == "XOR8") d.crcKind = mb::CrcKind::Xor8;
                else if (n == "custom") { d.crcKind = mb::CrcKind::Crc; d.crc.name.clear(); }
                else { d.crcKind = mb::CrcKind::Crc; mb::crcPreset(n, d.crc); }
            }
            ImGui::EndCombo();
        }
        if (d.crcKind == mb::CrcKind::Crc) {
            int wd = d.crc.width, po = (int)d.crc.poly, in = (int)d.crc.init, xo = (int)d.crc.xorout;
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("Width (bits)"), &wd)) { d.crc.width = std::clamp(wd, 1, 32); d.crc.name.clear(); ch = true; }
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("Polynomial"), &po, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) { d.crc.poly = (uint32_t)po; d.crc.name.clear(); ch = true; }
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("Init"), &in, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) { d.crc.init = (uint32_t)in; d.crc.name.clear(); ch = true; }
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt(tr("XOR out"), &xo, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) { d.crc.xorout = (uint32_t)xo; d.crc.name.clear(); ch = true; }
            if (ImGui::Checkbox(tr("Reflect in"), &d.crc.refin)) { d.crc.name.clear(); ch = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("Reflect out"), &d.crc.refout)) { d.crc.name.clear(); ch = true; }
        }
        if (d.crcKind != mb::CrcKind::None) {
            inum(tr("First byte covered"), d.crcFrom);
            int bo = d.crcLittle ? 1 : 0;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo(tr("Byte order"), &bo, "big endian\0little endian\0")) { d.crcLittle = bo == 1; ch = true; }
            chk(tr("List frames with a wrong check value"), d.showBadFrames);
        }
    }
    if (ImGui::CollapsingHeader(trw("Fields"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* types[] = {"uint", "int", "bool", "enum", "bitflags", "ascii", "bcd", "float32", "lat", "lon", "hex"};
        int remove = -1;
        if (ImGui::BeginTable("##mbfields", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn(tr("Name"));
            ImGui::TableSetupColumn(tr("Offset"));
            ImGui::TableSetupColumn(tr("Width"));
            ImGui::TableSetupColumn(tr("Type"));
            ImGui::TableSetupColumn(tr("Scale"));
            ImGui::TableSetupColumn(tr("Add"));
            ImGui::TableSetupColumn(tr("Units"));
            ImGui::TableSetupColumn(tr("Names (0=off, 1=on)"));
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < d.fields.size(); i++) {
                mb::Field& f = d.fields[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (inputString("##n", f.name, 110 * gUi)) ch = true;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(60 * gUi);
                if (ImGui::InputInt("##o", &f.offsetBits, 0)) ch = true;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(50 * gUi);
                if (ImGui::InputInt("##w", &f.widthBits, 0)) ch = true;
                ImGui::TableNextColumn();
                if (combo("##t", f.type, types, 11, 80 * gUi)) ch = true;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(70 * gUi);
                if (ImGui::InputDouble("##s", &f.scale, 0, 0, "%.6g")) ch = true;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(60 * gUi);
                if (ImGui::InputDouble("##a", &f.offset, 0, 0, "%.6g")) ch = true;
                ImGui::TableNextColumn();
                if (inputString("##u", f.units, 50 * gUi)) ch = true;
                ImGui::TableNextColumn();
                if (f.type == mb::FieldType::Enum || f.type == mb::FieldType::Bitflags) {
                    std::string t;
                    if (liveText("##e", f.type == mb::FieldType::Enum ? tableText(f.enumTable) : flagsText(f.flags), t, 200 * gUi)) {
                        if (f.type == mb::FieldType::Enum) f.enumTable = parseTable<int64_t>(t);
                        else f.flags = parseTable<int>(t);
                        ch = true;
                    }
                } else if (inputString("##f", f.format, 60 * gUi)) ch = true;
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) remove = (int)i;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (remove >= 0) { d.fields.erase(d.fields.begin() + remove); ch = true; }
        if (ImGui::Button(tr("Add a field"))) {
            mb::Field f;
            int end = 0;
            for (const mb::Field& g : d.fields) end = std::max(end, g.offsetBits + g.widthBits);
            f.name = "field" + std::to_string(d.fields.size() + 1);
            f.offsetBits = end;
            d.fields.push_back(f);
            ch = true;
        }
        ImGui::TextDisabled("%s", tr("Offsets count bits from the first bit after the sync word (after FEC and descrambling)."));
    }
    if (ImGui::CollapsingHeader(trw("Display"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (d.display.empty()) d.display.push_back({});
        for (size_t i = 0; i < d.display.size(); i++) {
            ImGui::PushID((int)i);
            mb::DisplayRule& r = d.display[i];
            if (!r.whenField.empty()) { ImGui::TextDisabled("%s %s = %g:", tr("when"), r.whenField.c_str(), r.equals); ImGui::SameLine(); }
            if (inputString("##tmpl", r.tmpl, -1)) ch = true;
            ImGui::PopID();
        }
        ImGui::TextDisabled("%s", tr("{name} is a field's value, {name.units} its units, {name.raw} the raw number; {_crc}, {_len}, {_hex}."));
    }
    return ch;
}

// ---------------------------------------------------------------- library, saving, registering
void libraryMenu() {
    if (ImGui::Button(tr("Examples"))) ImGui::OpenPopup("##mbex");
    if (ImGui::BeginPopup("##mbex")) {
        for (const mb::Example& e : mb::examples()) {
            const mb::ParseResult p = mb::parseDefinition(e.text);
            if (ImGui::MenuItem(p.def.name.empty() ? e.file.c_str() : p.def.name.c_str())) { setText(e.text); S.msg.clear(); }
            if (ImGui::IsItemHovered() && !p.def.description.empty()) ImGui::SetTooltip("%s", p.def.description.c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Saved"))) { S.saved = mb::listFolder(modesDir()); S.savedListed = true; ImGui::OpenPopup("##mbsaved"); }
    if (ImGui::BeginPopup("##mbsaved")) {
        if (S.saved.empty()) ImGui::TextDisabled("%s", tr("Nothing saved yet"));
        for (const mb::Saved& s : S.saved) {
            const std::string label = s.error.empty() ? s.name : s.name + "  (" + s.error + ")";
            if (ImGui::MenuItem(label.c_str())) { std::string t, err; if (mb::readText(s.path, t, err)) setText(t); else S.msg = err; }
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", modesDir().c_str());
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("New"))) {
        mb::Definition d;
        d.name = "New mode";
        d.id = "new-mode";
        d.mod = mb::Mod::FSK;
        d.symbolRate = 2400;
        d.deviationHz = 2400;
        d.sampleRate = 96000;
        d.preamble = mb::BitPattern{0xAAAAAAAAull, 32};
        d.sync = mb::BitPattern{0x2DD4, 16};
        d.fixedBytes = 8;
        d.crcKind = mb::CrcKind::Crc;
        mb::crcPreset("CRC-16/CCITT-FALSE", d.crc);
        mb::Field f;
        f.name = "id";
        f.widthBits = 16;
        d.fields.push_back(f);
        d.display.push_back({"", 0, "id {id}"});
        setText(mb::toJson(d));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open file..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) { std::string t, err; if (mb::readText(p, t, err)) setText(t); else S.msg = err; }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!S.parsed.ok);
    if (ImGui::Button(tr("Save"))) {
        std::string path, err;
        if (mb::saveDefinition(modesDir(), S.def, S.text, path, err)) S.msg = std::string(tr("Saved")) + " " + path;
        else S.msg = err;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Keep it in the data folder (modes/<id>.json)"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Save as file..."))) saveText((S.def.id + ".json").c_str(), S.text, S.msg);
    ImGui::SameLine();
    const Registered* reg = R.byId(S.def.id);
    if (ImGui::Button(reg ? tr("Update the mode") : tr("Register as mode"))) {
        std::string path, err;
        if (!mb::saveDefinition(modesDir(), S.def, S.text, path, err)) S.msg = err;
        else {
            Registered r;
            if (reg) r = *reg;
            else { r.family = R.nextFamily(); r.id = S.def.id; }
            r.name = S.def.name;
            if (!reg) R.list.push_back(r);
            else for (auto& x : R.list) if (x.id == r.id) x.name = r.name;
            R.save();
            addRow(r, S.def);
            if (V.family == r.family) V.family = 0;   // the running mode picks up the new definition
            S.msg = std::string(tr("In the mode list under CUSTOM MODES:")) + " " + r.name;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Save it and list it as a mode of its own (group CUSTOM MODES): picking it tunes to its frequency and decodes the live signal on receiver 1"));
    ImGui::EndDisabled();
    if (reg) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Remove the mode"))) {
            R.list.erase(std::remove_if(R.list.begin(), R.list.end(), [&](const Registered& x) { return x.id == S.def.id; }), R.list.end());
            R.save();
            S.msg = tr("Removed from the mode list (the entry goes at the next start); the definition file is kept.");
        }
    }
}

void issuesList() {
    int errs = 0, warns = 0;
    for (const mb::Issue& i : S.parsed.issues) (i.error ? errs : warns)++;
    if (S.parsed.ok && warns == 0) { ImGui::TextColored(pal::okGreen(), "%s", tr("The definition is valid.")); return; }
    if (S.parsed.ok) ImGui::TextColored(pal::okGreen(), "%s", tr("The definition is valid, with warnings:"));
    else ImGui::TextColored(pal::badRed(), tr("%d errors: the definition does not run until they are fixed"), errs);
    for (const mb::Issue& i : S.parsed.issues) ImGui::TextColored(i.error ? pal::badRed() : pal::warnAmber(), "  %s", i.text().c_str());
}

void testTab() {
    const bool busy = S.run == 1;
    ImGui::TextWrapped("%s", tr("Decode a recording with this definition, or a test signal made from it (random frame contents, with the noise and errors you set)."));
    ImGui::SeparatorText(tr("Recording"));
    ImGui::SetNextItemWidth(-90 * gUi);
    ImGui::InputText("##mbpath", S.path, sizeof S.path);
    ImGui::SameLine();
    if (ImGui::Button(tr("Browse..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            std::snprintf(S.path, sizeof S.path, "%s", p.c_str());
            switch (dect2::guessFormat(p)) {
                case dect2::FileFormat::CF32: S.fmt = 0; break;
                case dect2::FileFormat::CI16: S.fmt = 1; break;
                case dect2::FileFormat::CU8: S.fmt = 2; break;
                default: S.fmt = 3; break;
            }
            const double r = dect2::guessSampleRate(p);
            if (r > 0) S.rate = r;
        }
    }
    ImGui::SetNextItemWidth(170 * gUi);
    ImGui::Combo(tr("Format"), &S.fmt, "complex float32\0complex int16\0complex uint8 (RTL-SDR)\0complex int8 (HackRF)\0");
    ImGui::SetNextItemWidth(170 * gUi);
    if (S.rate <= 0) S.rate = S.def.sampleRate;
    ImGui::InputDouble(tr("Sample rate (Hz)"), &S.rate, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(170 * gUi);
    ImGui::InputDouble(tr("Signal offset from the centre (Hz)"), &S.offset, 0, 0, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Added to the definition's own offset; read it off the waterfall of the recording"));
    ImGui::BeginDisabled(busy || !S.path[0] || !S.parsed.ok);
    if (ImGui::Button(tr("Test on recording"))) testRecording();
    ImGui::EndDisabled();
    ImGui::SeparatorText(tr("Test signal"));
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputInt(tr("Frames"), &S.genFrames);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat(tr("SNR (dB)"), &S.genSnr, 0, 60, "%.0f");
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputFloat(tr("Frequency error (Hz)"), &S.genCfo, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputFloat(tr("Clock error (ppm)"), &S.genPpm, 0, 0, "%.0f");
    ImGui::BeginDisabled(busy || !S.parsed.ok);
    if (ImGui::Button(tr("Generate and decode"))) testGenerated(false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save the test signal..."))) testGenerated(true);
    ImGui::EndDisabled();
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (!S.workMsg.empty()) ImGui::TextWrapped("%s", S.workMsg.c_str());
        if (S.haveResult) {
            ImGui::SeparatorText(tr("Frames"));
            exportButtons(S.resultDef, S.result, S.msg);
            ImGui::SameLine();
            if (ImGui::Button(tr("Clear"))) { S.result.clear(); S.haveResult = false; }
            framesTable("##mbres", S.resultDef, S.result, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y));
        }
    }
}

void windowPanel(App& a) {
    if (!S.inited) init();
    if (S.run == 2) { if (S.worker.joinable()) S.worker.join(); S.run = 0; }
    firstWindowSize(980 * gUi, 720 * gUi);
    if (!ImGui::Begin(trw("Mode Builder"), &a.modeBuilderOpen)) { ImGui::End(); return; }
    reparse();
    libraryMenu();
    if (!S.msg.empty()) ImGui::TextWrapped("%s", S.msg.c_str());
    ImGui::Separator();
    if (ImGui::BeginTabBar("##mbtabs")) {
        if (ImGui::BeginTabItem(trw("Form"))) {
            if (!S.parsed.ok) {
                ImGui::TextColored(pal::warnAmber(), "%s", tr("The JSON has errors; the form shows the last valid definition and editing it replaces the JSON."));
            }
            ImGui::BeginChild("##mbform", ImVec2(0, 0));
            mb::Definition d = S.def;
            if (formView(d)) { S.def = d; setText(mb::toJson(d)); reparse(); }
            ImGui::Spacing();
            issuesList();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trw("JSON"))) {
            const float h = ImGui::GetContentRegionAvail().y;
            ImGui::PushFont(a.mono, 0);
            ImGui::InputTextMultiline("##mbjson", S.text.data(), S.text.capacity() + 1, ImVec2(-1, h * 0.72f), ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, resizeCb, &S.text);
            ImGui::PopFont();
            if (ImGui::Button(tr("Tidy"))) { if (S.parsed.ok) { setText(mb::toJson(S.def)); reparse(); } }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Rewrite the JSON in the standard layout (valid definitions only)"));
            ImGui::SameLine();
            if (ImGui::Button(tr("Copy"))) ImGui::SetClipboardText(S.text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(tr("Paste"))) { const char* c = ImGui::GetClipboardText(); if (c) setText(c); }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("Format: docs/MODE-BUILDER.md (the Signal Lab writes the same JSON)"));
            ImGui::BeginChild("##mbissues", ImVec2(0, 0));
            issuesList();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trw("Test"))) { testTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- the custom modes' main tab
void builderOverview(App& a) {
    ImGui::TextWrapped("%s", tr("The Mode Builder makes a decoder from a description of a digital protocol: modulation, sync word, frame length, error correction, check value and fields. Build one, test it on a recording or on a test signal made from it, then register it: it is listed here as a mode of its own."));
    if (ImGui::Button(tr("Open the Mode Builder"))) a.modeBuilderOpen = true;
    ImGui::Spacing();
    if (R.list.empty()) { ImGui::TextDisabled("%s", tr("No custom modes yet.")); return; }
    ImGui::SeparatorText(tr("Your custom modes"));
    for (const Registered& r : R.list) {
        ImGui::PushID(r.family);
        if (ImGui::Button(r.name.c_str())) selectMode(a, r.family);
        ImGui::SameLine();
        ImGui::TextDisabled("%s.json", r.id.c_str());
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the Signal view: histogram (OOK, ASK, FSK, GFSK, GMSK) or constellation (PSK)
void signalHistogram(const mb::SignalView& s) {
    const bool fsk = s.mod == mb::Mod::FSK || s.mod == mb::Mod::GFSK || s.mod == mb::Mod::GMSK;
    if (!plt::BeginPlot("##mbhist", ImVec2(-1, 220 * gUi), plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes(fsk ? tr("instantaneous frequency (Hz)") : tr("envelope (1 = full scale)"), tr("symbols"));
    plt::SetupAxisLimits(plt::X1, s.histMin, s.histMax, plt::Cond_Always);
    float h[64], x[64];
    const double bw = (s.histMax - s.histMin) / 64;
    float top = 1;
    for (int i = 0; i < 64; i++) { h[i] = i < (int)s.hist.size() ? s.hist[(size_t)i] : 0.f; x[i] = (float)(s.histMin + (i + 0.5) * bw); top = std::max(top, h[i]); }
    plt::SetupAxisLimits(plt::Y1, 0, top * 1.15, plt::Cond_Always);
    plt::Spec bs; bs.FillColor = pal::accent(0.8f);
    plt::PlotBars("##mbhbars", x, h, 64, bw, bs);
    if (!s.thresholds.empty()) {   // the slicer's thresholds now
        plt::Spec ts; ts.LineColor = ImVec4(0.95f, 0.55f, 0.25f, 0.95f); ts.LineWeight = 1.6f;
        plt::PlotInfLines("##mbhthr", s.thresholds.data(), (int)s.thresholds.size(), ts);
    }
    if (!s.centres.empty()) {      // the measured level centres
        plt::Spec cs; cs.LineColor = ImVec4(0.45f, 0.85f, 0.5f, 0.9f); cs.LineWeight = 1.2f;
        plt::PlotInfLines("##mbhcen", s.centres.data(), (int)s.centres.size(), cs);
    }
    plt::EndPlot();
}

void signalConstellation(const mb::SignalView& s) {
    const float side = std::min(ImGui::GetContentRegionAvail().x, 280 * gUi);
    if (!plt::BeginPlot("##mbconst", ImVec2(side, side), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal | plt::Flags_NoMouseText)) return;
    plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
    plt::SetupAxisLimits(plt::X1, -1.6, 1.6, plt::Cond_Always);
    plt::SetupAxisLimits(plt::Y1, -1.6, 1.6, plt::Cond_Always);
    if (!s.points.empty()) {
        plt::Spec sp; sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.6f; sp.Stride = sizeof(mb::cf);
        sp.MarkerFillColor = sp.MarkerLineColor = sp.LineColor = pal::accent(0.6f);
        const float* d = reinterpret_cast<const float*>(s.points.data());
        plt::PlotScatter("##mbcpts", d, d + 1, (int)s.points.size(), sp);
    }
    std::vector<mb::cf> ideal;
    for (int k = 0; k < s.levels; k++) ideal.push_back(mb::cf((float)std::cos(2 * 3.14159265358979 * k / s.levels), (float)std::sin(2 * 3.14159265358979 * k / s.levels)));
    plt::Spec gs; gs.Marker = plt::Marker_Cross; gs.MarkerSize = 5.f; gs.Stride = sizeof(mb::cf);
    gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.85f);
    const float* g = reinterpret_cast<const float*>(ideal.data());
    plt::PlotScatter("##mbcideal", g, g + 1, (int)ideal.size(), gs);
    plt::EndPlot();
}

// Advanced: write what the Signal view measured into the definition (FSK: deviation and offset; GMSK and PSK: offset) and reload the mode
void applyMeasured(const Registered& r) {
    const mb::SignalView& s = V.sig;
    mb::Definition d = V.def;
    if (s.mod == mb::Mod::FSK || s.mod == mb::Mod::GFSK) d.deviationHz = std::round(s.deviationHz);
    d.offsetHz += std::round(s.offsetHz);
    std::string err;
    if (mb::writeText(modesDir() + "/" + r.id + ".json", mb::toJson(d), err)) { V.msg = tr("Measured values saved to the definition."); V.family = 0; }   // V.family = 0: the mode loads again
    else V.msg = err;
}

void signalTab(App& a, const Registered& r) {
    (void)a;
    V.wantSig = true;
    const mb::SignalView& s = V.sig;
    const bool psk = s.mod == mb::Mod::PSK;
    if (!s.have) { ImGui::TextDisabled("%s", tr("Waiting for a signal: the view fills while the squelch is open.")); return; }
    if (psk) signalConstellation(s); else signalHistogram(s);
    if (!psk) ImGui::TextDisabled("%s", tr("orange: slicer thresholds, green: measured level centres"));
    else ImGui::TextDisabled("%s", tr("crosses: ideal points, dots: symbols after carrier recovery"));
    ImGui::Text(tr("%d of %d level clusters found    separation %.1f    %s %.1f dB"), s.clusters, s.levels, s.separation, psk ? tr("Es/N0") : tr("eye SNR"), s.snrDb);
    if (psk) ImGui::Text(tr("EVM %.1f %%    Eb/N0 %.1f dB    carrier offset %+.0f Hz"), s.evmPct, s.ebn0Db, s.offsetHz);
    else {
        const char* u = s.unit;
        for (size_t i = 0; i < s.thresholds.size() && i < 3; i++) ImGui::Text(tr("threshold %zu: %.4g %s (measured %.4g)"), i + 1, s.thresholds[i], u, i < s.measuredThresholds.size() ? s.measuredThresholds[i] : 0.0);
        if (s.resolved && s.unit[0]) ImGui::Text(tr("measured deviation %.0f Hz    offset %+.0f Hz"), s.deviationHz, s.offsetHz);
    }
    if (!s.resolved) ImGui::TextColored(pal::badRed(), "%s", tr("The clusters are not separate yet: weak signal, wrong symbol rate or deviation, or only noise."));
    if (ImGui::CollapsingHeader(tr("Advanced"))) {
        const bool fskLike = s.mod == mb::Mod::FSK || s.mod == mb::Mod::GFSK;
        const bool canApply = s.resolved && (fskLike || psk || s.mod == mb::Mod::GMSK);
        ImGui::BeginDisabled(!canApply);
        if (ImGui::Button(tr("Use measured deviation / offset"))) applyMeasured(r);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", fskLike ? tr("Writes the measured deviation and the carrier offset into the definition.") :
                                psk || s.mod == mb::Mod::GMSK ? tr("Writes the measured carrier offset into the definition.") :
                                tr("The slicer thresholds follow the signal by themselves; nothing to write."));
    }
}

void customTab(App& a) {
    if (a.family == kCustomFamilyBase - 1) { builderOverview(a); return; }
    const Registered* r = R.byFamily(a.family);
    if (!r) { ImGui::TextDisabled("%s", tr("This custom mode is not registered any more.")); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(r->name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Edit in Mode Builder"))) {
        std::string t, err;
        if (mb::readText(modesDir() + "/" + r->id + ".json", t, err)) { if (!S.inited) init(); setText(t); }
        a.modeBuilderOpen = true;
    }
    if (a.rxId != 1) { ImGui::TextWrapped("%s", tr("Custom modes run on receiver 1.")); return; }
    if (!V.error.empty()) { ImGui::TextColored(pal::badRed(), "%s", V.error.c_str()); return; }
    const mb::RxStats& st = V.stats;
    ImGui::TextDisabled(tr("%s %d, %.0f Bd    symbols %llu    sync words %llu    frames %llu    %s    last SNR %.0f dB"), mb::modName(V.def.mod), V.def.levels(), V.def.symbolRate,
                        (unsigned long long)st.symbols, (unsigned long long)st.syncHits, (unsigned long long)st.frames, st.open ? tr("signal") : tr("quiet"), st.snrDb);
    if (ImGui::BeginTabBar("##mbview")) {
        if (ImGui::BeginTabItem(trw("Frames"))) {
            exportButtons(V.def, V.frames, V.msg);
            ImGui::SameLine();
            if (ImGui::Button(tr("Clear"))) V.frames.clear();
            if (!V.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", V.msg.c_str()); }
            framesTable("##mblive", V.def, V.frames, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(trw("Signal"))) {
            if (!V.msg.empty()) ImGui::TextDisabled("%s", V.msg.c_str());
            signalTab(a, *r);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void customSummary(const App& a, std::string& l1, std::string& l2) {
    if (a.family == kCustomFamilyBase - 1) { l1 = "Mode Builder"; l2 = std::to_string(R.list.size()) + " custom modes"; return; }
    const Registered* r = R.byFamily(a.family);
    l1 = r ? r->name : "Custom mode";
    l2 = std::to_string((unsigned long long)V.stats.frames) + " frames";
}
void customStatus(App&) {
    ImGui::TextDisabled("%s", V.error.empty() ? (V.stats.open ? tr("Custom mode: signal") : tr("Custom mode: listening")) : V.error.c_str());
}

}   // namespace

extern const ModeUi kModeBuilderUi;
const ModeUi kModeBuilderUi = {
    .sideTitle = "FRAMES",
    .tabName = "Frames",
    .tabIcon = Ic::Layers,
    .tab = customTab,
    .status = customStatus,
    .summary = customSummary,
};

bool customModeSink(const float* iq, size_t n, double rate) {
    std::lock_guard<std::mutex> lk(L.mu);
    if (!L.rx) return false;
    if (rate != L.rate) { L.rate = rate; L.rx->configure(rate, 0); }
    L.buf.resize(n);
    for (size_t i = 0; i < n; i++) L.buf[i] = mb::cf(iq[2 * i], iq[2 * i + 1]);
    L.rx->feed(L.buf.data(), n);
    return true;
}

void modeBuilderRegister(App& a) {
    if (a.rxId != 1) return;
    if (!R.loaded) {   // the registered custom modes join the mode list before anything draws (#465: the toolbar asks for the tuning of the mode on screen)
        dect2::registerPluginTuning(kCustomFamilyBase - 1 + 2, "modebuilder", "Mode Builder", 0.001, 300000, 433.92, 250000, 0.2);
        R.load();
        for (const Registered& r : R.list) {
            mb::Definition d;
            std::string err;
            if (loadRegistered(r, d, err)) addRow(r, d);
            else { mb::Definition dd; dd.name = r.name; dd.symbolRate = 1; addRow(r, dd); }
        }
    }
}

void modeBuilderFrame(App& a) {
    if (a.rxId != 1) return;
    modeBuilderRegister(a);
    if (!isCustomFamily(a.family)) {
        if (V.family) {
            std::lock_guard<std::mutex> lk(L.mu);
            L.rx.reset();
            L.family = 0;
            V.family = 0;
        }
        return;
    }
    if (V.family != a.family) {
        V.family = a.family;
        V.frames.clear();
        V.error.clear();
        V.msg.clear();
        V.stats = {};
        V.sig = mb::SignalView();
        std::unique_ptr<mb::Receiver> rx;
        const Registered* r = R.byFamily(a.family);
        if (!r) V.error = tr("This custom mode is not registered any more.");
        else if (!loadRegistered(*r, V.def, V.error)) V.error = std::string(tr("The definition does not load:")) + " " + V.error;
        else rx = std::make_unique<mb::Receiver>(V.def);
        std::lock_guard<std::mutex> lk(L.mu);
        L.rx = std::move(rx);
        L.rate = 0;
        L.family = a.family;
    }
    std::vector<mb::Frame> got;
    {
        std::lock_guard<std::mutex> lk(L.mu);
        if (!L.rx) return;
        got = L.rx->take();
        V.stats = L.rx->stats();
        if (V.wantSig) V.sig = L.rx->signalView();
        else V.sig = mb::SignalView();
        V.wantSig = false;
    }
    for (mb::Frame& f : got) V.frames.push_back(std::move(f));
    while (V.frames.size() > kMaxFrames) V.frames.pop_front();
}

void modeBuilderOpenText(const std::string& json) {
    if (!S.inited) init();
    setText(json);
    S.msg = tr("From the Protocol lab: check the frame length, the fields and the check value, then test it on the recording.");
    S.openRequest = true;
}

void modeBuilderWindow(App& a) {
    if (S.openRequest && a.rxId == 1) { S.openRequest = false; a.modeBuilderOpen = true; }
    if (a.rxId != 1) {   // one Mode Builder for the program: RX 1's window
        if (App* f = receiverById(1)) { f->modeBuilderOpen = a.modeBuilderOpen; modeBuilderWindow(*f); a.modeBuilderOpen = f->modeBuilderOpen; }
        return;
    }
    if (a.modeBuilderOpen) windowPanel(a);
}
