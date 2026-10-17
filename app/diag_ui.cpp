// Radio problems where the user looks: the listing error, a failed start, a frequency the radio cannot tune and the USB hints, all under the
// radio picker, plus a "Copy diagnostics" button that puts what a bug report needs on the clipboard.
#include "app.h"
#include "dect2/converter.h"
#include "dect2/updater.h"
#include "dect2/hfdig_freedv.h"
#include "fixit.h"
#include "dect2/crash_report.h"
#include <fstream>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <thread>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if (defined(__x86_64__) || defined(__i386__)) && !defined(__APPLE__) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace {

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

bool hasWord(const std::string& line, const char* w) {
    std::string l = line;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l.find(w) != std::string::npos;
}

std::string rangeText(double lo, double hi, double scale, const char* unit) {
    if (lo <= 0 && hi <= 0) return "unknown";
    return fmt("%.6g-%.6g %s", lo / scale, hi / scale, unit);
}

std::string osName() {
#if defined(__APPLE__)
    char v[64] = ""; size_t n = sizeof v;
    sysctlbyname("kern.osproductversion", v, &n, nullptr, 0);
    return std::string("macOS ") + v;
#elif defined(_WIN32)
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi; memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    auto fn = nt ? (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion") : nullptr;   // GetVersionEx lies on newer Windows
    if (fn && fn(&vi) == 0) return fmt("Windows %lu.%lu build %lu", (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
    return "Windows";
#else
    std::string pretty;
    if (FILE* f = fopen("/etc/os-release", "r")) {
        char l[256];
        while (fgets(l, sizeof l, f)) {
            if (strncmp(l, "PRETTY_NAME=", 12) == 0) { pretty = trim(l + 12); if (pretty.size() > 1 && pretty.front() == '"') pretty = pretty.substr(1, pretty.size() - 2); }
        }
        fclose(f);
    }
    struct utsname u;
    const std::string kernel = uname(&u) == 0 ? std::string(" (kernel ") + u.release + ")" : "";
    return (pretty.empty() ? "Linux" : pretty) + kernel;
#endif
}

std::string cpuName() {
    std::string name;
#if defined(__APPLE__)
    char v[128] = ""; size_t n = sizeof v;
    if (sysctlbyname("machdep.cpu.brand_string", v, &n, nullptr, 0) == 0) name = v;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
    unsigned r[12] = {};
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u) {
        for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
        name = std::string((const char*)r, strnlen((const char*)r, sizeof r));
    }
#endif
#if defined(__linux__)
    if (trim(name).empty()) {
        if (FILE* f = fopen("/proc/cpuinfo", "r")) {
            char l[256];
            while (name.empty() && fgets(l, sizeof l, f)) {
                if (strncmp(l, "model name", 10) == 0 || strncmp(l, "Hardware", 8) == 0 || strncmp(l, "Model", 5) == 0) { const char* c = strchr(l, ':'); if (c) name = c + 1; }
            }
            fclose(f);
        }
    }
#endif
    name = trim(name);
    if (name.empty()) name = "unknown";
    return name + fmt(" (%u threads)", std::thread::hardware_concurrency());
}

// The OpenGL renderer when the window uses OpenGL; the Metal back end has no such string
std::string gpuName() {
    typedef const unsigned char* (*GetString)(unsigned);
    if (glfwGetCurrentContext()) {
        auto gs = (GetString)glfwGetProcAddress("glGetString");
        if (gs) { const unsigned char* r = gs(0x1F01 /* GL_RENDERER */); if (r) return (const char*)r; }
    }
#ifdef __APPLE__
    return "Metal";
#else
    return "unknown";
#endif
}

// "Record IQ": saves the radio's raw samples for a bug report. Idle: a button (and the format); recording: a red "Stop 12.3 s 98 MB";
// after it: "Show recording" opens the folder.
std::string sizeText(uint64_t bytes) {
    char b[32];
    if (bytes >= 1000000000ull) snprintf(b, sizeof b, "%.2f GB", bytes / 1e9);
    else snprintf(b, sizeof b, "%.0f MB", bytes / 1e6);
    return b;
}

void recordUi(App& a) {
    const RecordingStats rs = a.engine.recordingStats();
    if (rs.active) a.recWasActive = true;
    else if (a.recWasActive) {   // it ended: stopped here, with the radio, at the size limit or on a disk error
        a.recWasActive = false;
        a.recDonePath = rs.path;
        char b[64];
        snprintf(b, sizeof b, "recorded %.1f s (%s) to ", rs.seconds, sizeText(rs.bytes).c_str());
        a.engine.log(std::string(b) + rs.path);
        if (!rs.error.empty()) a.engine.log("recording stopped: " + rs.error);
        if (rs.droppedSamples) a.engine.log("the disk was too slow: some samples are missing from the recording");
    }
    const bool isFile = a.devices[a.devIdx].kind == DeviceInfo::File;
    if (rs.active) {
        char b[96];
        // ### keeps the button id still while the seconds and the size change. Without it the click is lost and the recording only ends when the radio is stopped.
        snprintf(b, sizeof b, "Stop  %.1f s  %s###recstop", rs.seconds, sizeText(rs.bytes).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        if (ImGui::SmallButton(b)) a.engine.stopRecording();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops the recording");
        if (rs.droppedSamples) {
            flowNext(4 * gUi);
            ImGui::TextColored(pal::warnAmber(), "Â· dropped");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The disk was too slow: some samples are missing from the recording. Record to a faster drive or use the 8-bit format.");
        }
    } else {
        const bool can = a.engine.running() && !isFile;
        ImGui::BeginDisabled(!can);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float sp = ImGui::CalcTextSize(" ").x;
        if (ImGui::SmallButton((std::string("    ") + tr("Record IQ")).c_str())) {
            const FileFormat f = a.recFormat == 1 ? FileFormat::CF32 : FileFormat::CS8;
            const std::string path = plat::dataDir() + "/recordings/" + a.engine.recordingName(f);
            std::string err;
            if (a.engine.startRecording(path, f, err)) a.recDonePath.clear();
            else a.engine.log("recording failed: " + err);
        }
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p0.x + ImGui::GetStyle().FramePadding.x + 2.2f * sp, p0.y + ImGui::GetItemRectSize().y * 0.5f), 3.2f * gUi, can ? ImGui::GetColorU32(pal::badRed()) : IM_COL32(120, 80, 78, 255));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr(isFile ? "Recordings of a file are not needed" : !can ? "Start the radio first" : "Saves the radio's raw samples to a file, for a bug report"));
    }
    flowNext(6 * gUi);
    ImGui::BeginDisabled(rs.active);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("8-bit").x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x + 4 * gUi);
    if (ImGui::BeginCombo("##recfmt", a.recFormat == 1 ? "float" : "8-bit")) {
        if (ImGui::Selectable("8-bit", a.recFormat == 0)) { a.recFormat = 0; plat::prefs().setI("recFormat", 0); }
        if (ImGui::Selectable("float", a.recFormat == 1)) { a.recFormat = 1; plat::prefs().setI("recFormat", 1); }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("8-bit: 2 bytes per sample, small. Float: 8 bytes per sample, exact.");
    flowNext(6 * gUi);
    ImGui::BeginDisabled(rs.active);
    if (ImGui::Checkbox("SigMF", &a.recSigmf)) { plat::prefs().setB("recSigmf", a.recSigmf); a.engine.setRecordSigmf(a.recSigmf); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Writes a .sigmf-meta file (sample rate, data type, centre frequency, time) next to the recording, and the same data under a .sigmf-data name, so the recording opens in IQEngine (iqengine.org) and other SigMF tools.");
    if (!a.recDonePath.empty() && !rs.active) {
        flowNext(6 * gUi);
        if (ImGui::SmallButton("Show recording")) dect2::openUrl(a.recDonePath.substr(0, a.recDonePath.find_last_of("/\\")));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.recDonePath.c_str());
    }
    flowEnd();
}

} // namespace

std::string radioRangeWarning(const DeviceInfo& d, double freqMhz, const std::map<std::string, std::string>* radio) {
    if (!d.isRadio() || d.minFreqHz <= 0 || d.maxFreqHz <= 0) return "";
    double hz = freqMhz * 1e6;
    const dect2::Converter c = radio ? dect2::converterFromRadio(*radio) : dect2::Converter();
    if (c.active) {   // the radio is tuned to the converter's output
        hz = c.rfToIf(hz);
        if (hz >= d.minFreqHz && hz <= d.maxFreqHz) return "";
        return fmt("%.3f MHz at the antenna is %.3f MHz at the radio, outside what this radio is specified for (%.6g-%.6g MHz); OnAir still asks for it, but the driver may not reach it or the signal may be weak or absent", freqMhz, hz / 1e6, d.minFreqHz / 1e6, d.maxFreqHz / 1e6);
    }
    if (hz >= d.minFreqHz && hz <= d.maxFreqHz) return "";
    return fmt("%.3f MHz is outside what this radio is specified for (%.6g-%.6g MHz); OnAir still asks for it, but the driver may not reach it or the signal may be weak or absent", freqMhz, d.minFreqHz / 1e6, d.maxFreqHz / 1e6);
}

std::vector<RadioMsg> radioMessages(const App& a) {
    std::vector<RadioMsg> m;
    if (!a.hackrfErr.empty() && !a.hackrfErrHidden) m.push_back({0, a.hackrfErr});
    if (!a.startErr.empty()) m.push_back({1, a.startErr});
    bool srcNoted = false;
    if (a.engine.running()) {   // what the radio's driver noted at the start or the last retune (a lower rate, a frequency it does not cover, ...)
        const std::string n = a.engine.sourceNote();
        if (!n.empty()) m.push_back({2, n});
        srcNoted = n.find("weak or absent") != std::string::npos;   // the driver already said so: not twice
    }
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) {
        const std::string w = srcNoted ? std::string() : radioRangeWarning(a.devices[a.devIdx], a.freqMhz, &a.tune.radio);
        if (!w.empty()) m.push_back({2, w});
    }
    for (auto& h : a.usbHints) m.push_back({3, h});
    return m;
}


namespace {
// A zip file with the entries stored (not compressed): enough for a few text logs, and it opens everywhere.
uint32_t crc32z(const std::string& d) {
    static uint32_t t[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : d) c = t[(c ^ ch) & 255] ^ (c >> 8);
    return ~c;
}
void put16(std::string& o, uint32_t v) { o.push_back((char)(v & 255)); o.push_back((char)((v >> 8) & 255)); }
void put32(std::string& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
bool writeZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, dir;
    for (const auto& f : files) {
        const uint32_t crc = crc32z(f.second), off = (uint32_t)out.size(), n = (uint32_t)f.second.size();
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0x0800); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, n); put32(out, n); put16(out, (uint32_t)f.first.size()); put16(out, 0);
        out += f.first; out += f.second;
        put32(dir, 0x02014b50); put16(dir, 20); put16(dir, 20); put16(dir, 0x0800); put16(dir, 0); put16(dir, 0); put16(dir, 0x21);
        put32(dir, crc); put32(dir, n); put32(dir, n); put16(dir, (uint32_t)f.first.size()); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
        put32(dir, 0); put32(dir, off); dir += f.first;
    }
    const uint32_t dirOff = (uint32_t)out.size();
    out += dir;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, (uint32_t)files.size()); put16(out, (uint32_t)files.size());
    put32(out, (uint32_t)dir.size()); put32(out, dirOff); put16(out, 0);
    std::ofstream o(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), std::ios::binary);
    o.write(out.data(), (std::streamsize)out.size());
    return (bool)o;
}
std::string slurp(const std::filesystem::path& p, size_t maxBytes = 4u << 20) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s = "(the first part is left out)\n" + s.substr(s.size() - maxBytes);
    return s;
}
}

std::string buildDiagnostics(const DiagInput& in) {
    std::string s = "OnAir diagnostics\n";
    s += "Version: " + in.version + "\n";
    s += "OS: " + in.os + "\n";
    s += "CPU: " + in.cpu + "\n";
    s += "Graphics: " + in.gpu + "\n\n";
    s += fmt("Radios (%zu):\n", in.radios.size());
    if (in.radios.empty()) s += "  none found\n";
    for (auto& d : in.radios)
        s += "  " + d.name + " | board: " + (d.board.empty() ? "-" : d.board) + " | serial: " + (d.serial.empty() ? "-" : d.serial) +
             " | rate: " + rangeText(d.minRateHz, d.maxRateHz, 1e6, "Msps") + " | frequency: " + rangeText(d.minFreqHz, d.maxFreqHz, 1e6, "MHz") +
             " | gain: " + rangeText(d.gainMinDb, d.gainMaxDb, 1, "dB") + "\n";
    s += "\nMessages shown:\n";
    if (in.messages.empty()) s += "  none\n";
    for (auto& l : in.messages) s += "  " + l + "\n";
    // the log can be thousands of lines: only the ones about radios, libraries and failures, newest last
    std::vector<const std::string*> pick;
    for (auto& l : in.log)
        if (hasWord(l, "radio") || hasWord(l, "library") || hasWord(l, "error") || hasWord(l, "failed")) pick.push_back(&l);
    const size_t from = pick.size() > 60 ? pick.size() - 60 : 0;
    s += fmt("\nLog (last %zu matching lines):\n", pick.size() - from);
    for (size_t i = from; i < pick.size(); i++) s += *pick[i] + "\n";
    if (!in.mapServer.empty()) s += in.mapServer;
    return s;
}

std::string diagnosticsText(App& a) {
    DiagInput in;
    in.version = ONAIR_VERSION;
    in.os = osName();
    in.cpu = cpuName();
    in.gpu = gpuName();
    for (auto& d : a.devices) if (d.isRadio()) in.radios.push_back(d);
    for (auto& m : radioMessages(a)) in.messages.push_back(m.text);
    size_t total = 0;
    in.log = a.engine.logSnapshot(total);
    in.mapServer = mapSendDiagnosticsBlock(a);
    return buildDiagnostics(in);
}

// A failed start or retune only reaches the engine log: pick the newest such line up from there (a few times a second is plenty)
static void pollStartError(App& a) {
    const bool run = a.engine.running();
    if (run && !a.diagWasRunning) a.startErr.clear();   // a successful start ends the message
    a.diagWasRunning = run;
    const double now = ImGui::GetTime();
    if (now - a.diagPollAt < 0.25) return;
    a.diagPollAt = now;
    size_t total = 0;
    const auto lines = a.engine.logSnapshot(total);
    static const char* tags[] = {"source start failed: ", "retune failed: "};
    for (size_t i = lines.size(); i-- > 0;) {
        for (const char* t : tags) {
            const size_t p = lines[i].find(t);
            if (p == std::string::npos) continue;
            if (lines[i] != a.startErrKey) {
                a.startErrKey = lines[i];
                const bool start = t == tags[0];
                a.startErr = std::string(start ? "Could not start the radio: " : "Could not change the radio setting: ") + lines[i].substr(p + strlen(t));
            }
            return;
        }
    }
}

// The small "How to fix" button and its popup: what it means, numbered steps for this OS, and the buttons that help (fixit.h)
void fixitUi(App& a, const fixit::Entry* e, const char* idKey) {
    if (!e) return;
    ImGui::PushID(idKey);
    if (ImGui::SmallButton(tr("How to fix"))) ImGui::OpenPopup("##fixit");
    ImGui::SetNextWindowSizeConstraints(ImVec2(340 * gUi, 0), ImVec2(520 * gUi, 600 * gUi));
    if (ImGui::BeginPopup("##fixit")) {
        ImGui::PushTextWrapPos(500 * gUi);
        ImGui::TextColored(pal::accent(), "%s", tr(e->title));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr(e->why));
        ImGui::Spacing();
        int n = 1;
        for (const char* st : fixit::stepsFor(*e, fixit::currentOs())) { ImGui::Text("%d.", n++); ImGui::SameLine(); ImGui::TextUnformatted(tr(st)); }
        ImGui::Spacing();
        bool first = true;
        for (int b = 0; b < 3 && e->buttons[b].label; b++) {
            const fixit::Button& bt = e->buttons[b];
            if (!(bt.os & fixit::currentOs())) continue;
            if (!first) ImGui::SameLine();
            first = false;
            if (ImGui::Button(tr(bt.label))) {
                switch (bt.act) {
                case fixit::Act::OpenUrl: dect2::openUrl(bt.arg); break;
                case fixit::Act::CopyText: ImGui::SetClipboardText(bt.arg); break;
                case fixit::Act::OpenDiagnostics: ImGui::SetClipboardText(diagnosticsText(a).c_str()); a.diagCopiedAt = ImGui::GetTime(); break;
                case fixit::Act::OpenCodecs: {
                    const auto d = dect2::codecFolderList();
                    if (!d.empty()) { std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(d[0]), ec); dect2::openUrl(d[0]); }
                    break;
                }
                }
            }
            if (bt.act == fixit::Act::CopyText && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bt.arg);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void radioMessagesUi(App& a, bool vertical) {
    pollStartError(a);
    const auto msgs = radioMessages(a);
    if (!vertical) {   // the one-line top bar has no room for text: a mark with the messages as its tooltip
        if (msgs.empty()) return;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "(!)");
        if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420 * gUi); for (auto& m : msgs) ImGui::TextWrapped("%s", m.text.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        return;
    }
    for (size_t i = 0; i < msgs.size(); i++) {
        const RadioMsg& m = msgs[i];
        const ImVec4 col = m.kind <= 1 ? ImVec4(0.95f, 0.4f, 0.35f, 1) : m.kind == 2 ? ImVec4(0.95f, 0.78f, 0.25f, 1) : ImVec4(0.62f, 0.66f, 0.72f, 1);
        ImGui::PushID((int)i);
        if (m.kind <= 1) {   // errors can be dismissed; the range and the hints go away by themselves
            if (ImGui::SmallButton("x")) { if (m.kind == 0) a.hackrfErrHidden = true; else a.startErr.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dismiss");
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        fixitUi(a, fixit::lookup(m.text), "fix");
        ImGui::PopID();
    }
    if (a.devices.size() <= 2) {   // nothing listed: say so and say what helps
        ImGui::TextDisabled("%s", tr("No radio found"));
        ImGui::SameLine();
        fixitUi(a, fixit::byId("no_radio"), "fixnone");
    }
    const bool copied = ImGui::GetTime() - a.diagCopiedAt < 2.0;
    if (ImGui::SmallButton(tr(copied ? "Copied" : "Copy diagnostics"))) {
        ImGui::SetClipboardText(diagnosticsText(a).c_str());
        a.diagCopiedAt = ImGui::GetTime();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies the version, system, radios, messages and radio log lines, for a bug report"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save bug report"))) {
        const std::string path = plat::saveFileDialog("onair-bug-report.zip");
        if (!path.empty()) {
            const std::filesystem::path d = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(dect2::crash::logDir().data()), dect2::crash::logDir().size()));
            std::vector<std::pair<std::string, std::string>> f;
            f.push_back({"diagnostics.txt", diagnosticsText(a)});
            for (const char* n : {"onair.log", "onair-prev.log", "crash.txt"}) {
                std::error_code ec;
                if (std::filesystem::exists(d / n, ec)) f.push_back({n, slurp(d / n)});
            }
            a.diagCopiedAt = writeZip(path, f) ? ImGui::GetTime() : -10.0;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Saves one zip file with the diagnostics, the log and the last crash report: attach it to a GitHub issue"));
    flowNext(6 * gUi);
    recordUi(a);
}
