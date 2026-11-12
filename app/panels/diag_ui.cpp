// Radio problems where the user looks: the listing error, a failed start, a frequency the radio cannot tune and the USB hints, all under the
// radio picker, plus a "Copy diagnostics" button that puts what a bug report needs on the clipboard.
#include "dect2/common/u8file.h"
#include "dect2/source/gain.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/audio/audioout.h"
#include "dect2/audio/hf_demod.h"
#include "dect2/audio/mp3_enc.h"
#include "dect2/source/iq_record.h"
#include "../app.h"
#include "../transcribe/transcribe_app.h"
#include "../clips/clips_ui.h"
#include "dect2/source/converter.h"
#include "dect2/engine/updater.h"
#include "dect2/hfdig/hfdig_freedv.h"
#include "fixit.h"
#include "dect2/engine/crash_report.h"
#include "dect2/voicekeys/key_guard.h"
#include "../ui/panel_layout.h"
#include "../ui/next_step_logic.h"
#include "../ui/ease_logic.h"
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

// the second column of the record rows: the format boxes start at the same x in the IQ and the audio row
float recordColX() { return std::max(panel::dotButtonW("Record IQ"), panel::dotButtonW("Record audio")) + 8 * gUi; }
void toRecordCol(float x0) {
    const float want = x0 + recordColX();
    if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + 6 * gUi < want) ImGui::SameLine(want); else flowNext(6 * gUi);
}

// part 0: the whole row; 1: the button and the format; 2: the SigMF switch (the Record group puts it with Rewind and Library)
void recordUi(App& a, int part = 0) {
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
    const float rowX0 = ImGui::GetCursorPosX();
    if (part != 2) {
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
        if (panel::dotButton(tr("Record IQ"), can)) {
            const FileFormat f = (FileFormat)recFileFormatOf(a.recFormat);
            const std::string path = recordingsDir() + "/" + (a.recFormat == 2 ? blueRecordingName(a.engine.recordingName(f)) : a.engine.recordingName(f));
            std::string err;
            if (a.engine.startRecording(path, f, err)) a.recDonePath.clear();
            else a.engine.log("recording failed: " + err);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr(isFile ? "Recordings of a file are not needed" : !can ? "Start the radio first" : "Saves the radio's raw samples to a file, for a bug report"));
    }
    toRecordCol(rowX0);
    ImGui::BeginDisabled(rs.active);
    ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize("X-Midas BLUE").x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x + 4 * gUi, std::max(60.f * gUi, ImGui::GetContentRegionAvail().x)));   // never wider than the panel
    if (ImGui::BeginCombo("##recfmt", a.recFormat == 2 ? "X-Midas BLUE" : a.recFormat == 1 ? "float" : a.recFormat == 3 ? tr("16-bit") : "8-bit")) {
        if (ImGui::Selectable("8-bit", a.recFormat == 0)) { a.recFormat = 0; plat::prefs().setI("recFormat", 0); }
        if (ImGui::Selectable(tr("16-bit"), a.recFormat == 3)) { a.recFormat = 3; plat::prefs().setI("recFormat", 3); }
        if (ImGui::Selectable("float", a.recFormat == 1)) { a.recFormat = 1; plat::prefs().setI("recFormat", 1); }
        if (ImGui::Selectable("X-Midas BLUE (.tmp)", a.recFormat == 2)) { a.recFormat = 2; plat::prefs().setI("recFormat", 2); }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("8-bit: 2 bytes per sample, small. Float: 8 bytes per sample, exact. X-Midas BLUE: float samples in a .tmp file with the rate and centre frequency in its header (for X-Midas, SigPlot and other BLUE tools).");
    }   // part != 2
    if (part != 1) {
    ImGui::BeginDisabled(rs.active);
    if (ImGui::Checkbox("SigMF", &a.recSigmf)) { plat::prefs().setB("recSigmf", a.recSigmf); a.engine.setRecordSigmf(a.recSigmf); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Writes a .sigmf-meta file (sample rate, data type, centre frequency, time) next to the recording, and the same data under a .sigmf-data name, so the recording opens in IQEngine (iqengine.org) and other SigMF tools.");
    }   // part != 1
    if (part != 2 && !a.recDonePath.empty() && !rs.active) {
        flowNext(6 * gUi);
        if (ImGui::SmallButton(tr("Show recording##iq"))) dect2::openUrl(a.recDonePath.substr(0, a.recDonePath.find_last_of("/\\")));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.recDonePath.c_str());
    }
    flowEnd();
}

// "Record audio": the sound you hear, from any mode (a tap in AudioOut, before the volume). Idle: a button, the format and three switches;
// recording: a red "Stop 12.3 s 1.2 MB". Stops by itself on a mode change or a retune unless "Keep going" is on.
// part 0: the whole row; 1: the button, the format, Show recording; 2: the switches (Only audio, Split, Keep going)
void audioRecordUi(App& a, int part = 0) {
    dect2::AudioRecorder& rec = dect2::AudioRecorder::instance();
    const dect2::AudioRecStats rs = rec.stats();
    if (rs.active) {
        a.audioRecWasActive = true;
        if (!a.audioRecKeep && (!a.engine.running() || a.engine.standardMode() != a.audioRecMode || std::fabs(a.freqMhz - a.audioRecFreq) > 1e-6)) rec.stop();
    } else if (a.audioRecWasActive) {
        a.audioRecWasActive = false;
        a.audioRecDonePath = rs.path;
        tx::captionsRecordStop(rs.path);   // name.srt / name.vtt next to the recording when auto subtitles ran
        char b[96];
        snprintf(b, sizeof b, "audio recorded: %.1f s in %d file(s), %s, to ", rs.seconds, rs.files, sizeText(rs.bytes).c_str());
        if (rs.files > 0) a.engine.log(std::string(b) + rs.path);
        if (!rs.error.empty()) a.engine.log("audio recording stopped: " + rs.error);
        if (rs.dropped) a.engine.log("the disk was too slow: some audio is missing from the recording");
    }
    const float rowX0 = ImGui::GetCursorPosX();
    if (part != 2) {
    if (rs.active) {
        char b[96];
        snprintf(b, sizeof b, "%s  %.1f s  %.1f MB%s###audiorecstop", tr("Stop"), rs.seconds, rs.bytes / 1e6, a.audioRecGate && !rs.gateOpen ? "  (waiting)" : "");
        ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        if (ImGui::SmallButton(b)) rec.stop();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rs.path.c_str());
    } else {
        const bool can = a.engine.running();
        ImGui::BeginDisabled(!can);
        if (panel::dotButton(tr("Record audio"), can)) {
            dect2::AudioRecOptions o;
            o.format = (dect2::AudioRecFormat)a.audioRecFmt; o.gated = a.audioRecGate; o.split = a.audioRecGate && a.audioRecSplit;
            const int m = a.engine.standardMode();
            const double hz = a.freqMhz * 1e6;
            char tag[96]; snprintf(tag, sizeof tag, "OnAir %s %.3f MHz", dect2::recordingModeId(m).c_str(), a.freqMhz);
            o.tag = tag;
            const std::string path = recordingsDir() + "/" + dect2::AudioRecorder::fileName(dect2::recordingModeId(m), hz, o.format);
            std::string err;
            if (rec.start(path, o, err)) { tx::captionsRecordStart(); a.audioRecDonePath.clear(); a.audioRecMode = m; a.audioRecFreq = a.freqMhz; }
            else a.engine.log("audio recording failed: " + err);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr(!can ? "Start the radio first" : "Saves the sound you hear, in any mode, to a file (before the volume slider, so the level does not depend on it)"));
    }
    tourMark("record", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());   // the recording lesson
    toRecordCol(rowX0);
    ImGui::BeginDisabled(rs.active);
    static const char* const fm[] = {"WAV", "FLAC", "MP3"};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FLAC").x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x + 4 * gUi);
    if (ImGui::BeginCombo("##audiorecfmt", fm[a.audioRecFmt])) {
        for (int i = 0; i < 3; i++) {
            const bool off = i == 2 && !dect2::Mp3Enc::available();
            ImGui::BeginDisabled(off);
            if (ImGui::Selectable(fm[i], a.audioRecFmt == i)) { a.audioRecFmt = i; plat::prefs().setI("audioRecFmt", i); }
            ImGui::EndDisabled();
            if (off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("MP3 needs the LAME library (libmp3lame), which is not installed"));
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("WAV: 16-bit, large. FLAC: lossless, about half the size. MP3: small (needs libmp3lame). FM stereo is kept as stereo."));
    }   // part != 2
    if (part != 1) {
    ImGui::BeginDisabled(rs.active);
    if (ImGui::Checkbox(tr("Skip silence"), &a.audioRecGate)) plat::prefs().setB("audioRecGate", a.audioRecGate);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Audio recording writes to the file only while there is sound, like a squelch: the silent parts are left out, with a short lead-in and tail kept around each burst of sound."));
    if (a.audioRecGate) {
        flowNext(6 * gUi);
        ImGui::BeginDisabled(rs.active);
        if (ImGui::Checkbox(tr("Split"), &a.audioRecSplit)) plat::prefs().setB("audioRecSplit", a.audioRecSplit);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("One file per transmission: a new file starts after 3 s of silence"));
    }
    flowNext(6 * gUi);
    if (ImGui::Checkbox(tr("Keep recording"), &a.audioRecKeep)) plat::prefs().setB("audioRecKeep", a.audioRecKeep);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Audio recording carries on when you change the mode or the frequency, or stop the receiver. Off: it stops by itself then. The Stop button always works."));
    }   // part != 1
    if (part != 2 && !a.audioRecDonePath.empty() && !rs.active) {
        flowNext(6 * gUi);
        if (ImGui::SmallButton(tr("Show recording##aud"))) dect2::openUrl(a.audioRecDonePath.substr(0, a.audioRecDonePath.find_last_of("/\\")));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.audioRecDonePath.c_str());
        if (!rs.error.empty()) { flowNext(4 * gUi); ImGui::TextColored(pal::warnAmber(), "%s", rs.error.c_str()); }
    }
    flowEnd();
}

// "Monitor audio": hear the tuned channel of any mode (also the ones without sound of their own), USB / LSB / NFM / AM; the outputs (two devices, a
// local TCP stream for other programs) are in the "Audio..." pop-up. It stays silent while the mode plays its own sound.
void audioMonitorUi(App& a) {
    dect2::AudioMonitor& mon = a.engine.monitor();
    dect2::MonitorConfig c;
    c.on = a.monOn; c.demod = dect2::hfDemodResolve(a.monDemod, a.freqMhz); c.volume = a.monVol; c.device = a.monDev; c.out2 = a.monOut2; c.device2 = a.monDev2;
    c.volume2 = a.monVol2; c.latencyMs = a.monLatency; c.yield = a.monYield; c.net = a.monNet; c.port = a.monPort; c.bindAll = a.monBindAll; c.wav = a.monWav;
    static std::string last;
    char key[512];
    snprintf(key, sizeof key, "%d|%d|%.3f|%s|%d|%s|%.3f|%d|%d|%d|%d|%d|%d", c.on, c.demod, c.volume, c.device.c_str(), c.out2, c.device2.c_str(), c.volume2, c.latencyMs, c.yield, c.net, c.port, c.bindAll, c.wav);
    if (key != last) { last = key; mon.configure(c); }
    a.engine.setMonitorOffsetHz(a.monOffset);
    auto save = [&] {
        auto& d = plat::prefs();
        d.setB("monOn", a.monOn); d.setI("monDemod", a.monDemod); d.setD("monVol", a.monVol); d.setD("monVol2", a.monVol2); d.setI("monLatency", a.monLatency);
        d.setB("monYield", a.monYield); d.setS("monDev", a.monDev); d.setS("monDev2", a.monDev2); d.setB("monOut2", a.monOut2); d.setB("monNet", a.monNet);
        d.setI("monPort", a.monPort); d.setB("monBindAll", a.monBindAll); d.setB("monWav", a.monWav);
    };
    if (ImGui::Checkbox(tr("Hear this channel"), &a.monOn)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Play the tuned channel through the speakers in any mode, to recognise a signal: the radio's samples are demodulated as USB, LSB, narrow FM or AM. Stays silent while the mode plays its own sound."));
    const dect2::MonitorStats ms = mon.stats();
    if (a.monOn) {
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Auto").x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x + 4 * gUi);
        if (ImGui::BeginCombo("##mondemod", dect2::hfDemodName(a.monDemod))) {
            for (int i = 0; i < dect2::HfDemodCount; i++) if (ImGui::Selectable(dect2::hfDemodName(i), a.monDemod == i)) { a.monDemod = i; save(); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Auto: USB below 30 MHz, narrow FM above"));
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::SliderFloat("##monvol", &a.monVol, 0.f, 1.f, "%.2f")) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Volume of the monitor"));
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(70 * gUi);
        float off = (float)a.monOffset;
        if (ImGui::InputFloat("Hz##monoff", &off, 0, 0, "%.0f")) { a.monOffset = off; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Fine tuning from the tuned frequency, in Hz (an SSB signal is heard 1 to 2 kHz above or below its carrier)"));
        flowNext(6 * gUi);
        if (ms.yielded) ImGui::TextDisabled("%s", tr("(the mode plays)"));
        else ImGui::TextDisabled("%.0f dB", ms.levelDb);
    }
    flowNext(6 * gUi);
    if (ImGui::SmallButton(tr("Outputs..."))) ImGui::OpenPopup("##audioout");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Where \"Hear this channel\" plays: the output device, a second output (for example a virtual audio cable for fldigi or WSJT-X) and a local network stream"));
    if (ImGui::BeginPopup("##audioout")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380 * gUi);
        static std::vector<std::string> devs;
        if (ImGui::IsWindowAppearing()) devs = dect2::AudioOut::devices();
        auto devCombo = [&](const char* id, std::string& cur) {
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo(id, cur.empty() ? tr("System default") : cur.c_str())) {
                if (ImGui::Selectable(tr("System default"), cur.empty())) { cur.clear(); save(); }
                for (const std::string& d : devs) if (ImGui::Selectable(d.c_str(), cur == d)) { cur = d; save(); }
                ImGui::EndCombo();
            }
        };
        ImGui::TextUnformatted(tr("Output 1"));
        devCombo("##mondev1", a.monDev);
        if (ImGui::Checkbox(tr("Output 2"), &a.monOut2)) save();
        if (a.monOut2) { devCombo("##mondev2", a.monDev2); ImGui::SetNextItemWidth(120 * gUi); if (ImGui::SliderFloat("##monvol2", &a.monVol2, 0.f, 1.f, "%.2f")) save(); }
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderInt(tr("Delay (ms)"), &a.monLatency, 20, 1000)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Audio queued before it starts playing: lower is quicker, higher survives a busy computer"));
        if (ImGui::Checkbox(tr("Stay silent while the mode plays"), &a.monYield)) save();
        ImGui::Separator();
        if (ImGui::Checkbox(tr("Network audio stream"), &a.monNet)) save();
        if (a.monNet) {
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::InputInt("Port##monport", &a.monPort, 0, 0)) { a.monPort = std::max(1024, std::min(65535, a.monPort)); save(); }
            if (ImGui::Checkbox(tr("WAV header"), &a.monWav)) save();
            if (ImGui::Checkbox(tr("Allow other computers"), &a.monBindAll)) save();
            ImGui::TextDisabled(a.monBindAll ? "0.0.0.0:%d" : "127.0.0.1:%d", a.monPort);
            ImGui::TextDisabled("%s", tr("16-bit stereo, 48 kHz. ffplay -f s16le -ar 48000 -ac 2 tcp://127.0.0.1:PORT, or a WAV player with the header on. Security: anyone who can reach the port can listen; leave 'Allow other computers' off unless you need it."));
            ImGui::TextDisabled("%s %d", tr("Listeners:"), ms.clients);
        }
        if (!ms.error.empty()) ImGui::TextColored(pal::warnAmber(), "%s", ms.error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    flowEnd();
}

} // namespace

void systemInfoText(std::string& os, std::string& cpu, std::string& gpu) {
    os = osName();
    cpu = cpuName();
    gpu = gpuName();
}

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
    uint32_t nEntries = 0;
    for (const auto& f : files) {
        if (dect2::voicekeys::isKeyMaterialPath(f.first)) continue;   // a voice key file never goes into a zip (key_guard.h)
        nEntries++;
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
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, nEntries); put16(out, nEntries);
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

bool writeZipFile(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) { return writeZip(path, files); }

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
                    if (!d.empty()) { std::error_code ec; std::filesystem::create_directories(dect2::u8file(d[0]), ec); dect2::openUrl(d[0]); }
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

// 4.0, the app top bar: IQ recording as one flat control. Stopped: a red dot and "rec" (greyed while the receiver is stopped or the source is a
// file); recording: a square, the time and the size, a click stops it. The same start and stop as Record IQ in the side panel, with its format.
void recordCompact(App& a) {
    const RecordingStats rs = a.engine.recordingStats();
    const bool isFile = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::File;
    const bool can = rs.active || (a.engine.running() && !isFile && a.devIdx >= 0);
    char b[96];
    if (rs.active) { const int t = (int)rs.seconds; snprintf(b, sizeof b, "%d:%02d  %s", t / 60, t % 60, sizeText(rs.bytes).c_str()); }
    else snprintf(b, sizeof b, "%s", tr("rec"));
    const float h = ImGui::GetFrameHeight(), mark = 12 * gUi;
    const ImVec2 ts = ImGui::CalcTextSize(b);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!can);
    const bool hit = ImGui::InvisibleButton("##rectopbtn", ImVec2(mark + ts.x + 8 * gUi, h));
    const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov && can) dl->AddRectFilled(p, ImVec2(p.x + mark + ts.x + 8 * gUi, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.6f));
    const ImVec2 c(p.x + 4 * gUi + 4 * gUi, p.y + h * 0.5f);
    const ImU32 red = ImGui::GetColorU32(can ? pal::badRed() : pal::grey());
    if (rs.active) dl->AddRectFilled(ImVec2(c.x - 3.5f * gUi, c.y - 3.5f * gUi), ImVec2(c.x + 3.5f * gUi, c.y + 3.5f * gUi), red);
    else dl->AddCircleFilled(c, 4 * gUi, red);
    dl->AddText(ImVec2(p.x + mark + 4 * gUi, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(rs.active ? pal::badRed() : can ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)), b);
    if (hit && can) {
        if (rs.active) a.engine.stopRecording();
        else {
            const FileFormat f = (FileFormat)recFileFormatOf(a.recFormat);
            const std::string path = recordingsDir() + "/" + (a.recFormat == 2 ? blueRecordingName(a.engine.recordingName(f)) : a.engine.recordingName(f));
            std::string err;
            if (a.engine.startRecording(path, f, err)) a.recDonePath.clear();
            else a.engine.log("recording failed: " + err);
        }
    }
    if (hov) ImGui::SetTooltip("%s", rs.active ? tr("Recording the radio's raw samples: click to stop") : isFile ? tr("Recordings of a file are not needed") : !can ? tr("Start the radio first") : tr("Record the radio's raw samples (IQ) to a file, in the format chosen under Record IQ in the side panel"));
}

// "What do I do next": one plain line about how the reception is going and, when a click helps, the one fix (app/ui/next_step_logic.h).
// maxW > 0 (4.0, the one action row above the tabs): a coloured dot, the line on one row cut to fit with the whole of it in the tooltip, then
// the fix button on the same row. maxW 0: the line wraps and the button goes under it, as before. Returns false when there is nothing to say.
bool nextStepLine(App& a, float maxW) {
    static double startedAt = -1;
    const double now = ImGui::GetTime();
    const bool run = a.engine.running();
    if (run && startedAt < 0) startedAt = now;
    if (!run) startedAt = -1;
    nextstep::In in;
    in.running = run;
    in.hasRadio = !a.devices.empty();
    in.radioHasGain = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio();
    in.autoGain = a.agcOn;
    in.levelCompensated = adcLevelCompensated();
    in.runSec = run ? now - startedAt : 0;
    const SampleLoss sl = a.engine.sampleLoss();
    in.network = sl.network;
    in.radioLoss = sl.live && (sl.rateLow || (sl.sinceSourceLostSec >= 0 && sl.sinceSourceLostSec < 5));   // a network radio's link was down
    const bool overflow = sl.live && sl.sinceCpuSec >= 0 && sl.sinceCpuSec < 5;   // which kind (overflow_cause.h): one at a start or retune is the start's own
    in.cpuLoss = overflow && sl.cpuCause == OverflowCause::Cpu;
    in.heldUp = overflow && (sl.cpuCause == OverflowCause::Waiting || sl.cpuCause == OverflowCause::Stalled);
    in.rmsDbfs = a.spec.stats.rmsDbfs;
    in.clipFraction = a.spec.stats.clipFraction;
    // the lock state is known for the modes whose telemetry the app reads here; the others get only the measured hints
    if (run && a.fmMode && a.rx.standard == 6) { in.lockKnown = true; in.locked = a.rx.fm.state == 2; in.snrDb = a.rx.fm.carrier ? a.rx.fm.snrDb : NAN; }
    else if (run && a.dabMode && a.rx.standard == 3) { in.lockKnown = true; in.locked = a.rx.dab.state == 2; in.snrDb = a.rx.dab.snrDb; }
    else if (run && !a.fmMode && !a.dabMode && a.rx.standard >= 0 && a.rx.standard <= 1) { in.lockKnown = true; in.locked = a.rx.state == 2; in.snrDb = a.rx.dataValid ? a.rx.dataSnrDb : NAN; }
    const nextstep::Out o = nextstep::advise(in);
    if (!o.text[0]) return false;
    // E10: where a quiet screen is normal the mode says so, instead of "no signal" or a general note
    std::string text = tr(o.text);
    if (const char* note = ease::quietNote(curModeName(a)); note && run && in.runSec >= nextstep::kSearchSec && (o.state == nextstep::State::Unknown || o.state == nextstep::State::NoSignal)) text = ease::quietLine(tr(note), in.runSec);
    // always one line, in the same place in every mode: grey while nothing needs doing, green when it works, amber when a step helps
    const ImVec4 col = o.needsAttention() ? ImVec4(0.95f, 0.78f, 0.25f, 1) : o.state == nextstep::State::Good ? pal::okGreen() : ImVec4(0.62f, 0.66f, 0.72f, 1);
    if (maxW > 0) {
        const float dot = 10 * gUi;
        const float fixW = o.fix != nextstep::Fix::None ? ImGui::CalcTextSize(tr(o.fixLabel)).x + 2 * ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x : 0.f;
        const std::string shown = ellipsize(text, std::max(40 * gUi, maxW - dot - fixW));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 3.5f * gUi, p.y + h * 0.5f), 3.5f * gUi, ImGui::GetColorU32(col));
        ImGui::SetCursorScreenPos(ImVec2(p.x + dot, p.y));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col, "%s", shown.c_str());
        if (shown != text && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(text.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (o.fix != nextstep::Fix::None) {
        ImGui::PushID("nextstep");
        if (maxW > 0) ImGui::SameLine();
        if (ImGui::SmallButton(tr(o.fixLabel))) {
            if (o.fix == nextstep::Fix::AutoGain) autogainRun(a, 1);
            else if (o.fix == nextstep::Fix::Start) startReceiver(a);
        }
        ImGui::PopID();
    }
    return true;
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
    sameLineIf(ImGui::CalcTextSize(tr("Save bug report")).x + 2 * ImGui::GetStyle().FramePadding.x + 4 * gUi);   // a narrow panel: on the next row
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
    // the three groups of the Source panel: Radio (above, with its status), Record, Listen; one left edge, a second column for the formats
    panel::subHeading(tr("Record"));
    recordGroupUi(a);
    panel::subHeading(tr("Listen"));
    listenGroupUi(a);
}

// the Record group of the Source panel, also the Workspace layout's Recorder block (app/canvas)
void recordGroupUi(App& a) {
    recordUi(a, 1);              // [dot] Record IQ     [8-bit]
    flowEnd();
    recordUi(a, 2);              // SigMF  Rewind  Library
    flowNext(6 * gUi);
    timeMachineUi(a);
    flowNext(6 * gUi);
    recordingLibraryUi(a);
    flowEnd();
    audioRecordUi(a, 1);         // [dot] Record audio  [WAV]
    flowNext(6 * gUi);
    clipsui::button(a);
    flowEnd();
    audioRecordUi(a, 2);         // Skip silence  Split  Keep recording: they belong to the audio recording
    flowEnd();
}

// the Listen group (the monitor: Hear this channel, its demodulator and volume, the output devices), also the Audio output block
void listenGroupUi(App& a) {
    audioMonitorUi(a);           // Hear this channel  [demod]  volume  Outputs...
}
