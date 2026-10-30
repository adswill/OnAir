// Tools > Signal Lab: blind analysis of an unknown digital signal. The input is the signal selected on the spectrum (the Identify
// tool or Shift+drag keeps its samples while this window is open), an IQ recording, bits pasted in, or a made-up test signal. The
// stages (core/src/signallab) run on a worker thread; this file is the window: the stage results, the constellation, the bit viewer
// (frames in a grid with a per-column heat map, field boundaries, interpretations, a diff of two frames), notes, and the exports
// (the Mode Builder description of docs/SIGNAL-LAB.md, CSV, hex dump, the saved analysis). Nothing here decrypts anything.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "dect2/common/u8file.h"
#include "dect2/loraraw/loraraw_lab.h"
#include "dect2/protolab/protolab.h"
#include "dect2/signallab/signallab.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
namespace sl = dect2::signallab;

struct State {
    // input
    char path[512] = "";
    int fmt = 0;                      // 0 cf32, 1 cs16, 2 cu8, 3 cs8
    double rate = 250000, offset = 0, bw = 0;
    std::string pasted;
    int demoSeed = 1;
    bool autoLive = false;
    uint64_t keptSeq = 0;
    bool keepOn = false;
    std::string liveInfo;
    // options
    int forceMod = 0;                 // 0 auto, OOK, ASK, FSK, MSK, GMSK, PSK, QAM
    int forceOrder = 0;
    double forceBaud = 0;
    float gapMs = 4;
    // worker
    std::thread worker;
    std::atomic<int> run{0};          // 0 idle, 1 running, 2 finished
    std::mutex mu;
    std::string err, busyText;
    // result
    sl::Analysis A;
    bool have = false;
    std::string truth;                // demo signals: what was sent
    char name[96] = "unknown signal";
    std::string notes;
    // bit viewer
    int selFrame = 0;
    int selA = -1, selB = -1;         // selected columns [min, max]
    bool dragging = false;
    bool diffOn = false;
    int diffA = 0, diffB = 1;
    int heat = 1;                     // 0 entropy, 1 change rate
    bool lsbFirst = false;
    char fieldName[48] = "field";
    int fieldType = 5;
    // frames tab
    int frameLen = 0;
    char syncText[80] = "";
    // LoRa demodulator
    int lrSource = 0;                 // 0 the Recording tab's file, 1 the last spectrum selection
    int lrSf = 7, lrBwIdx = 2, lrCr = 5, lrLdro = 0, lrImplicitLen = 16, lrPreamble = 8;
    double lrBwHz = 125000;           // used when lrBwIdx is the last entry (custom)
    int lrSync = 0x12;
    bool lrImplicit = false, lrCrc = true, lrInvert = false;
    double lrOffset = 0;
    bool lrAutoOffset = true;
    std::vector<dect2::lora::LabFrame> lrFrames;
    dect2::lora::DemodStats lrStats;
    dect2::lora::LabDetect lrDet;
    std::string lrNote;
    // evaluation
    int evalCases = 40;
    std::string evalText;
    ~State() { if (worker.joinable()) worker.join(); }
};
State S;

const sl::Mod kMods[] = {sl::Mod::Unknown, sl::Mod::OOK, sl::Mod::ASK, sl::Mod::FSK, sl::Mod::MSK, sl::Mod::GMSK, sl::Mod::PSK, sl::Mod::QAM};

void startWork(const char* what, std::function<void()> job) {
    if (S.run == 1) return;
    if (S.worker.joinable()) S.worker.join();
    S.run = 1;
    { std::lock_guard<std::mutex> lk(S.mu); S.err.clear(); S.busyText = what; }
    S.worker = dect2::namedThread("onair-signallab", [job] {
        try { job(); } catch (const std::exception& e) { std::lock_guard<std::mutex> lk(S.mu); S.err = e.what(); }
        S.run = 2;
    });
}

sl::Options optionsNow() {
    sl::Options o;
    o.minGapSec = S.gapMs / 1000.0;
    o.forceMod = kMods[std::clamp(S.forceMod, 0, 7)];
    o.forceOrder = S.forceOrder;
    o.forceBaud = S.forceBaud;
    return o;
}

void setResult(sl::Analysis a, const std::string& truth) {
    std::lock_guard<std::mutex> lk(S.mu);
    a.name = S.name;
    a.notes = S.notes;
    S.A = std::move(a);
    S.have = true;
    S.truth = truth;
    S.selFrame = 0; S.selA = S.selB = -1; S.diffA = 0; S.diffB = 1;
    S.frameLen = (int)S.A.frame.frameBits;
    snprintf(S.syncText, sizeof S.syncText, "%s", sl::bitsToString(S.A.frame.sync.pattern).c_str());
}

void analyseSamples(std::vector<sl::cf> x, double fs, std::string truth = "") {
    const sl::Options o = optionsNow();
    startWork(tr("analysing"), [x = std::move(x), fs, o, truth] { setResult(sl::analyse(x, fs, o), truth); });
}

void analyseFile() {
    const std::string path = S.path;
    static const dect2::protolab::IqFormat fmts[] = {dect2::protolab::IqFormat::Cf32, dect2::protolab::IqFormat::Cs16, dect2::protolab::IqFormat::Cu8, dect2::protolab::IqFormat::Cs8};
    const auto f = fmts[std::clamp(S.fmt, 0, 3)];
    const double rate = S.rate, off = S.offset, bw = S.bw;
    const sl::Options o = optionsNow();
    startWork(tr("reading the recording"), [=] {
        std::vector<dect2::protolab::cf> iq;
        std::string err;
        if (!dect2::protolab::loadIq(path, f, iq, err, 30000000)) { std::lock_guard<std::mutex> lk(S.mu); S.err = err; return; }
        double r = rate;
        std::vector<sl::cf> x = (bw > 0 || off != 0) ? sl::channelise(iq, rate, off, bw > 0 ? bw : rate / 4, r) : std::move(iq);
        setResult(sl::analyse(x, r, o), "");
    });
}

void analyseLive(const dect2::sigid::Capture::Kept& k) {
    double r = k.rateHz;
    std::vector<sl::cf> x = k.widthHz > 0 ? sl::channelise(k.x, k.rateHz, k.centreOffHz, k.widthHz, r) : sl::mix(k.x, k.rateHz, -k.centreOffHz);
    char b[160];
    snprintf(b, sizeof b, tr("selection at %.4f MHz, %.1f kHz wide, %.2f s"), (k.rfHz + k.centreOffHz) / 1e6, k.widthHz / 1e3, (double)k.x.size() / k.rateHz);
    S.liveInfo = b;
    analyseSamples(std::move(x), r);
}

void analysePasted() {
    std::vector<sl::Bits> st;
    std::istringstream in(S.pasted);
    std::string line;
    while (std::getline(in, line)) {
        sl::Bits b;
        const bool bits = line.find_first_not_of("01 \t\r") == std::string::npos;
        if (bits) b = sl::stringToBits(line);
        else {
            for (char c : line) {
                const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) continue;
                for (int k = 3; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1));
            }
        }
        if (b.size() >= 16) st.push_back(std::move(b));
    }
    if (st.empty()) { std::lock_guard<std::mutex> lk(S.mu); S.err = tr("Paste one frame or stream per line, as bits (0/1) or hex."); return; }
    startWork(tr("analysing the bits"), [st] { setResult(sl::analyseBits(st), ""); });
}

void analyseDemo() {
    const sl::GenSpec g = sl::randomSpec((uint32_t)std::max(1, S.demoSeed));
    char b[300];
    snprintf(b, sizeof b, "%s-%d, %.0f Bd, %s%s%s, sync %s, %d payload bytes%s%s, %s, SNR %.0f dB, offset %+.0f Hz",
             sl::modName(g.mod), g.order, g.baud, sl::lineCodeName(g.line), g.scrambler >= 0 ? ", " : "", g.scrambler >= 0 ? sl::scramblers()[(size_t)g.scrambler].name : "",
             sl::bitsToHex(g.sync).c_str(), g.payloadBytes, g.crcAlgo >= 0 ? " + " : "", g.crcAlgo >= 0 ? sl::checkAlgos()[(size_t)g.crcAlgo].name : "",
             g.gapSec > 0 ? "bursts" : "continuous", g.snrDb, g.offsetHz);
    const std::string truth = b;
    const sl::Options o = optionsNow();
    startWork(tr("making and analysing a test signal"), [g, o, truth] { setResult(sl::analyse(sl::generate(g), g.fs, o), truth); });
}

// the analysis is changed on the worker (the checksum search can take a moment), on a copy
void modify(const char* what, std::function<void(sl::Analysis&)> f) {
    sl::Analysis copy;
    { std::lock_guard<std::mutex> lk(S.mu); copy = S.A; }
    startWork(what, [copy, f]() mutable {
        f(copy);
        std::lock_guard<std::mutex> lk(S.mu);
        S.A = std::move(copy);
        S.frameLen = (int)S.A.frame.frameBits;
        snprintf(S.syncText, sizeof S.syncText, "%s", sl::bitsToString(S.A.frame.sync.pattern).c_str());
    });
}

bool saveText(const char* suggest, const std::string& text) {
    const std::string p = saveFileDialog(suggest);
    if (p.empty()) return false;
    std::ofstream f(dect2::u8file(p), std::ios::binary);
    f << text;
    return (bool)f;
}

ImU32 fieldColor(sl::FieldType t) {
    switch (t) {
        case sl::FieldType::Constant: return IM_COL32(140, 140, 150, 255);
        case sl::FieldType::Counter: return IM_COL32(110, 170, 255, 255);
        case sl::FieldType::Checksum: return IM_COL32(110, 210, 120, 255);
        case sl::FieldType::Ascii: return IM_COL32(240, 160, 90, 255);
        case sl::FieldType::Enum: return IM_COL32(230, 200, 90, 255);
        case sl::FieldType::Value: return IM_COL32(200, 130, 230, 255);
        case sl::FieldType::Bytes: return IM_COL32(170, 120, 200, 255);
    }
    return IM_COL32_WHITE;
}

// blue (never changes) to red (changes all the time)
ImU32 heatColor(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const int r = (int)(40 + 215 * v), g = (int)(60 + 120 * (1 - std::fabs(v - 0.5) * 2)), b = (int)(200 * (1 - v) + 30);
    return IM_COL32(r, g, b, 255);
}

void inputPanel(App& a) {
    const bool busy = S.run == 1;
    if (!ImGui::BeginTabBar("##slin")) return;
    if (ImGui::BeginTabItem(trw("Spectrum selection"))) {
        ImGui::TextWrapped("%s", tr("Select the unknown signal on the spectrum: the Identify tool (key I) or Shift+drag across it. While this window is open the samples of that selection are kept, and the lab analyses them."));
        const uint64_t ks = a.engine.sigid().keptSeq();
        const bool fresh = ks != 0 && ks != S.keptSeq;
        auto take = [&] {   // copies the samples (up to 32 MB) only when they are analysed
            dect2::sigid::Capture::Kept k;
            uint64_t seq = S.keptSeq;
            if (a.engine.sigid().keptSamples(k, seq)) { S.keptSeq = seq; analyseLive(k); }
        };
        if (fresh && S.autoLive && !busy) take();
        ImGui::Checkbox(tr("Analyse each new selection"), &S.autoLive);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || ks == 0);
        if (ImGui::Button(tr("Analyse the last selection"))) { S.keptSeq = 0; take(); }
        ImGui::EndDisabled();
        if (!a.engine.running()) ImGui::TextDisabled("%s", tr("Start the radio (or a recording) first."));
        else if (!fresh && S.liveInfo.empty()) ImGui::TextDisabled("%s", tr("No selection yet."));
        if (!S.liveInfo.empty()) ImGui::TextDisabled("%s", S.liveInfo.c_str());
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Recording"))) {
        ImGui::SetNextItemWidth(-90 * gUi);
        ImGui::InputText("##slpath", S.path, sizeof S.path);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse..."))) {
            const std::string p = openFileDialog();
            if (!p.empty()) {
                snprintf(S.path, sizeof S.path, "%s", p.c_str());
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
        ImGui::SetNextItemWidth(150 * gUi);
        ImGui::Combo(tr("Format"), &S.fmt, "complex float32\0complex int16\0complex uint8 (RTL-SDR)\0complex int8 (HackRF)\0");
        ImGui::SetNextItemWidth(150 * gUi);
        ImGui::InputDouble(tr("Sample rate (Hz)"), &S.rate, 0, 0, "%.0f");
        ImGui::SetNextItemWidth(150 * gUi);
        ImGui::InputDouble(tr("Signal offset from the centre (Hz)"), &S.offset, 0, 0, "%.0f");
        ImGui::SetNextItemWidth(150 * gUi);
        ImGui::InputDouble(tr("Signal bandwidth (Hz, 0 = all)"), &S.bw, 0, 0, "%.0f");
        ImGui::BeginDisabled(busy || !S.path[0]);
        if (ImGui::Button(tr("Analyse the recording"))) analyseFile();
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Bits"))) {
        ImGui::TextWrapped("%s", tr("Bits you already have (from another tool): one frame or stream per line, as 0/1 or hex. Stages 4 to 6 run on them."));
        struct Cb { static int resize(ImGuiInputTextCallbackData* d) { if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) { auto* s = static_cast<std::string*>(d->UserData); s->resize((size_t)d->BufTextLen); d->Buf = s->data(); } return 0; } };
        ImGui::InputTextMultiline("##slpaste", S.pasted.data(), S.pasted.capacity() + 1, ImVec2(-1, 100 * gUi), ImGuiInputTextFlags_CallbackResize, Cb::resize, &S.pasted);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Analyse the bits"))) analysePasted();
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Test signal"))) {
        ImGui::TextWrapped("%s", tr("A made-up signal with a known structure (the same generator as the evaluation): see what the lab finds, then compare with what was sent."));
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::InputInt(tr("Signal number"), &S.demoSeed);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Make and analyse"))) analyseDemo();
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Saved analysis"))) {
        ImGui::TextWrapped("%s", tr("An analysis saved before (its frames, fields and notes)."));
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Load..."))) {
            const std::string p = openFileDialog();
            if (!p.empty()) {
                std::ifstream f(dect2::u8file(p), std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string text = ss.str();
                startWork(tr("loading"), [text] {
                    sl::Analysis a;
                    std::string err;
                    if (!sl::loadAnalysisJson(text, a, err)) { std::lock_guard<std::mutex> lk(S.mu); S.err = err; return; }
                    std::lock_guard<std::mutex> lk(S.mu);
                    snprintf(S.name, sizeof S.name, "%s", a.name.c_str());
                    S.notes = a.notes;
                    S.A = std::move(a);
                    S.have = true;
                    S.truth.clear();
                    S.frameLen = (int)S.A.frame.frameBits;
                    snprintf(S.syncText, sizeof S.syncText, "%s", sl::bitsToString(S.A.frame.sync.pattern).c_str());
                });
            }
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (ImGui::TreeNode(trw("Options"))) {
        ImGui::SetNextItemWidth(130 * gUi);
        ImGui::Combo(tr("Modulation"), &S.forceMod, "automatic\0OOK\0ASK\0FSK\0MSK\0GMSK\0PSK\0QAM\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::InputInt(tr("Order (0 auto)"), &S.forceOrder);
        S.forceOrder = std::clamp(S.forceOrder, 0, 16);
        ImGui::SetNextItemWidth(130 * gUi);
        ImGui::InputDouble(tr("Symbol rate (Bd, 0 = estimate)"), &S.forceBaud, 0, 0, "%.1f");
        ImGui::SetNextItemWidth(130 * gUi);
        ImGui::SliderFloat(tr("Shortest pause between bursts (ms)"), &S.gapMs, 0.2f, 50.f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::TreePop();
    }
}

void summaryTab() {
    const sl::Analysis& A = S.A;
    if (!S.truth.empty()) { ImGui::TextColored(pal::accent(), "%s", tr("Sent:")); ImGui::SameLine(); ImGui::TextWrapped("%s", S.truth.c_str()); }
    ImGui::BeginChild("##sllog", ImVec2(0, 200 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& l : A.log) {
        const bool head = !l.empty() && l[0] >= '1' && l[0] <= '6';
        if (head) ImGui::TextColored(pal::accent(), "%s", l.c_str());
        else ImGui::TextUnformatted(l.c_str());
    }
    ImGui::EndChild();
    if (ImGui::Button(tr("Copy the log"))) {
        std::string t;
        for (const auto& l : A.log) t += l + "\n";
        ImGui::SetClipboardText(t.c_str());
    }
}

void channelTab() {
    const sl::ChannelInfo& c = S.A.channel;
    ImGui::Text(tr("Bandwidth %.0f Hz (99 %% of the power), %.0f Hz at -3 dB"), c.bw99Hz, c.bw3dBHz);
    ImGui::Text(tr("Carrier offset %+.1f Hz, SNR %.1f dB"), c.offsetHz, c.snrDb);
    ImGui::Text(tr("%zu burst(s), on %.0f %% of the time, mean burst %.2f ms%s"), c.bursts.size(), c.dutyCycle * 100, c.meanBurstSec * 1000, c.continuous ? tr(" (continuous)") : "");
    if (c.burstPeriodSec > 0) ImGui::Text(tr("The bursts repeat every %.3f ms"), c.burstPeriodSec * 1000);
    // the bursts on a time line
    if (!c.bursts.empty() && S.A.fs > 0) {
        const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), h = 26 * gUi;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 24, 30, 255));
        const double end = (double)c.bursts.back().end;
        for (const sl::Burst& b : c.bursts) {
            const float x0 = p0.x + (float)(b.start / end) * w, x1 = p0.x + std::max(1.f, (float)(b.end / end) * w);
            dl->AddRectFilled(ImVec2(x0, p0.y + 3), ImVec2(std::max(x1, x0 + 1), p0.y + h - 3), ImGui::GetColorU32(pal::accent(0.85f)));
        }
    }
}

void modulationTab() {
    const sl::Analysis& A = S.A;
    if (ImGui::BeginTable("##slmod", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn(tr("Family"));
        ImGui::TableSetupColumn(tr("Confidence"));
        ImGui::TableSetupColumn(tr("Why"));
        ImGui::TableHeadersRow();
        for (const sl::ModGuess& g : A.mod.ranked) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (g.order) ImGui::Text("%s-%d", sl::modName(g.mod), g.order); else ImGui::TextUnformatted(sl::modName(g.mod));
            ImGui::TableSetColumnIndex(1);
            ImGui::ProgressBar((float)g.confidence, ImVec2(90 * gUi, 0));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", g.why.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Text(tr("Used: %s-%d"), sl::modName(A.usedMod), A.usedOrder);
    if (A.usedMod == sl::Mod::FSK || A.usedMod == sl::Mod::MSK || A.usedMod == sl::Mod::GMSK)
        ImGui::Text(tr("Deviation %.0f Hz, tone spacing %.0f Hz, modulation index %.2f"), A.mod.deviationHz, A.mod.toneSpacingHz, A.mod.modIndex);
    if (A.mod.spreadHint) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr("Spread-spectrum hint: noise-like, or a repeating chip code."));
    const sl::ModFeatures& f = A.mod.f;
    ImGui::TextDisabled(tr("envelope variation %.2f, kurtosis %.2f, near-zero %.0f %%, x^n lines %.0f / %.0f / %.0f / %.0f, frequency levels %d, cyclic prefix %.2f, flatness %.2f"),
                        f.envCv, f.envKurt, f.lowFrac * 100, f.line[1], f.line[2], f.line[4], f.line[8], f.freqClusters, f.cpCorr, f.flatness);
    // constellation (PSK/QAM) or the soft levels (the others), of the first bursts
    const float sz = 220 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz, sz));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz, p0.y + sz), IM_COL32(16, 20, 26, 255));
    dl->AddLine(ImVec2(p0.x + sz / 2, p0.y), ImVec2(p0.x + sz / 2, p0.y + sz), IM_COL32(60, 60, 70, 255));
    dl->AddLine(ImVec2(p0.x, p0.y + sz / 2), ImVec2(p0.x + sz, p0.y + sz / 2), IM_COL32(60, 60, 70, 255));
    const ImU32 dot = ImGui::GetColorU32(pal::accent(0.6f));
    size_t drawn = 0;
    float scale = 1;
    for (const sl::Symbols& s : A.symbols) {
        if (!s.points.empty()) {
            double p = 0; for (const auto& z : s.points) p += std::norm(z); p /= (double)s.points.size();
            scale = (float)(sz * 0.3 / std::sqrt(std::max(1e-12, p)));
            for (const auto& z : s.points) { if (drawn++ > 6000) break; dl->AddRectFilled(ImVec2(p0.x + sz / 2 + z.real() * scale, p0.y + sz / 2 - z.imag() * scale), ImVec2(p0.x + sz / 2 + z.real() * scale + 2, p0.y + sz / 2 - z.imag() * scale + 2), dot); }
        } else if (!s.soft.empty() && s.levels.size() >= 2) {
            const float span = std::max(1e-6f, s.levels.back() - s.levels.front());
            // soft values against time: the eye of the slicer
            for (size_t i = 0; i < s.soft.size() && drawn < 6000; i++, drawn++) {
                const float x = p0.x + (float)(drawn % 600) / 600.f * sz, y = p0.y + sz / 2 - (s.soft[i] - (s.levels.back() + s.levels.front()) / 2) / span * sz * 0.6f;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 2, y + 2), dot);
            }
        }
        if (drawn > 6000) break;
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    double evm = 0, jit = 0; size_t n = 0;
    for (const sl::Symbols& s : A.symbols) { evm += s.evm * (double)s.sym.size(); jit += s.timingJitter * (double)s.sym.size(); n += s.sym.size(); }
    if (n) {
        ImGui::Text(tr("%zu symbols"), n);
        ImGui::Text(tr("EVM %.0f %%"), evm / (double)n * 100);
        ImGui::Text(tr("timing jitter %.3f symbols"), jit / (double)n);
    }
    ImGui::TextDisabled("%s", A.symbols.empty() || A.symbols[0].points.empty() ? tr("soft values of the slicer") : tr("constellation after the carrier loop"));
    ImGui::EndGroup();
}

void rateTab() {
    const sl::RateResult& r = S.A.rate;
    ImGui::Text(tr("Symbol rate %.3f Bd, confidence %.0f %%"), r.baud, r.confidence * 100);
    if (ImGui::BeginTable("##slrate", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn(tr("Method"));
        ImGui::TableSetupColumn(tr("Rate (Bd)"));
        ImGui::TableSetupColumn(tr("Strength"));
        ImGui::TableHeadersRow();
        for (const sl::RateEstimate& e : r.methods) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(sl::rateMethodName(e.method));
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", e.baud);
            ImGui::TableSetColumnIndex(2); ImGui::ProgressBar((float)e.strength, ImVec2(90 * gUi, 0));
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", tr("Estimates within 2 % vote together; spectral lines at twice or three times a rate support it. Set the rate under Options to override."));
}

void lineTab() {
    const sl::Analysis& A = S.A;
    ImGui::TextWrapped("%s", tr("The decodings tried, most structured first. Two-chip codes (Manchester, Miller) need valid chip pairs. Pick one to decode the frames again with it."));
    const bool busy = S.run == 1;
    if (ImGui::BeginTable("##slline", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 200 * gUi))) {
        ImGui::TableSetupColumn(tr("Line code"));
        ImGui::TableSetupColumn(tr("Descrambler"));
        ImGui::TableSetupColumn(tr("Chip violations"));
        ImGui::TableSetupColumn(tr("Score"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int i = 0;
        for (const sl::LineCodeGuess& g : A.lineCodes) {
            if (i >= 30) break;
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            const bool cur = g.code == A.line.code && g.scrambler == A.line.scrambler;
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(cur ? pal::accent() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", sl::lineCodeName(g.code));
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(g.scrambler >= 0 ? sl::scramblers()[(size_t)g.scrambler].name : "-");
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f %%", g.violations * 100);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", g.score);
            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(tr("Use"))) { const sl::LineCodeGuess gg = g; modify(tr("decoding"), [gg](sl::Analysis& a) { sl::applyLineCode(a, gg); }); }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Invert the bits"))) { sl::LineCodeGuess g = A.line; g.inverted = !g.inverted; modify(tr("decoding"), [g](sl::Analysis& a) { sl::applyLineCode(a, g); }); }
    ImGui::SameLine();
    static int addSc = 0;
    ImGui::SetNextItemWidth(220 * gUi);
    std::string items;
    for (const auto& s : sl::scramblers()) { items += s.name; items += '\0'; }
    ImGui::Combo("##slsc", &addSc, items.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Descramble with it"))) { sl::LineCodeGuess g = A.line; g.scrambler = addSc; modify(tr("decoding"), [g](sl::Analysis& a) { sl::applyLineCode(a, g); }); }
    ImGui::EndDisabled();
}

void framesTab() {
    const sl::Analysis& A = S.A;
    const bool busy = S.run == 1;
    ImGui::Text(tr("Preamble %zu bits; %zu frames of %zu bits%s"), A.frame.preambleBits, A.frame.frames.size(), A.frame.frameBits, A.frame.variableLength ? tr(" (lengths vary)") : "");
    if (!A.frame.autocorrPeaks.empty()) {
        std::string s;
        for (size_t l : A.frame.autocorrPeaks) s += std::to_string(l) + " ";
        ImGui::TextDisabled(tr("Bit-stream autocorrelation peaks at lags %s"), s.c_str());
    }
    ImGui::SetNextItemWidth(-260 * gUi);
    ImGui::InputText(tr("Sync word (bits)"), S.syncText, sizeof S.syncText);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputInt(tr("Length"), &S.frameLen, 0);
    S.frameLen = std::max(0, S.frameLen);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Re-frame"))) { const sl::Bits sy = sl::stringToBits(S.syncText); const size_t fl = (size_t)S.frameLen; modify(tr("framing"), [sy, fl](sl::Analysis& a) { sl::reframe(a, sy, fl); }); }
    ImGui::EndDisabled();
    if (ImGui::BeginTable("##slsync", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 130 * gUi))) {
        ImGui::TableSetupColumn(tr("Sync word candidate"));
        ImGui::TableSetupColumn(tr("Hex"));
        ImGui::TableSetupColumn(tr("Hits"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int i = 0;
        for (const sl::SyncCandidate& c : A.frame.syncCandidates) {
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(sl::bitsToString(c.pattern).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(sl::bitsToHex(c.pattern).c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", c.hits);
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(tr("Use"))) { const sl::Bits sy = c.pattern; modify(tr("framing"), [sy](sl::Analysis& a) { sl::reframe(a, sy, 0); }); }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (A.checks.empty()) ImGui::TextWrapped("%s", tr("No checksum from the catalogue fits the frames (CRC-8/16/32 variants, Fletcher-16, sums, XOR; every start, position, bit and byte order, both polarities). The bits may still hold errors, or the frames are whitened another way."));
    for (const sl::ChecksumHit& h : A.checks) {
        const sl::CheckAlgo& c = sl::checkAlgos()[(size_t)h.algo];
        if (h.lengthFromEnd) ImGui::BulletText(tr("%s at the end of each frame, over bits %s%zu.., %s first, %s, %d of %d frames"), c.name, h.coversSync ? "sync+" : "", h.coverStart,
                                               h.lsbFirst ? "LSB" : "MSB", h.littleEndian ? "little-endian" : "big-endian", h.matched, h.tested);
        else ImGui::BulletText(tr("%s over bits %s%zu..%zu, at bit %zu, %s first, %s, %d of %d frames"), c.name, h.coversSync ? "sync+" : "", h.coverStart, h.coverEnd, h.coverEnd,
                               h.lsbFirst ? "LSB" : "MSB", h.littleEndian ? "little-endian" : "big-endian", h.matched, h.tested);
    }
}

void interpretations(const sl::Bits& f, size_t s, size_t n) {
    if (n == 0 || s >= f.size()) return;
    n = std::min(n, f.size() - s);
    ImGui::Text(tr("Frame %d, bits %zu..%zu (%zu bits)"), S.selFrame, s, s + n - 1, n);
    ImGui::Checkbox(tr("LSB first"), &S.lsbFirst);
    ImGui::Text("bits     %s", sl::interpretBits(f, s, n, 4).c_str());
    ImGui::Text("hex      %s", sl::interpretHex(f, s, n, S.lsbFirst).c_str());
    if (n <= 64) {
        ImGui::Text("unsigned %s", sl::interpretDecimal(f, s, n, S.lsbFirst, false).c_str());
        ImGui::Text("signed   %s", sl::interpretDecimal(f, s, n, S.lsbFirst, true).c_str());
    }
    if (n >= 8) ImGui::Text("ASCII    %s", sl::interpretAscii(f, s, n, S.lsbFirst).c_str());
    // the same bits across the frames
    if (ImGui::TreeNode(trw("This field in every frame"))) {
        for (size_t i = 0; i < S.A.frame.frames.size() && i < 64; i++) {
            const sl::Bits& g = S.A.frame.frames[i];
            if (s + n > g.size()) continue;
            ImGui::Text("%3zu  %s  %s", i, sl::interpretHex(g, s, n, S.lsbFirst).c_str(), n <= 32 ? sl::interpretDecimal(g, s, n, S.lsbFirst).c_str() : "");
        }
        ImGui::TreePop();
    }
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputText("##slfname", S.fieldName, sizeof S.fieldName);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo("##slftype", &S.fieldType, "constant\0counter\0checksum\0ascii\0enum\0uint\0bytes\0");
    ImGui::SameLine();
    if (ImGui::Button(tr("Make this a field"))) {
        std::lock_guard<std::mutex> lk(S.mu);
        sl::Field fd;
        fd.name = S.fieldName; fd.start = s; fd.length = n; fd.type = (sl::FieldType)std::clamp(S.fieldType, 0, 6); fd.value = sl::interpretHex(f, s, n);
        auto& fl = S.A.fields;
        // the new field replaces what it overlaps
        fl.erase(std::remove_if(fl.begin(), fl.end(), [&](const sl::Field& o) { return o.start < s + n && s < o.start + o.length; }), fl.end());
        fl.push_back(fd);
        std::sort(fl.begin(), fl.end(), [](const sl::Field& a, const sl::Field& b) { return a.start < b.start; });
    }
}

void bitViewerTab() {
    sl::Analysis& A = S.A;
    const auto& fr = A.frame.frames;
    if (fr.empty()) { ImGui::TextUnformatted(tr("No frames yet.")); return; }
    ImGui::RadioButton(tr("Heat: how often a bit changes"), &S.heat, 1);
    ImGui::SameLine();
    ImGui::RadioButton(tr("Heat: entropy"), &S.heat, 0);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Diff two frames"), &S.diffOn);
    if (S.diffOn) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        ImGui::InputInt("##sldA", &S.diffA);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        ImGui::InputInt("##sldB", &S.diffB);
        S.diffA = std::clamp(S.diffA, 0, (int)fr.size() - 1);
        S.diffB = std::clamp(S.diffB, 0, (int)fr.size() - 1);
    }
    size_t L = 0;
    for (const auto& f : fr) L = std::max(L, f.size());
    const float cell = std::max(5.f, 7.f * gUi), headH = 3 * cell;
    const float W = (float)L * cell + 40 * gUi, H = headH + (float)fr.size() * cell + 4;
    ImGui::BeginChild("##slgrid", ImVec2(0, std::min(H + 20 * gUi, 330 * gUi)), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slcells", ImVec2(W, H));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = o.x + 36 * gUi;
    const ImVec2 clipMin = ImGui::GetWindowPos(), clipMax = ImVec2(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);
    // fields band and heat map
    for (const sl::Field& f : A.fields) {
        const float a = x0 + (float)f.start * cell, b = x0 + (float)(f.start + f.length) * cell;
        dl->AddRectFilled(ImVec2(a, o.y), ImVec2(b - 1, o.y + cell), fieldColor(f.type));
        dl->AddLine(ImVec2(a, o.y), ImVec2(a, o.y + H), IM_COL32(255, 255, 255, 60));
    }
    for (size_t k = 0; k < A.columns.size() && k < L; k++) {
        const double v = S.heat ? A.columns[k].change * 2 : A.columns[k].entropy;
        dl->AddRectFilled(ImVec2(x0 + (float)k * cell, o.y + cell + 1), ImVec2(x0 + (float)(k + 1) * cell - 1, o.y + headH - 2), heatColor(v));
    }
    const sl::Bits df = S.diffOn ? sl::diffFrames(fr[(size_t)S.diffA], fr[(size_t)S.diffB]) : sl::Bits();
    const int r0 = std::max(0, (int)((clipMin.y - o.y - headH) / cell) - 1), r1 = std::min((int)fr.size(), (int)((clipMax.y - o.y - headH) / cell) + 2);
    const int c0 = std::max(0, (int)((clipMin.x - x0) / cell) - 1), c1 = std::min((int)L, (int)((clipMax.x - x0) / cell) + 2);
    const int sa = std::min(S.selA, S.selB), sb = std::max(S.selA, S.selB);
    for (int r = r0; r < r1; r++) {
        const sl::Bits& f = fr[(size_t)r];
        const float y = o.y + headH + (float)r * cell;
        char t[16];
        snprintf(t, sizeof t, "%d", r);
        dl->AddText(ImVec2(o.x, y - 2), r == S.selFrame ? ImGui::GetColorU32(pal::accent()) : IM_COL32(150, 150, 160, 255), t);
        const bool inDiff = S.diffOn && (r == S.diffA || r == S.diffB);
        for (int c = c0; c < c1 && c < (int)f.size(); c++) {
            ImU32 col = f[(size_t)c] ? IM_COL32(225, 230, 240, 255) : IM_COL32(45, 52, 64, 255);
            if (inDiff && (size_t)c < df.size() && df[(size_t)c]) col = f[(size_t)c] ? IM_COL32(255, 120, 110, 255) : IM_COL32(150, 40, 40, 255);
            dl->AddRectFilled(ImVec2(x0 + (float)c * cell, y), ImVec2(x0 + (float)(c + 1) * cell - 1, y + cell - 1), col);
        }
        if (r == S.selFrame && sa >= 0) dl->AddRect(ImVec2(x0 + (float)sa * cell - 1, y - 1), ImVec2(x0 + (float)(sb + 1) * cell, y + cell), ImGui::GetColorU32(pal::accent()), 0, 0, 2);
    }
    // click / drag selects bits of a frame; the tooltip names the field and the column statistics
    if (hov) {
        const ImVec2 m = ImGui::GetMousePos();
        const int c = (int)std::floor((m.x - x0) / cell), r = (int)std::floor((m.y - o.y - headH) / cell);
        if (c >= 0 && c < (int)L) {
            std::string tip;
            for (const sl::Field& f : A.fields) if ((size_t)c >= f.start && (size_t)c < f.start + f.length) { tip = f.name + " (" + sl::fieldTypeName(f.type) + ")\n"; break; }
            if ((size_t)c < A.columns.size()) {
                char b[120];
                snprintf(b, sizeof b, tr("bit %d: ones %.0f %%, entropy %.2f, changes %.0f %%"), c, A.columns[(size_t)c].p1 * 100, A.columns[(size_t)c].entropy, A.columns[(size_t)c].change * 100);
                tip += b;
            }
            ImGui::SetTooltip("%s", tip.c_str());
            if (r >= 0 && r < (int)fr.size()) {
                if (ImGui::IsMouseClicked(0)) { S.selFrame = r; S.selA = S.selB = c; S.dragging = true; }
                else if (S.dragging && ImGui::IsMouseDown(0)) S.selB = c;
            }
        }
    }
    if (!ImGui::IsMouseDown(0)) S.dragging = false;
    ImGui::EndChild();
    if (S.diffOn) {
        size_t n = 0;
        std::string at;
        for (size_t i = 0; i < df.size(); i++) if (df[i]) { n++; if (at.size() < 200) at += std::to_string(i) + " "; }
        ImGui::Text(tr("Frames %d and %d differ in %zu bits: %s"), S.diffA, S.diffB, n, at.c_str());
    }
    if (S.selA >= 0 && S.selFrame < (int)fr.size()) interpretations(fr[(size_t)S.selFrame], (size_t)std::min(S.selA, S.selB), (size_t)(std::abs(S.selB - S.selA) + 1));
    else ImGui::TextDisabled("%s", tr("Click or drag across the bits of a frame to read them as hex, numbers or text."));
    // the fields, editable
    if (ImGui::TreeNodeEx(trw("Fields"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##slfields", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn(tr("Name"));
            ImGui::TableSetupColumn(tr("Bits"));
            ImGui::TableSetupColumn(tr("Type"));
            ImGui::TableSetupColumn(tr("Frame 0"));
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int del = -1;
            for (size_t i = 0; i < A.fields.size(); i++) {
                sl::Field& f = A.fields[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char nb[48];
                snprintf(nb, sizeof nb, "%s", f.name.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##n", nb, sizeof nb)) f.name = nb;
                ImGui::TableSetColumnIndex(1); ImGui::Text("%zu..%zu", f.start, f.start + f.length - 1);
                ImGui::TableSetColumnIndex(2);
                int t = (int)f.type;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##t", &t, "constant\0counter\0checksum\0ascii\0enum\0uint\0bytes\0")) f.type = (sl::FieldType)t;
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(f.value.c_str());
                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton("x")) del = (int)i;
                ImGui::PopID();
            }
            if (del >= 0) A.fields.erase(A.fields.begin() + del);
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

void exportTab() {
    sl::Analysis& A = S.A;
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputText(tr("Name"), S.name, sizeof S.name)) A.name = S.name;
    ImGui::TextUnformatted(tr("Notes (saved with the analysis and in the Mode Builder description)"));
    struct Cb { static int resize(ImGuiInputTextCallbackData* d) { if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) { auto* s = static_cast<std::string*>(d->UserData); s->resize((size_t)d->BufTextLen); d->Buf = s->data(); } return 0; } };
    if (ImGui::InputTextMultiline("##slnotes", S.notes.data(), S.notes.capacity() + 1, ImVec2(-1, 90 * gUi), ImGuiInputTextFlags_CallbackResize, Cb::resize, &S.notes)) A.notes = S.notes;
    A.name = S.name; A.notes = S.notes;
    static std::string msg;
    if (ImGui::Button(tr("Mode Builder description..."))) msg = saveText("signal.onair-mode.json", sl::toModeJson(A)) ? tr("Saved") : "";
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Symbol rate, modulation, line code, sync word, frame length, fields and checksum as JSON (docs/SIGNAL-LAB.md); Mode Builder turns it into a decoder."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy it"))) { ImGui::SetClipboardText(sl::toModeJson(A).c_str()); msg = tr("Copied"); }
    if (ImGui::Button(tr("Frames as CSV..."))) msg = saveText("frames.csv", sl::toCsv(A)) ? tr("Saved") : "";
    ImGui::SameLine();
    if (ImGui::Button(tr("Hex dump..."))) msg = saveText("frames.txt", sl::toHexDump(A)) ? tr("Saved") : "";
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy the hex dump"))) { ImGui::SetClipboardText(sl::toHexDump(A).c_str()); msg = tr("Copied"); }
    if (ImGui::Button(tr("Save the analysis..."))) msg = saveText("signal-lab-analysis.json", sl::saveAnalysisJson(A)) ? tr("Saved") : "";
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The description plus the frames, the decoded streams, the log and your notes; load it again under Saved analysis."));
    if (!msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", msg.c_str()); }
}

void evaluationTab() {
    ImGui::TextWrapped("%s", tr("Runs the lab on many made-up signals (FSK, GMSK, OOK, PSK with line codes, scramblers, preambles, CRCs, noise and offsets) and counts how often each stage gets the truth back."));
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputInt(tr("Signals"), &S.evalCases);
    S.evalCases = std::clamp(S.evalCases, 1, 1000);
    ImGui::SameLine();
    ImGui::BeginDisabled(S.run == 1);
    if (ImGui::Button(tr("Run the evaluation"))) {
        const int n = S.evalCases;
        startWork(tr("evaluating"), [n] { const sl::EvalReport r = sl::evaluate(n, 1, true); std::lock_guard<std::mutex> lk(S.mu); S.evalText = r.text; });
    }
    ImGui::EndDisabled();
    if (!S.evalText.empty()) {
        ImGui::BeginChild("##sleval", ImVec2(0, 260 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(S.evalText.c_str());
        ImGui::EndChild();
        if (ImGui::Button(tr("Copy the report"))) ImGui::SetClipboardText(S.evalText.c_str());
    }
}

const double kLrBw[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
const char* const kLrBwNames = "7.8 kHz\0" "10.4 kHz\0" "15.6 kHz\0" "20.8 kHz\0" "31.25 kHz\0" "41.7 kHz\0" "62.5 kHz\0" "125 kHz\0" "250 kHz\0" "500 kHz\0" "custom\0";

dect2::lora::LabSettings lrSettings() {
    dect2::lora::LabSettings s;
    s.sf = std::clamp(S.lrSf, 5, 12);
    s.bwHz = S.lrBwIdx >= 0 && S.lrBwIdx < 10 ? kLrBw[S.lrBwIdx] : std::max(1000.0, S.lrBwHz);
    s.cr = std::clamp(S.lrCr, 5, 8);
    s.syncWord = S.lrSync & 0xFF;
    s.implicitHdr = S.lrImplicit;
    s.implicitLen = std::clamp(S.lrImplicitLen, 1, 255);
    s.crc = S.lrCrc;
    s.ldro = S.lrLdro - 1;   // combo: automatic, off, on
    s.invertIq = S.lrInvert;
    s.preamble = std::clamp(S.lrPreamble, 4, 64);
    s.offsetHz = S.lrOffset;
    return s;
}

void lrApply(const dect2::lora::LabSettings& s) {
    S.lrSf = s.sf;
    S.lrBwIdx = 10; S.lrBwHz = s.bwHz;
    for (int i = 0; i < 10; i++) if (std::fabs(kLrBw[i] - s.bwHz) < 0.5) S.lrBwIdx = i;
    S.lrCr = s.cr; S.lrSync = s.syncWord; S.lrCrc = s.crc; S.lrInvert = s.invertIq; S.lrLdro = s.ldro + 1; S.lrOffset = s.offsetHz; S.lrImplicit = false;
}

// Reads the chosen source into samples at their rate; false with S.err set
bool lrSamples(App& a, std::vector<dect2::cf32>& x, double& fs, double& selOffset) {
    selOffset = 0;
    if (S.lrSource == 1) {
        dect2::sigid::Capture::Kept k;
        uint64_t seq = 0;
        if (!a.engine.sigid().keptSamples(k, seq) || k.x.empty()) { S.err = tr("No selection yet: select the signal on the spectrum first."); return false; }
        x = std::move(k.x); fs = k.rateHz; selOffset = k.centreOffHz;
        return true;
    }
    return false;
}

void loraTab(App& a) {
    const bool busy = S.run == 1;
    ImGui::TextWrapped("%s", tr("Demodulates LoRa (chirp spread spectrum) frames from the Recording or the last spectrum selection with the settings below, or finds the settings itself. Receive only: nothing is decrypted."));
    ImGui::RadioButton(tr("Recording (the file of the Recording tab)"), &S.lrSource, 0);
    ImGui::SameLine();
    ImGui::RadioButton(tr("Last spectrum selection"), &S.lrSource, 1);
    if (S.lrSource == 0) ImGui::TextDisabled("%s %s @ %.0f Hz", tr("File:"), S.path[0] ? S.path : tr("(none, pick one in the Recording tab)"), S.rate);
    ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt(tr("Spreading factor"), &S.lrSf, 0, 0); S.lrSf = std::clamp(S.lrSf, 5, 12);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo(tr("Bandwidth"), &S.lrBwIdx, kLrBwNames);
    if (S.lrBwIdx == 10) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ImGui::InputDouble("##lrbw", &S.lrBwHz, 0, 0, "%.0f Hz"); }
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::Combo(tr("Coding rate"), &S.lrCr, "\0\0\0\0\0""4/5\0""4/6\0""4/7\0""4/8\0");
    S.lrCr = std::clamp(S.lrCr, 5, 8);
    if (!S.lrImplicit && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only used with the implicit header: an explicit header carries the coding rate."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt(tr("Sync word (hex)"), &S.lrSync, 0, 0, ImGuiInputTextFlags_CharsHexadecimal); S.lrSync &= 0xFF;
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::Combo(tr("Low data rate optimisation"), &S.lrLdro, "automatic\0off\0on\0");
    ImGui::Checkbox(tr("Implicit header"), &S.lrImplicit);
    if (S.lrImplicit) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt(tr("Payload bytes"), &S.lrImplicitLen, 0, 0);
        ImGui::SameLine(); ImGui::Checkbox(tr("CRC"), &S.lrCrc);
    }
    ImGui::SameLine(); ImGui::Checkbox(tr("Inverted IQ"), &S.lrInvert);
    ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt(tr("Preamble chirps"), &S.lrPreamble, 0, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble(tr("Offset from the centre (Hz)"), &S.lrOffset, 0, 0, "%.0f");
    ImGui::Checkbox(tr("Auto-detect also measures the offset"), &S.lrAutoOffset);
    const bool haveSrc = S.lrSource == 1 || S.path[0];
    ImGui::BeginDisabled(busy || !haveSrc);
    const bool run = ImGui::Button(tr("Demodulate"));
    ImGui::SameLine();
    const bool det = ImGui::Button(tr("Auto-detect parameters"));
    ImGui::EndDisabled();
    if (run || det) {
        const dect2::lora::LabSettings ls = lrSettings();
        const bool detect = det, autoOff = S.lrAutoOffset;
        const int src = S.lrSource;
        const std::string path = S.path;
        static const dect2::protolab::IqFormat fmts[] = {dect2::protolab::IqFormat::Cf32, dect2::protolab::IqFormat::Cs16, dect2::protolab::IqFormat::Cu8, dect2::protolab::IqFormat::Cs8};
        const auto fmt = fmts[std::clamp(S.fmt, 0, 3)];
        const double rate = S.rate, fileOff = S.offset;
        std::vector<dect2::cf32> sel;
        double selRate = 0, selOff = 0;
        bool ok = true;
        if (src == 1) { std::lock_guard<std::mutex> lk(S.mu); S.err.clear(); ok = lrSamples(a, sel, selRate, selOff); }
        if (ok) startWork(detect ? tr("searching the LoRa parameters") : tr("demodulating LoRa"), [=, sel = std::move(sel)]() mutable {
            std::vector<dect2::cf32> x;
            double fs = rate, off = fileOff;
            if (src == 1) { x = std::move(sel); fs = selRate; off = selOff; }
            else {
                std::vector<dect2::protolab::cf> iq;
                std::string err;
                if (!dect2::protolab::loadIq(path, fmt, iq, err, 30000000)) { std::lock_guard<std::mutex> lk(S.mu); S.err = err; return; }
                x.assign(iq.begin(), iq.end());
            }
            if (detect) {
                dect2::lora::LabDetectOptions o;
                o.autoOffset = autoOff; o.offsetHz = ls.offsetHz + (src == 1 ? off : 0);
                const dect2::lora::LabDetect d = dect2::lora::labAutoDetect(x.data(), x.size(), fs, o);
                dect2::lora::LabResult r;
                if (d.found) r = dect2::lora::labDemodulate(x.data(), x.size(), fs, d.settings);
                std::lock_guard<std::mutex> lk(S.mu);
                S.lrDet = d; S.lrNote = d.note; S.lrFrames = r.frames; S.lrStats = r.stats;
                if (d.found) lrApply(d.settings);
            } else {
                dect2::lora::LabSettings s2 = ls;
                if (src == 1) s2.offsetHz += off;
                const dect2::lora::LabResult r = dect2::lora::labDemodulate(x.data(), x.size(), fs, s2);
                char b[200];
                snprintf(b, sizeof b, "%zu frame(s); preambles %llu, bad sync word %llu, bad header %llu, CRC failures %llu", r.frames.size(), (unsigned long long)r.stats.preambles,
                         (unsigned long long)r.stats.syncBad, (unsigned long long)r.stats.headerBad, (unsigned long long)r.stats.crcBad);
                std::lock_guard<std::mutex> lk(S.mu);
                S.lrDet = dect2::lora::LabDetect(); S.lrNote = b; S.lrFrames = r.frames; S.lrStats = r.stats;
            }
        });
    }
    if (!S.lrNote.empty()) ImGui::TextWrapped("%s", S.lrNote.c_str());
    if (!S.lrDet.candidates.empty()) {
        ImGui::TextDisabled("%s", tr("Combinations that heard a preamble (best first):"));
        int shown = 0;
        for (const auto& c : S.lrDet.candidates) {
            if (++shown > 6) break;
            ImGui::BulletText("SF%d, %.1f kHz: %d frame(s), %d with a good CRC, %d preamble(s)", c.sf, c.bwHz / 1e3, c.frames, c.crcOk, c.preambles);
        }
    }
    if (!S.lrFrames.empty() && ImGui::BeginTable("##lrframes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (s)"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("CR"), ImGuiTableColumnFlags_WidthFixed, 36 * gUi);
        ImGui::TableSetupColumn(tr("CRC"), ImGuiTableColumnFlags_WidthFixed, 54 * gUi);
        ImGui::TableSetupColumn(tr("SNR"), ImGuiTableColumnFlags_WidthFixed, 54 * gUi);
        ImGui::TableSetupColumn(tr("Offset"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("Hex"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Text"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& f : S.lrFrames) {
            std::string hex, txt;
            char b[8];
            for (uint8_t c : f.payload) { snprintf(b, sizeof b, "%02X ", c); hex += b; txt += (c >= 32 && c < 127) ? (char)c : '.'; }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", f.startSec);
            ImGui::TableNextColumn(); ImGui::Text("4/%d", f.cr);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(!f.crcPresent ? "none" : f.crcOk ? "good" : "BAD");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", f.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f", f.cfoHz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hex.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(txt.c_str());
        }
        ImGui::EndTable();
    }
    if (!S.lrFrames.empty() && ImGui::Button(tr("Copy the frames as hex"))) {
        std::string t;
        char b[8];
        for (const auto& f : S.lrFrames) { for (uint8_t c : f.payload) { snprintf(b, sizeof b, "%02X", c); t += b; } t += "\n"; }
        ImGui::SetClipboardText(t.c_str());
    }
}

void windowPanel(App& a) {
    firstWindowSize(820 * gUi, 680 * gUi);
    if (!ImGui::Begin(trw("Signal Lab"), &a.signallabOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Works out an unknown digital signal step by step: channel, modulation, symbol rate, symbols, line code, frames, checksum and fields. Every stage shows what it found and how sure it is."));
    inputPanel(a);
    if (S.run == 1) {
        std::string t;
        { std::lock_guard<std::mutex> lk(S.mu); t = S.busyText; }
        ImGui::TextColored(pal::accent(), "%s...", t.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (!S.err.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", S.err.c_str());
    }
    ImGui::Separator();
    if (S.run == 1) { ImGui::End(); return; }   // the worker owns the analysis while it runs
    if (ImGui::BeginTabBar("##slres")) {
        if (S.have) {
            if (ImGui::BeginTabItem(trw("Summary"))) { summaryTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("1 Channel"))) { channelTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("2 Modulation"))) { modulationTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("3 Symbol rate"))) { rateTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("4 Line code"))) { lineTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("5 Frames"))) { framesTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("6 Bit viewer"))) { bitViewerTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Export"))) { exportTab(); ImGui::EndTabItem(); }
        }
        if (ImGui::BeginTabItem(trw("LoRa demodulator"))) { loraTab(a); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Evaluation"))) { evaluationTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
} // namespace

void signallabWindow(App& a) {
    if (a.rxId != 1) {   // one lab for the program: RX 1's window
        if (App* f = receiverById(1)) { f->signallabOpen = a.signallabOpen; signallabWindow(*f); a.signallabOpen = f->signallabOpen; }
        return;
    }
    // keep the samples of the spectrum selection only while the lab is open (up to 32 MB)
    if (a.signallabOpen != S.keepOn) { S.keepOn = a.signallabOpen; a.engine.sigid().keepSamples(S.keepOn); }
    if (a.signallabOpen) windowPanel(a);
    else if (S.run != 1 && S.worker.joinable()) S.worker.join();
}
