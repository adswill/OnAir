// Radio problems where the user looks: the listing error, a failed start, a frequency the radio cannot tune and the USB hints, all under the
// radio picker, plus a "Copy diagnostics" button that puts what a bug report needs on the clipboard.
#include "app.h"
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

} // namespace

std::string radioRangeWarning(const DeviceInfo& d, double freqMhz) {
    if (!d.isRadio() || d.minFreqHz <= 0 || d.maxFreqHz <= 0) return "";
    const double hz = freqMhz * 1e6;
    if (hz >= d.minFreqHz && hz <= d.maxFreqHz) return "";
    return fmt("%.3f MHz is outside this radio's range (%.6g-%.6g MHz)", freqMhz, d.minFreqHz / 1e6, d.maxFreqHz / 1e6);
}

std::vector<RadioMsg> radioMessages(const App& a) {
    std::vector<RadioMsg> m;
    if (!a.hackrfErr.empty() && !a.hackrfErrHidden) m.push_back({0, a.hackrfErr});
    if (!a.startErr.empty()) m.push_back({1, a.startErr});
    if (a.engine.running()) {   // what the radio's driver noted at the start or the last retune (a lower rate, a frequency it does not cover, ...)
        const std::string n = a.engine.sourceNote();
        if (!n.empty()) m.push_back({2, n});
    }
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) {
        const std::string w = radioRangeWarning(a.devices[a.devIdx], a.freqMhz);
        if (!w.empty()) m.push_back({2, w});
    }
    if (a.devices.size() <= 2) for (auto& h : a.usbHints) m.push_back({3, h});
    return m;
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
        ImGui::PopID();
    }
    const bool copied = ImGui::GetTime() - a.diagCopiedAt < 2.0;
    if (ImGui::SmallButton(copied ? "Copied" : "Copy diagnostics")) {
        ImGui::SetClipboardText(diagnosticsText(a).c_str());
        a.diagCopiedAt = ImGui::GetTime();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies the version, system, radios, messages and radio log lines, for a bug report");
}
