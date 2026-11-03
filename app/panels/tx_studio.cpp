// Tools > TX studio: a bench for making and checking a transmit signal WITHOUT a radio. Pick a mode, edit its parameters and payload, see the signal
// (spectrum, waterfall, constellation, time view), read the measurements (occupied bandwidth, PAPR, ACPR / mask), run the app's own decoder on it with
// an optional noise / offset / fading impairment, and save it as an IQ file with a metadata file (or sweep a parameter into many files).
// The signal comes from the same generators and output stage as the Transmit panel (makeTxGenerator), but nothing here opens a radio or transmits,
// and none of the Arm / licence / band-plan code is touched. The engine is core/src/tx/tx_studio.cpp, the measurements core/src/tx/tx_measure.cpp.
#include "tx_state.h"
#include "../ui/typography.h"
#include "../ui/icons.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/iq_file.h"
#include "dect2/tx/tx_studio.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>

using namespace txui;
using dect2::StudioFormat;
using dect2::StudioMeasure;
using dect2::StudioSignal;

namespace {

// What the plots show: made on the worker thread from the (impaired) signal
struct Shown {
    std::shared_ptr<StudioSignal> clean, sig;   // sig: clean with the impairments (the same object when there are none)
    StudioMeasure m;
    std::vector<double> specX, specY, maskX, maskY;   // kHz from the centre of the span; dB re the mean density inside the occupied band
    int wfRows = 0, wfCols = 0;
    std::vector<float> wf;                      // rows x cols, dB re the same reference
    std::string err;
    uint64_t gen = 0;
};

struct Studio {
    TxUi ui;                                    // the payload / parameter state of the studio (the Transmit panel keeps its own)
    const TxModeSpec* mode = nullptr;
    char search[64] = {};
    double seconds = 2, rateKhz = 0;            // rateKhz 0: the mode's own rate
    double centerMhz = 0;                       // metadata only
    bool autoUpdate = true;
    // impairments
    double snr = 30, cfo = 0, doppler = 5, delayUs = 5;
    bool useSnr = false, useCfo = false;
    int fading = 0;
    // result
    Shown shown;
    std::future<Shown> job;
    bool jobRunning = false, again = false, regen = true;
    std::string genKey, impKey, shownGenKey;
    double changedAt = 0;
    uint64_t genCount = 0;
    // loopback
    std::future<dect2::StudioLoopback> lbJob;
    bool lbRunning = false;
    std::atomic<bool> lbCancel{false};
    dect2::StudioLoopback lb;
    // view
    float tStart = 0, tSpanLog = 3.2f;          // time view: start (0..1) and log10 of the span in samples
    float symSpan = 0;                          // constellation: samples per symbol (0: every sample)
    // save / batch
    int fmt = 4;
    char folder[512] = {}, base[96] = {};
    bool saveImpaired = false;
    std::future<std::string> saveJob;
    bool saving = false, openAfter = false;
    std::string saveMsg, lastData, lastMeta;
    StudioFormat lastFmt = StudioFormat::SigMF;
    char sweepKey[48] = "wpm", sweepVals[96] = "10, 20, 30";
    std::future<std::string> batchJob;
    bool batching = false;
    std::atomic<size_t> batchDone{0}, batchTotal{0};
    std::atomic<bool> batchCancel{false};
    std::string batchMsg;
    bool init = false;
};
Studio gS;

std::string keyOfReq(const dect2::TxRequest& r, const Studio& s, double rate) {
    char b[512];
    snprintf(b, sizeof b, "%s|%.0f|%.3f|%.3f|%s|%s|%s|%s|%s|%s|%s|%s|%s|%d %d %d %d %d %d %d %d %u %.1f %.4f %.4f %d %.0f %.0f %d %d %d %d %d %d", r.modeId.c_str(), rate, s.seconds, r.bandwidthMhz, r.callsign.c_str(),
             r.grid.c_str(), r.text.c_str(), r.dest.c_str(), r.path.c_str(), r.file.c_str(), r.name.c_str(), r.provider.c_str(), r.network.c_str(), r.kind, r.baudIdx, r.shiftIdx, r.variant, r.wpm, r.dbm,
             r.function, r.serviceId, r.address, r.toneHz, r.lat, r.lon, (int)r.reverse, r.deviationHz, r.preemphUs, r.rdsPi, r.slot, r.colorCode, r.talkgroup, (int)r.analogVoice, r.audioSrc);
    std::string k = b;
    for (const auto& sv : r.services) k += sv + ",";
    for (const auto& kv : r.opt) { snprintf(b, sizeof b, " %s=%.5f", kv.first.c_str(), kv.second); k += b; }
    for (int v : r.synth.modeOpt) k += " " + std::to_string(v);
    return k;
}

dect2::txm::Impair impairOf(const Studio& s) {
    dect2::txm::Impair im;
    im.snrDb = s.useSnr ? s.snr : 99;
    im.cfoHz = s.useCfo ? s.cfo : 0;
    im.fading = s.fading;
    im.dopplerHz = s.doppler;
    im.delayUs = s.delayUs;
    return im;
}

std::string impKeyOf(const Studio& s) {
    char b[160];
    snprintf(b, sizeof b, "%d %.2f %d %.1f %d %.2f %.2f", (int)s.useSnr, s.snr, (int)s.useCfo, s.cfo, s.fading, s.doppler, s.delayUs);
    return b;
}

// the plots' data from a signal
void buildDisplay(Shown& sh, const std::string& modeId) {
    using namespace dect2;
    const StudioSignal& s = *sh.sig;
    sh.m = studioMeasure(s, modeId.c_str());
    if (s.iq.size() < 64) return;
    const auto psd = txm::welchPsd(s.iq.data(), s.iq.size(), s.rateHz, 4096, 48);
    const double ref = sh.m.maskResult.refDb != 0 ? sh.m.maskResult.refDb : (psd.empty() ? 0 : 10 * std::log10(std::max(1e-20, 1.0 / (double)psd.pw.size())));
    sh.specX.resize(psd.pw.size());
    sh.specY.resize(psd.pw.size());
    for (size_t i = 0; i < psd.pw.size(); i++) { sh.specX[i] = psd.freqOf(i) / 1e3; sh.specY[i] = psd.db(i) - ref; }
    const double c = sh.m.centroidHz;
    const auto& pts = sh.m.mask.pts;
    for (int side = -1; side <= 1; side += 2)   // the mask as one polyline: left edge inward, then right side outward (the last point runs to the span's edge)
        for (size_t k = 0; k < pts.size(); k++) {
            const size_t j = side < 0 ? pts.size() - 1 - k : k;
            const double off = j + 1 == pts.size() ? std::max(pts[j].offsetHz, s.rateHz / 2) : pts[j].offsetHz;
            sh.maskX.push_back((c + side * off) / 1e3);
            sh.maskY.push_back(pts[j].limitDb);
        }
    // waterfall: 96 rows over the whole signal, 256 columns (the loudest bin of each group)
    constexpr int kRows = 96, kCols = 256, kF = 1024;
    sh.wfRows = kRows; sh.wfCols = kCols;
    sh.wf.assign((size_t)kRows * kCols, -120.f);
    const size_t L = 2048;
    for (int r = 0; r < kRows && s.iq.size() >= L; r++) {
        const size_t at = (size_t)((double)(s.iq.size() - L) * r / (kRows - 1));
        const auto row = txm::welchPsd(s.iq.data() + at, L, s.rateHz, kF, 1);
        for (int cidx = 0; cidx < kCols; cidx++) {
            double m = 0;
            for (int k = 0; k < kF / kCols; k++) m = std::max(m, row.pw[(size_t)(cidx * (kF / kCols) + k)]);
            sh.wf[(size_t)r * kCols + cidx] = (float)((m > 1e-20 ? 10 * std::log10(m) : -200.0) - ref);
        }
    }
}

Shown makeShown(std::shared_ptr<StudioSignal> clean, const dect2::txm::Impair& im, const std::string& modeId, uint64_t gen) {
    Shown sh;
    sh.gen = gen;
    sh.clean = clean;
    if (im.any()) {
        sh.sig = std::make_shared<StudioSignal>(*clean);
        dect2::txm::applyImpairments(sh.sig->iq, sh.sig->rateHz, im);
    } else sh.sig = clean;
    buildDisplay(sh, modeId);
    return sh;
}

void startJob(Studio& s, bool regen) {
    if (!s.mode) return;
    s.jobRunning = true;
    s.regen = false;
    const uint64_t gen = ++s.genCount;
    const dect2::TxRequest r = makeReq(s.ui, *s.mode, false);
    const dect2::txm::Impair im = impairOf(s);
    const std::string modeId = s.mode->id;
    const TxModeSpec* mode = s.mode;
    const double secs = s.seconds, rate = s.rateKhz * 1e3;
    std::shared_ptr<StudioSignal> have = regen ? nullptr : s.shown.clean;
    s.shownGenKey = keyOfReq(r, s, rate);
    s.impKey = impKeyOf(s);
    s.job = std::async(std::launch::async, [=]() {
        Shown out;
        try {
            std::shared_ptr<StudioSignal> clean = have;
            if (!clean) {
                clean = std::make_shared<StudioSignal>();
                std::string err;
                if (!dect2::studioGenerate(*mode, r, rate, secs, *clean, err)) { out.err = err; out.gen = gen; return out; }
            }
            out = makeShown(clean, im, modeId, gen);
        } catch (const std::exception& e) { out.err = e.what(); }
        return out;
    });
}

bool sliderD(const char* label, double* v, double lo, double hi, const char* fmt, int flags = 0) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, v, &lo, &hi, fmt, flags);
}

std::string bytesText(uint64_t b) {
    char t[32];
    if (b >= (1ull << 30)) snprintf(t, sizeof t, "%.2f GB", (double)b / (1 << 30));
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.1f MB", (double)b / (1 << 20));
    else snprintf(t, sizeof t, "%.1f kB", (double)b / 1024.0);
    return t;
}

std::string hzText(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3f MHz", hz / 1e6);
    else if (hz >= 1e3) snprintf(b, sizeof b, "%.2f kHz", hz / 1e3);
    else snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}

// a simple blue - green - yellow - red level scale for the waterfall (0..1)
ImU32 heat(float t) {
    t = std::max(0.f, std::min(1.f, t));
    const float r = std::max(0.f, std::min(1.f, 4 * t - 2)), g = std::max(0.f, std::min(1.f, t < 0.5f ? 4 * t : 4 - 4 * t)), b = std::max(0.f, std::min(1.f, 1.5f - 4 * t + 0.5f));
    return IM_COL32((int)(255 * r), (int)(255 * g), (int)(255 * b * (t < 0.25f ? 0.4f + 2.4f * t : 1.f)), 255);
}

void chooseStudioMode(Studio& s, const TxModeSpec& m) {
    if (s.mode == &m) return;
    s.mode = &m;
    s.ui.modeId = m.id;
    s.ui.synth = SynthConfig();
    s.ui.bwIdx = 0;
    s.ui.destKind = 1;                      // an IQ file: no radio-only notices in the shared payload panels
    paramsEnter(s.ui, m);
    const ModeTuning* mt = modeTuning(m.stdMode);
    s.centerMhz = mt && mt->defMhz > 0 ? mt->defMhz : 0;
    s.rateKhz = 0;
    s.seconds = m.nominalRateHz > 1e6 ? 0.25 : 2;
    s.regen = true;
    s.shown = Shown();
    s.lb = dect2::StudioLoopback();
    s.lastData.clear();
    s.saveMsg.clear();
    snprintf(s.base, sizeof s.base, "%s", dect2::iqFileBaseName(m.id, s.centerMhz * 1e6, m.nominalRateHz).c_str());
}

void modePicker(Studio& s) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##tssearch", tr("Search signals..."), s.search, sizeof s.search);
    auto has = [&](const TxModeSpec& m) {
        if (!s.search[0]) return true;
        std::string q = s.search, a = std::string(m.name) + " " + m.id;
        for (auto& c : q) c = (char)tolower((unsigned char)c);
        for (auto& c : a) c = (char)tolower((unsigned char)c);
        return a.find(q) != std::string::npos;
    };
    if (ImGui::BeginChild("##tsmodes", ImVec2(0, 150 * gUi), ImGuiChildFlags_Borders)) {
        for (const auto& m : dect2::txModes()) {
            if (!has(m)) continue;
            if (ImGui::Selectable(m.name, s.mode == &m)) chooseStudioMode(s, m);
        }
    }
    ImGui::EndChild();
}

// ready-made payloads for the text fields
void templates(Studio& s) {
    TxUi& u = s.ui;
    const std::string call = u.callsign[0] ? u.callsign : "N0CALL";
    const std::string grid = u.grid[0] ? u.grid : "KO24";
    ImGui::TextDisabled("%s", tr("Templates"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("CQ call"))) snprintf(u.text, sizeof u.text, "CQ CQ CQ DE %s %s K", call.c_str(), call.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Beacon"))) snprintf(u.text, sizeof u.text, "VVV DE %s %s", call.c_str(), grid.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Test pattern"))) snprintf(u.text, sizeof u.text, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789");
    ImGui::SameLine();
    if (ImGui::SmallButton("RYRY")) snprintf(u.text, sizeof u.text, "RYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Text file..."))) {
        const std::string p = openFileDialog();
        std::ifstream f(p, std::ios::binary);
        if (f) {
            std::string t((size_t)200, '\0');
            f.read(&t[0], 200);
            t.resize((size_t)f.gcount());
            for (auto& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            snprintf(u.text, sizeof u.text, "%s", t.c_str());
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The first 200 characters of a text file become the text to send"));
}

void settingsColumn(Studio& s) {
    modePicker(s);
    if (!s.mode) { ImGui::TextDisabled("%s", tr("Choose a signal above.")); return; }
    const TxModeSpec& m = *s.mode;
    ImGui::TextWrapped("%s: %s", m.name, m.generator);
    templates(s);
    payloadPanel(s.ui, m, false);
    typo::separatorText(tr("Signal"));
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble(tr("Length (s)"), &s.seconds, 0.5, 2, "%.2f");
    s.seconds = std::max(0.01, std::min(600.0, s.seconds));
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble(tr("Sample rate (kHz, 0 = the mode's own)"), &s.rateKhz, 0, 0, "%.3f");
    s.rateKhz = std::max(0.0, s.rateKhz);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble(tr("Centre frequency (MHz, file info only)"), &s.centerMhz, 0, 0, "%.6f");
    if (dect2::txModeFinite(m)) ImGui::TextDisabled("%s", tr("This signal ends by itself (a message or file): it may be shorter than the length."));
    if (ImGui::CollapsingHeader(trw("Advanced parameters"))) advancedPanel(s.ui, m, false);
}

// IQ scatter of the samples in the time window
void constellation(const Shown& sh, float size, float tStart, float span, float symSpan) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tsiq", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg), 4 * gUi);
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f);
    dl->AddLine(ImVec2(p.x + size / 2, p.y), ImVec2(p.x + size / 2, p.y + size), grid);
    dl->AddLine(ImVec2(p.x, p.y + size / 2), ImVec2(p.x + size, p.y + size / 2), grid);
    if (!sh.sig || sh.sig->iq.empty()) return;
    const auto& iq = sh.sig->iq;
    const size_t a = (size_t)((double)(iq.size() - 1) * tStart), n = std::min(iq.size() - a, (size_t)span);
    float mx = 1e-6f;
    for (size_t i = 0; i < n; i += std::max<size_t>(1, n / 4000)) mx = std::max(mx, std::abs(iq[a + i]));
    const double step = symSpan >= 1 ? symSpan : std::max(1.0, (double)n / 3000.0);
    const ImU32 c = ImGui::GetColorU32(ImVec4(0.3f, 0.7f, 1.f, 0.7f));
    int drawn = 0;
    for (double i = 0; i < (double)n && drawn < 20000; i += step, drawn++) {
        const cf32 v = iq[a + (size_t)i];
        const float x = p.x + size / 2 + v.real() / mx * (size / 2 - 3), y = p.y + size / 2 - v.imag() / mx * (size / 2 - 3);
        dl->AddRectFilled(ImVec2(x - 1, y - 1), ImVec2(x + 1, y + 1), c);
    }
}

void waterfall(const Shown& sh, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tswf", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(8, 8, 10, 255));
    if (sh.wf.empty()) return;
    const float cw = size.x / sh.wfCols, rh = size.y / sh.wfRows;
    for (int r = 0; r < sh.wfRows; r++)
        for (int c = 0; c < sh.wfCols; c++) {
            const float db = sh.wf[(size_t)r * sh.wfCols + c];
            dl->AddRectFilled(ImVec2(p.x + c * cw, p.y + r * rh), ImVec2(p.x + (c + 1) * cw + 0.5f, p.y + (r + 1) * rh + 0.5f), heat((db + 80.f) / 90.f));
        }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Time runs downwards over the whole signal; frequency across the span (left = lowest). Colour: level re the mean density in the band."));
}

void plots(Studio& s, float height) {
    const Shown& sh = s.shown;
    const float hs = std::max(110 * gUi, height * 0.34f);
    char id[48];
    snprintf(id, sizeof id, "##tsspec%llu", (unsigned long long)sh.gen);
    if (plt::BeginPlot(id, ImVec2(-1, hs), plt::Flags_NoTitle)) {
        plt::SetupAxes("kHz", "dB re band");
        const double half = sh.sig ? sh.sig->rateHz / 2e3 : 1;
        plt::SetupAxisLimits(plt::X1, -half, half, plt::Cond_Once);
        plt::SetupAxisLimits(plt::Y1, -90, 15, plt::Cond_Once);
        if (!sh.specX.empty()) {
            plt::Spec ln; ln.LineColor = ImVec4(0.3f, 0.75f, 1.f, 0.95f);
            plt::PlotLine("spectrum", sh.specX.data(), sh.specY.data(), (int)sh.specX.size(), ln);
            if (!sh.maskX.empty()) {
                plt::Spec mk; mk.LineColor = ImVec4(1.f, 0.5f, 0.2f, 0.9f);
                plt::PlotLine(sh.m.mask.regulatory ? "mask" : "generic mask", sh.maskX.data(), sh.maskY.data(), (int)sh.maskX.size(), mk);
            }
        }
        plt::EndPlot();
    }
    const float hw = std::max(70 * gUi, height * 0.22f);
    waterfall(sh, ImVec2(ImGui::GetContentRegionAvail().x, hw));
    // time view + constellation
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderFloat(tr("Start"), &s.tStart, 0.f, 1.f, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderFloat(tr("Span (10^x samples)"), &s.tSpanLog, 1.5f, 5.5f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat(tr("Samples/symbol"), &s.symSpan, 0.f, 64.f, s.symSpan < 1 ? "all" : "%.1f");
    const float hb = std::max(110 * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi);
    const float cs = std::min(hb, 190 * gUi);
    const size_t span = (size_t)std::pow(10.0, (double)s.tSpanLog);
    if (sh.sig && !sh.sig->iq.empty()) {
        const auto& iq = sh.sig->iq;
        const size_t a = (size_t)((double)(iq.size() - 1) * s.tStart), n = std::min(iq.size() - a, span);
        static std::vector<double> tx, ti, tq, tm;
        const size_t pts = std::min<size_t>(n, 1500), stride = std::max<size_t>(1, n / std::max<size_t>(1, pts));
        tx.clear(); ti.clear(); tq.clear(); tm.clear();
        for (size_t i = 0; i + stride <= n; i += stride) {
            float mxm = 0;
            for (size_t k = 0; k < stride; k++) mxm = std::max(mxm, std::abs(iq[a + i + k]));
            tx.push_back(1e3 * (double)(a + i) / sh.sig->rateHz);
            ti.push_back(iq[a + i].real()); tq.push_back(iq[a + i].imag()); tm.push_back(mxm);
        }
        if (plt::BeginPlot("##tstime", ImVec2(ImGui::GetContentRegionAvail().x - cs - 8 * gUi, hb), plt::Flags_NoTitle)) {
            plt::SetupAxes("ms", "level");
            plt::SetupAxisLimits(plt::Y1, -1.1, 1.1, plt::Cond_Once);
            if (!tx.empty()) {
                plt::Spec a1; a1.LineColor = ImVec4(0.3f, 0.7f, 1.f, 0.9f);
                plt::Spec a2; a2.LineColor = ImVec4(1.f, 0.5f, 0.3f, 0.9f);
                plt::Spec a3; a3.LineColor = ImVec4(0.6f, 0.9f, 0.4f, 0.8f);
                plt::PlotLine("I", tx.data(), ti.data(), (int)tx.size(), a1);
                plt::PlotLine("Q", tx.data(), tq.data(), (int)tx.size(), a2);
                plt::PlotLine("|x|", tx.data(), tm.data(), (int)tx.size(), a3);
            }
            plt::EndPlot();
        }
        ImGui::SameLine();
        constellation(sh, cs, s.tStart, (float)span, s.symSpan);
    }
}

void measureTab(Studio& s) {
    const StudioMeasure& m = s.shown.m;
    if (!m.valid) { ImGui::TextDisabled("%s", tr("No signal yet.")); return; }
    auto row = [](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(210 * gUi); ImGui::TextUnformatted(v.c_str()); };
    char b[160];
    row(tr("Sample rate"), hzText(m.rateHz));
    snprintf(b, sizeof b, "%.3f s (%llu samples)", m.seconds, (unsigned long long)m.samples);
    row(tr("Duration"), b);
    row(tr("Occupied bandwidth, 99 %"), hzText(m.obw99Hz));
    row(tr("Bandwidth at -26 dB"), hzText(m.obw26Hz));
    snprintf(b, sizeof b, "%+.1f dBFS peak, %+.1f dBFS RMS", m.peakDb, m.rmsDb);
    row(tr("Level"), b);
    snprintf(b, sizeof b, "%.1f dB (%.1f dB exceeded by only 0.1 %% of samples)", m.paprDb, m.papr999Db);
    row("PAPR", b);
    if (m.acpr.lowerValid || m.acpr.upperValid) {
        snprintf(b, sizeof b, "lower %s, upper %s (channel = the 99 %% width, next to the signal)", m.acpr.lowerValid ? (std::to_string((int)std::lround(m.acpr.lowerDb)) + " dB").c_str() : "outside the span",
                 m.acpr.upperValid ? (std::to_string((int)std::lround(m.acpr.upperDb)) + " dB").c_str() : "outside the span");
        row("ACPR", b);
    } else row("ACPR", tr("the span is too narrow to hold a neighbour channel"));
    if (m.maskResult.valid) {
        snprintf(b, sizeof b, "%s: %s (worst %+.1f dB at %+.1f kHz)", m.mask.name.c_str(), m.maskResult.pass ? "within" : "EXCEEDED", m.maskResult.worstExcessDb, m.maskResult.atHz / 1e3);
        row(tr("Out-of-band mask"), b);
        if (!m.mask.regulatory) ImGui::TextDisabled("%s", tr("No standard's own mask is built in for this mode: the generic shoulder above is a guide, not a compliance test."));
    } else row(tr("Out-of-band mask"), tr("the sample rate does not reach far enough past the band"));
    if (m.peakDb >= -0.5) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.f), "%s", tr("The peaks are at full scale: the limiter rounds them in a real transmission."));
    typo::separatorText(tr("File size by format"));
    for (int f = 0; f < dect2::kStudioFormats; f++) {
        if ((StudioFormat)f == StudioFormat::WavAudio && !dect2::studioAudioFormatOk(m.rateHz)) continue;
        row(dect2::studioFormatName((StudioFormat)f), bytesText(dect2::studioFileBytes((StudioFormat)f, m.samples)));
    }
}

void checkTab(Studio& s) {
    ImGui::TextWrapped("%s", tr("Runs OnAir's own receiver on the signal above (with the impairments set here) and reports whether it decodes. The signal is played from a temporary file; no radio is used."));
    ImGui::Checkbox(tr("Noise"), &s.useSnr);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.useSnr);
    ImGui::SetNextItemWidth(220 * gUi);
    sliderD("##snr", &s.snr, -10.0, 60.0, "SNR %.1f dB (whole sample-rate band)");
    ImGui::EndDisabled();
    ImGui::Checkbox(tr("Frequency offset"), &s.useCfo);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.useCfo);
    ImGui::SetNextItemWidth(220 * gUi);
    sliderD("##cfo", &s.cfo, -20000.0, 20000.0, "%.0f Hz");
    ImGui::EndDisabled();
    const char* fades[] = {"None", "Flat Rayleigh", "Two-path (-6 dB echo)"};
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::Combo(tr("Fading"), &s.fading, fades, 3);
    if (s.fading) {
        ImGui::SetNextItemWidth(160 * gUi);
        sliderD(tr("Doppler (Hz)"), &s.doppler, 0.1, 200.0, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (s.fading == 2) { ImGui::SetNextItemWidth(160 * gUi); sliderD(tr("Echo delay (us)"), &s.delayUs, 0.5, 100.0, "%.1f"); }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(s.lbRunning || !s.shown.sig || s.jobRunning);
    if (ImGui::Button(tr("Run decoder check"))) {
        s.lbRunning = true; s.lbCancel = false; s.lb = dect2::StudioLoopback();
        auto sig = s.shown.sig;
        const TxModeSpec* mode = s.mode;
        s.lbJob = std::async(std::launch::async, [sig, mode, &c = s.lbCancel]() { return dect2::studioLoopback(*mode, *sig, mode->stdMode == 25 ? 40.0 : 90.0, &c); });
    }
    ImGui::EndDisabled();
    if (s.lbRunning) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) s.lbCancel = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("decoding..."));
        if (s.lbJob.valid() && s.lbJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { s.lb = s.lbJob.get(); s.lbRunning = false; }
    }
    if (!s.lb.summary.empty()) {
        const ImVec4 col = s.lb.locked ? ImVec4(0.3f, 0.85f, 0.4f, 1.f) : ImVec4(0.95f, 0.35f, 0.3f, 1.f);
        ImGui::TextColored(col, "%s", s.lb.summary.c_str());
        ImGui::TextDisabled("%s", tr("A lock means the receiver found and decoded the signal. A mode without a lock indicator in the engine shows no lock here; check its own panel (Open in decoder)."));
    }
}

std::string uniqueBase(const Studio& s) {
    std::string b = s.base;
    if (b.empty()) b = std::string("onair_txstudio_") + s.mode->id;
    return b;
}

void startSave(Studio& s, StudioFormat f, const std::string& dir, bool impaired, bool openIt) {
    if (!s.shown.sig || !s.mode) return;
    auto sig = impaired ? s.shown.sig : s.shown.clean;
    const dect2::TxRequest r = makeReq(s.ui, *s.mode, false);
    const dect2::txm::Impair im = impairOf(s);
    const StudioMeasure m = s.shown.m;
    const TxModeSpec* mode = s.mode;
    const double center = s.centerMhz * 1e6;
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    const std::string path = d + uniqueBase(s) + (openIt ? "_" + std::to_string((long long)std::time(nullptr)) : "");
    s.saving = true; s.openAfter = openIt; s.lastFmt = f;
    s.saveJob = std::async(std::launch::async, [=]() {
        dect2::StudioWriteInfo wi;
        wi.mode = mode; wi.req = &r; wi.measure = &m; wi.centerHz = center;
        wi.impair = impaired ? &im : nullptr;
        std::string data, meta, err;
        if (!dect2::studioWrite(path, *sig, f, wi, data, meta, err)) return std::string("!") + err;
        return data + "\n" + meta;
    });
}

// the file becomes the receiver's IQ file source and the mode of this signal is selected (the receiver is not started)
bool openInDecoder(App& a, Studio& s, const std::string& dataPath, const std::string& metaPath) {
    if (a.engine.running()) { s.saveMsg = tr("Stop the receiver first, then press Open in decoder again."); return false; }
    int fileIdx = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[(size_t)i].kind == DeviceInfo::File) { fileIdx = i; break; }
    if (fileIdx < 0) { s.saveMsg = tr("No IQ file source in this build."); return false; }
    IqFileInfo info; std::string err;
    a.file.offsetBytes = 0; a.file.endBytes = 0; a.file.bigEndian = false; a.file.centreHz = 0;
    if (probeIqFile(metaPath, info, err)) {
        a.file.path = info.dataPath; a.file.format = info.format; a.file.offsetBytes = info.offsetBytes; a.file.endBytes = info.endBytes;
        if (info.sampleRate > 0) a.file.sampleRate = info.sampleRate;
        if (info.centerHz > 0) a.freqMhz = info.centerHz / 1e6;
    } else {
        FileFormat ff;
        if (!dect2::studioPlayFormat(s.lastFmt, ff)) { s.saveMsg = tr("A WAV audio file cannot be played as an IQ source."); return false; }
        a.file.path = dataPath; a.file.format = ff; a.file.sampleRate = s.shown.sig ? s.shown.sig->rateHz : a.file.sampleRate;
    }
    a.devIdx = fileIdx;
    for (const auto& m : allModes())
        if (txModeForList(m.family, m.subId, 1) == s.mode) { selectMode(a, m.family, m.subId); break; }
    a.engine.log("TX studio: " + dataPath);
    s.saveMsg = tr("Opened as the IQ file source and the mode is selected. Press Start in the receiver.");
    return true;
}

void saveTab(App& a, Studio& s) {
    if (!s.folder[0]) snprintf(s.folder, sizeof s.folder, "%s", (plat::dataDir() + "/txstudio").c_str());
    ImGui::SetNextItemWidth(330 * gUi);
    ImGui::InputText(tr("Folder"), s.folder, sizeof s.folder);
    ImGui::SetNextItemWidth(330 * gUi);
    ImGui::InputText(tr("File name"), s.base, sizeof s.base);
    if (ImGui::BeginCombo(tr("Format"), dect2::studioFormatName((StudioFormat)s.fmt))) {
        for (int f = 0; f < dect2::kStudioFormats; f++) {
            if ((StudioFormat)f == StudioFormat::WavAudio && s.shown.sig && !dect2::studioAudioFormatOk(s.shown.sig->rateHz)) continue;
            if (ImGui::Selectable(dect2::studioFormatName((StudioFormat)f), s.fmt == f)) s.fmt = f;
        }
        ImGui::EndCombo();
    }
    if ((StudioFormat)s.fmt == StudioFormat::WavAudio) ImGui::TextDisabled("%s", tr("Audio modes: the real part of the baseband (the audio of an upper-sideband signal whose tones sit above DC)."));
    const bool imp = impairOf(s).any();
    if (imp) ImGui::Checkbox(tr("Save with the impairments (noise, offset, fading) applied"), &s.saveImpaired);
    if (s.shown.sig) ImGui::TextDisabled(tr("%.3f s, %s"), s.shown.sig->seconds, bytesText(dect2::studioFileBytes((StudioFormat)s.fmt, s.shown.sig->iq.size())).c_str());
    ImGui::BeginDisabled(s.saving || !s.shown.sig);
    if (ImGui::Button(tr("Save"))) startSave(s, (StudioFormat)s.fmt, s.folder, imp && s.saveImpaired, false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Open in decoder"))) {
        const std::string rec = recordingsDir();
        startSave(s, StudioFormat::SigMF, rec, imp && s.saveImpaired, true);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Writes a SigMF file into the recordings folder and makes it the receiver's IQ file source in this mode. Nothing is transmitted."));
    if (s.saving && s.saveJob.valid() && s.saveJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string r = s.saveJob.get();
        s.saving = false;
        if (!r.empty() && r[0] == '!') { s.saveMsg = r.substr(1); }
        else {
            const size_t nl = r.find('\n');
            s.lastData = r.substr(0, nl); s.lastMeta = nl == std::string::npos ? "" : r.substr(nl + 1);
            s.saveMsg = std::string(tr("Saved: ")) + s.lastData + "  (+ " + s.lastMeta + ")";
            if (s.openAfter) openInDecoder(a, s, s.lastData, s.lastMeta);
        }
    }
    if (s.saving) ImGui::TextDisabled("%s", tr("writing..."));
    if (!s.saveMsg.empty()) ImGui::TextWrapped("%s", s.saveMsg.c_str());

    typo::separatorText(tr("Batch export"));
    ImGui::TextWrapped("%s", tr("Sweep one parameter over a list and write one file per value. Key: a parameter key (wpm, baudIdx, kind, toneHz, ... or an advanced key such as am.depth), or impair.snr / impair.cfo. Values: 10, 20, 30 or first:last:step."));
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputText(tr("Key"), s.sweepKey, sizeof s.sweepKey);
    ImGui::SameLine();
    if (ImGui::BeginCombo("##swkeys", tr("pick"), ImGuiComboFlags_NoPreview)) {
        for (const char* k : {"wpm", "toneHz", "baudIdx", "shiftIdx", "kind", "variant", "dbm", "bandwidthMhz", "deviationHz", "impair.snr", "impair.cfo"})
            if (ImGui::Selectable(k)) snprintf(s.sweepKey, sizeof s.sweepKey, "%s", k);
        if (s.mode) for (const auto& d : s.ui.params) if (d.bind == Bind::Opt && ImGui::Selectable(d.key)) snprintf(s.sweepKey, sizeof s.sweepKey, "%s", d.key);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText(tr("Values"), s.sweepVals, sizeof s.sweepVals);
    const auto vals = dect2::studioParseList(s.sweepVals);
    if (vals.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1.f), "%s", tr("Not a valid list (at most 200 values)."));
    else if (s.shown.sig) ImGui::TextDisabled(tr("%zu files, about %s in all"), vals.size(), bytesText(vals.size() * dect2::studioFileBytes((StudioFormat)s.fmt, s.shown.sig->iq.size())).c_str());
    ImGui::BeginDisabled(s.batching || vals.empty() || !s.mode);
    if (ImGui::Button(tr("Export batch"))) {
        s.batching = true; s.batchCancel = false; s.batchDone = 0; s.batchTotal = vals.size(); s.batchMsg.clear();
        const dect2::TxRequest r = makeReq(s.ui, *s.mode, false);
        const TxModeSpec* mode = s.mode;
        const std::string key = s.sweepKey, folder = s.folder, base = uniqueBase(s);
        const double rate = s.rateKhz * 1e3, secs = s.seconds, center = s.centerMhz * 1e6;
        const dect2::txm::Impair im = impairOf(s);
        const StudioFormat f = (StudioFormat)s.fmt;
        s.batchJob = std::async(std::launch::async, [=, &s]() {
            const auto items = dect2::studioBatch(*mode, r, key, vals, rate, secs, im, f, folder, base, center, [&s](size_t i, size_t) { s.batchDone = i; }, &s.batchCancel);
            size_t ok = 0;
            std::string fails;
            for (const auto& it : items) { if (it.ok) ok++; else fails += "  " + std::to_string(it.value) + ": " + it.err + "\n"; }
            return std::to_string(ok) + " of " + std::to_string(vals.size()) + " files written to " + folder + "\n" + fails;
        });
    }
    ImGui::EndDisabled();
    if (s.batching) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Stop##batch"))) s.batchCancel = true;
        ImGui::ProgressBar(s.batchTotal ? (float)s.batchDone / (float)s.batchTotal : 0.f, ImVec2(300 * gUi, 0));
        if (s.batchJob.valid() && s.batchJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { s.batchMsg = s.batchJob.get(); s.batching = false; }
    }
    if (!s.batchMsg.empty()) ImGui::TextWrapped("%s", s.batchMsg.c_str());
}

} // namespace

void txStudioWindow(App& a) {
    if (!a.txStudioOpen) {
        // let a running job finish before the next frame uses its state; nothing else is kept
        return;
    }
    Studio& s = gS;
    if (!s.init) {
        s.init = true;
        s.ui.destKind = 1;
        loadPayloadPrefs(s.ui);
        s.ui.destKind = 1;
        s.ui.ifRateHz = 0;                  // the studio picks its own rate (makeReq would otherwise force the Transmit panel's file rate)
        s.ui.autoSampleRate = true;
        if (const TxModeSpec* rm = rxMode(a)) chooseStudioMode(s, *rm);
        else if (!dect2::txModes().empty()) chooseStudioMode(s, dect2::txModes().front());
    }
    firstWindowSize(1180 * gUi, 780 * gUi);
    if (!ImGui::Begin(trw("TX studio"), &a.txStudioOpen)) { ImGui::End(); return; }

    // a finished job
    if (s.jobRunning && s.job.valid() && s.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Shown r = s.job.get();
        s.jobRunning = false;
        if (!r.err.empty()) { s.shown.err = r.err; s.shown.gen = r.gen; }
        else s.shown = std::move(r);
        if (s.again) { s.again = false; startJob(s, s.regen); }
    }

    ImGui::TextWrapped("%s", tr("A bench for signals: nothing here is transmitted and no radio is used. To send a signal on air, use the Transmit panel (it stays behind Arm, your licence and the band plan)."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Open the Transmit panel"))) txViewFlag() = true;

    // what changed since the signal was made
    if (s.mode) {
        const dect2::TxRequest r = makeReq(s.ui, *s.mode, false);
        const std::string gk = keyOfReq(r, s, s.rateKhz * 1e3), ik = impKeyOf(s);
        const bool genChanged = gk != s.shownGenKey || s.regen, impChanged = ik != s.impKey;
        const double now = ImGui::GetTime();
        if (genChanged || impChanged) { if (s.changedAt == 0) s.changedAt = now; } else s.changedAt = 0;
        const bool heavy = s.mode->nominalRateHz > 1e6;
        const bool due = s.changedAt > 0 && now - s.changedAt > 0.6 && (s.autoUpdate || (!genChanged && impChanged));
        if (due) {
            if (s.jobRunning) s.again = true;
            else startJob(s, genChanged || !s.shown.clean);
            s.changedAt = 0;
        }
        if ((genChanged || impChanged) && !due) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.f), "%s", s.autoUpdate ? tr("updating...") : tr("settings changed"));
        }
        ImGui::Checkbox(tr("Update when a setting changes"), &s.autoUpdate);
        if (heavy && s.autoUpdate) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(a wide signal is slow to make: turn this off and use Generate)")); }
        ImGui::SameLine();
        ImGui::BeginDisabled(s.jobRunning);
        if (ImGui::Button(tr("Generate"))) { s.regen = true; startJob(s, true); }
        ImGui::EndDisabled();
        if (s.jobRunning) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("working...")); }
        if (!s.shown.err.empty()) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.f), "%s", s.shown.err.c_str());
        if (s.shown.sig && !s.shown.sig->note.empty()) ImGui::TextDisabled("%s", s.shown.sig->note.c_str());
        if (s.shown.sig && s.shown.sig->capped) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.f), "%s", tr("The length was cut at the sample limit (24 M samples)."));
    }

    const float leftW = std::min(430.f * gUi, ImGui::GetContentRegionAvail().x * 0.4f);
    if (ImGui::BeginChild("##tsleft", ImVec2(leftW, 0), ImGuiChildFlags_Borders)) settingsColumn(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float availY = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##tsplots", ImVec2(0, availY * 0.56f), ImGuiChildFlags_None)) plots(s, ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    if (ImGui::BeginChild("##tstabs", ImVec2(0, 0), ImGuiChildFlags_None)) {
        if (ImGui::BeginTabBar("##tstab")) {
            if (ImGui::BeginTabItem(trw("Measurements"))) { measureTab(s); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Decoder check"))) { checkTab(s); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Save and batch"))) { saveTab(a, s); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::End();
}
