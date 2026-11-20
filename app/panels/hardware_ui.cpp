// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Hardware: this computer (OS, processor, memory, graphics), the minimum requirements and how it measures up, and every media
// codec, container and capture input OnAir uses with a green tick or a red cross, and the USB radio drivers (the system part, the driver
// libraries, every radio plugged in). "Copy to clipboard" gives the whole page as text.
// The lists and the rules: hardware_logic.h. The hardware encoders are tried on a worker thread (a few black pictures each), as the
// video generator does, so the window opens at once and fills in.
#include "../app.h"
#include "hardware_logic.h"
#include "../ui/icons.h"
#include "../ui/theme.h"
#include "dect2/tv/media_probe.h"
#include "dect2/tv/videogen_params.h"
#include "dect2/audio/videoin.h"
#include "dect2/audio/ac4_dec.h"
#include "dect2/tv/avsplus_dec.h"
#include "dect2/cdr/cdr_dra.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/codec/optional_libs.h"
#include "dect2/source/usb_diag.h"
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace hwinfo;

unsigned thisOs() {
#if defined(_WIN32)
    return kWin;
#elif defined(__APPLE__)
    return kMac;
#else
    return kLinux;
#endif
}

uint64_t ramBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (uint64_t)m.ullTotalPhys : 0;
#elif defined(__APPLE__)
    uint64_t v = 0; size_t n = sizeof v;
    return sysctlbyname("hw.memsize", &v, &n, nullptr, 0) == 0 ? v : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? (uint64_t)pages * (uint64_t)size : 0;
#endif
}

const char* archName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86 (32-bit)";
#elif defined(__arm__)
    return "arm (32-bit)";
#else
    return "";
#endif
}

// GL_VERSION of the window's context; empty with Metal or without a context
std::string glVersionText() {
    typedef const unsigned char* (*GetString)(unsigned);
    if (!glfwGetCurrentContext()) return "";
    auto gs = (GetString)glfwGetProcAddress("glGetString");
    const unsigned char* r = gs ? gs(0x1F02 /* GL_VERSION */) : nullptr;
    return r ? std::string((const char*)r) : std::string();
}

Facts readFacts() {
    Facts f;
    std::string gpu;
    systemInfoText(f.osText, f.cpuText, gpu);
    f.gpuText = gpu;
    f.os = parseOsText(f.osText, f.osMajor, f.osMinor, f.osBuild);
    f.is64bit = sizeof(void*) == 8;
    f.arch = archName();
    f.threads = (int)std::thread::hardware_concurrency();
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    f.x86 = true;
    __builtin_cpu_init();
    f.avx2 = __builtin_cpu_supports("avx2");
    f.fma = __builtin_cpu_supports("fma");
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    f.neon = true;
#endif
#ifdef DECT2_REQUIRES_AVX2
    f.avx2Build = true;
#endif
    f.ramBytes = ramBytes();
    f.glVersionText = glVersionText();
#ifdef __APPLE__
    f.metal = f.glVersionText.empty();
#endif
    if (!f.glVersionText.empty()) parseGlVersion(f.glVersionText, f.glMajor, f.glMinor, f.gles);
    const dect2::GpuLdpc& g = dect2::GpuLdpc::instance();
    f.gpuCompute = g.available();
#if defined(_WIN32)
    const char* api = "Direct3D 11";
#elif defined(__APPLE__)
    const char* api = "Metal";
#else
    const char* api = "Vulkan";
#endif
    f.gpuComputeText = f.gpuCompute ? std::string(g.deviceName()) + " (" + api + ")" : std::string("none: ") + g.unavailableReason();
    return f;
}

// A video encoder counts when the video generator's trial encode worked (videogen_params.cpp tryVideo); else the reason it gave.
MediaStatus videoEncoderStatus(const Media& m) {
    MediaStatus s;
    if (!dect2::media::ffmpegHas(dect2::media::FfKind::Encoder, m.name)) { s.note = "not in this FFmpeg"; return s; }
    for (const auto& e : dect2::vgen::videoEncoders())
        if (e.name == m.name) {
            s.ok = true;
            if (m.hardware && !e.hardware) s.note = "Windows software encoder (no graphics-card encoder)";
            return s;
        }
    for (const auto& p : dect2::vgen::videoEncoderProblems())
        if (p.name.rfind(m.name, 0) == 0) { s.note = p.reason; return s; }
    s.note = m.hardware ? "no hardware for it on this computer" : "does not open";
    return s;
}

MediaStatus audioEncoderStatus(const Media& m) {
    MediaStatus s;
    for (const auto& e : dect2::vgen::audioEncoders()) if (e.name == m.name) { s.ok = true; return s; }
    s.note = dect2::media::ffmpegHas(dect2::media::FfKind::Encoder, m.name) ? "in this FFmpeg, but does not open" : "not in this FFmpeg";
    return s;
}

MediaStatus addOnStatus(const Media& m) {
    MediaStatus s;
    const std::string k = m.name;
    if (k == "ac4" || k == "avsplus") {
        const auto show = [&s](bool has, bool found, const std::string& path, const std::string& msg) {
            s.ok = has;
            s.note = has ? "loaded: " + path : found ? msg : "not installed (Help > Voice codecs)";
        };
        if (k == "ac4") { const auto li = dect2::ac4Library(); show(li.hasDecoder, li.found, li.path, li.message); }
        else { const auto li = dect2::avsPlusLibrary(); show(li.hasDecoder, li.found, li.path, li.message); }
    } else if (k == "dra") {
        std::string p, prob;
        s.ok = dect2::cdr::draAvailable(&p, &prob);
        s.note = s.ok ? "loaded: " + p : !prob.empty() ? prob : "not installed (Help > Voice codecs)";
    }
    return s;
}

std::vector<MediaLine> probeMedia() {
    namespace md = dect2::media;
    std::vector<MediaLine> out;
    for (const Media* m : mediaFor(thisOs())) {
        MediaStatus s;
        switch (m->kind) {
        case MediaKind::VideoEncoder: s = videoEncoderStatus(*m); break;
        case MediaKind::AudioEncoder: s = audioEncoderStatus(*m); break;
        case MediaKind::Decoder: s.ok = md::ffmpegHas(md::FfKind::Decoder, m->name); break;
        case MediaKind::Muxer: s.ok = md::ffmpegHas(md::FfKind::Muxer, m->name); break;
        case MediaKind::Demuxer: s.ok = md::ffmpegHas(md::FfKind::Demuxer, m->name); break;
        case MediaKind::Capture: {
            s.ok = dect2::videoInputSupported();
            const size_t n = s.ok ? dect2::listVideoInputs().size() : 0;
            s.note = s.ok ? std::to_string(n) + (n == 1 ? " device found" : " devices found") : "this build has no camera input (FFmpeg without libavdevice)";
            break;
        }
        case MediaKind::AddOn: s = addOnStatus(*m); break;
        }
        if (!s.ok && s.note.empty()) s.note = "not in this FFmpeg";
        out.push_back(MediaLine{m, s});
    }
    return out;
}

// ---- USB radio drivers (worker thread: the library search looks in folders, the bus scan opens WinUSB radios for a moment) ----

bool fileThere(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

void systemUsbLines(std::vector<UsbLine>& out) {
#if defined(_WIN32)
    const char* root = getenv("SystemRoot");
    const std::string dll = std::string(root && *root ? root : "C:\\Windows") + "\\System32\\winusb.dll";
    UsbLine l{UsbGroup::System, "WinUSB", "RTL-SDR, HackRF, Airspy, RX888, USRP, bladeRF and the other libusb radios", fileThere(dll), false, ""};
    l.note = l.ok ? "part of Windows; give each radio the WinUSB driver once with Zadig (zadig.akeo.ie)" : dll + " is missing";
    out.push_back(l);
#elif defined(__APPLE__)
    out.push_back(UsbLine{UsbGroup::System, "USB access", "every USB radio", true, false, "macOS needs no extra driver for USB radios"});
#elif defined(__linux__) && !defined(__ANDROID__)
    std::string rules;
    for (const char* p : {"/etc/udev/rules.d/60-onair-sdr.rules", "/usr/lib/udev/rules.d/60-onair-sdr.rules", "/lib/udev/rules.d/60-onair-sdr.rules"})
        if (rules.empty() && fileThere(p)) rules = p;
    out.push_back(UsbLine{UsbGroup::System, "udev rules (60-onair-sdr.rules)", "opening USB radios without root", !rules.empty(), false,
                          rules.empty() ? "install the .deb, or run install-udev-rules.sh from the portable folder, then replug the radio" : rules});
    const auto blacklist = [&out](const char* file, const char* name, const char* usedFor, const char* module, const char* what) {
        const std::string p = std::string("/etc/modprobe.d/") + file;
        UsbLine l{UsbGroup::System, name, usedFor, fileThere(p), true, p};
        if (!l.ok) {
            l.note = "the .deb or install-udev-rules.sh installs it";
            if (fileThere(std::string("/sys/module/") + module)) l.note += std::string("; the kernel driver ") + module + " is loaded and takes " + what;
        }
        out.push_back(l);
    };
    blacklist("onair-rtlsdr-blacklist.conf", "RTL-SDR TV driver blacklist", "RTL-SDR dongles", "dvb_usb_rtl28xxu", "RTL-SDR dongles as TV sticks");
    blacklist("onair-sdrplay-blacklist.conf", "SDRplay Mirics driver blacklist", "SDRplay RSP radios", "msi2500", "SDRplay RSPs");
#else
    (void)out;
#endif
}

std::vector<UsbLine> probeUsb() {
    std::vector<UsbLine> out;
    systemUsbLines(out);
    for (const auto& l : dect2::codec::optionalLibraries()) {
        if (l.group != "Radio drivers" || l.id == "hamlib") continue;   // Hamlib is CAT rig control, not a USB radio driver
        out.push_back(UsbLine{UsbGroup::Library, l.name, l.usedFor, l.found, true, l.found ? l.foundPath : "get it from " + l.source});
    }
    std::vector<UsbRadioSeen> radios;
    for (const auto& r : dect2::usbRadiosPresent()) radios.push_back(UsbRadioSeen{r.radio, r.vid, r.pid, r.problem});
    std::vector<std::string> hints = dect2::usbRadioHints();
    for (auto* more : {&dect2::sdrplayApiHints, &dect2::spectranApiHints, &dect2::rsaApiHints, &dect2::limeLibraryHints})
        for (auto& h : more()) hints.push_back(h);
    for (auto& l : usbRadioLines(radios, hints)) out.push_back(l);
    return out;
}

struct Probe {
    std::vector<MediaLine> media;
    std::vector<UsbLine> usb;
};

Probe probeAll() { return Probe{probeMedia(), probeUsb()}; }

struct State {
    bool init = false;
    Facts facts;
    std::vector<Check> checks;
    std::string ffVersion;
    std::future<Probe> probe;
    std::vector<MediaLine> media;
    std::vector<UsbLine> usb;
    bool probed = false;
    std::string copiedNote;
    double copiedAt = -1;
    double usbCopiedAt = -1;   // the USB section's own copy button
};

State& st() { static State s; return s; }

void refresh(State& s) {
    s.facts = readFacts();   // on the UI thread: it reads the window's OpenGL context
    s.checks = evaluate(s.facts);
    s.ffVersion = dect2::media::ffmpegVersion();
    if (!s.probe.valid()) { s.probed = false; s.probe = std::async(std::launch::async, probeAll); }
    s.init = true;
}

// a tick, a cross or a warning sign the height of the text line, then the cursor after it on the same line
void mark(Verdict v) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    const Ic ic = v == Verdict::Pass ? Ic::Check : v == Verdict::Fail ? Ic::Cross : Ic::Warning;
    const ImVec4 c = v == Verdict::Pass ? pal::okGreen() : v == Verdict::Fail ? pal::badRed() : pal::warnAmber();
    icons::draw(ic, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.85f, ImGui::GetColorU32(c));
}

void systemSection(const State& s) {
    const Facts& f = s.facts;
    if (!ImGui::BeginTable("##hwsys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
    const auto row = [](const char* k, const std::string& v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", v.c_str());
    };
    row(tr("Operating system"), f.osText);
    row(tr("Processor"), f.cpuText + (f.arch.empty() ? "" : ", " + f.arch));
    std::string ins;
    if (f.x86) ins = std::string("SSE2") + (f.avx2 ? ", AVX2" : ", no AVX2") + (f.fma ? ", FMA" : ", no FMA");
    else if (f.neon) ins = "NEON";
    if (!ins.empty()) row(tr("Instructions"), ins);
    row(tr("Memory (RAM)"), f.ramBytes ? gbText(f.ramBytes) : std::string("unknown"));
    row(tr("Graphics card"), f.gpuText);
    row(tr("Graphics API"), f.metal ? std::string("Metal") : f.glVersionText.empty() ? std::string("unknown") : (f.gles ? "" : "OpenGL ") + f.glVersionText);
    row(tr("GPU computing"), f.gpuComputeText);
    row(tr("FFmpeg"), s.ffVersion);
    ImGui::EndTable();
}

void requirementsSection(const State& s) {
    const Verdict all = overall(s.checks);
    mark(all);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", tr(overallText(all)));
    if (!ImGui::BeginTable("##hwreq", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("##st", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4 * gUi);
    ImGui::TableSetupColumn(tr("Item"));
    ImGui::TableSetupColumn(tr("Minimum"));
    ImGui::TableSetupColumn(tr("Recommended"));
    ImGui::TableSetupColumn(tr("This computer"));
    ImGui::TableHeadersRow();
    for (const Check& c : s.checks) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); mark(c.verdict);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(verdictWord(c.verdict)));
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", tr(c.item.c_str()));
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.minimum.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.recommended.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.found.c_str());
    }
    ImGui::EndTable();
    ImGui::TextDisabled("%s", tr("Green: OK. Amber: works, below the recommended. Red: below the minimum."));
}

void mediaSection(const State& s) {
    if (!s.probed) {
        ImGui::TextDisabled("%s", tr("Trying the encoders of this computer..."));
        return;
    }
    int works = 0;
    for (const auto& l : s.media) works += l.s.ok ? 1 : 0;
    ImGui::Text("%d / %d %s", works, (int)s.media.size(), tr("work on this computer"));
    ImGui::TextDisabled("%s", tr("A red cross on an optional line is not a problem: OnAir uses another encoder, or the add-on is only for some countries."));
    if (!ImGui::BeginTable("##hwmedia", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("##st", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4 * gUi);
    ImGui::TableSetupColumn(tr("Media"));
    ImGui::TableSetupColumn(tr("FFmpeg name"));
    ImGui::TableSetupColumn(tr("Used for"));
    ImGui::TableSetupColumn(tr("Status"));
    ImGui::TableHeadersRow();
    int lastKind = -1;
    for (const auto& l : s.media) {
        if ((int)l.m->kind != lastKind) {
            lastKind = (int)l.m->kind;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn(); ImGui::TextColored(pal::heading(), "%s", tr(kindTitle(l.m->kind)));
        }
        ImGui::PushID(l.m);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); mark(l.s.ok ? Verdict::Pass : Verdict::Fail);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", l.m->label);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", l.m->name);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", l.m->usedFor);
        ImGui::TableNextColumn();
        if (l.s.ok) ImGui::TextWrapped("%s%s%s", tr("works"), l.s.note.empty() ? "" : ": ", l.s.note.c_str());
        else ImGui::TextWrapped("%s%s%s", l.m->optional ? tr("missing (optional)") : tr("missing"), l.s.note.empty() ? "" : ": ", l.s.note.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void usbSection(State& s) {
    if (!s.probed) {
        ImGui::TextDisabled("%s", tr("Looking for the radio drivers and the radios plugged in..."));
        return;
    }
    if (ImGui::Button(tr("Copy to clipboard##usb"))) {
        ImGui::SetClipboardText(usbReportText(s.usb).c_str());
        s.usbCopiedAt = ImGui::GetTime();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies the USB radio drivers list as text"));
    if (s.usbCopiedAt >= 0 && ImGui::GetTime() - s.usbCopiedAt < 2.5) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "%s", tr("Copied")); }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("A driver library is only needed for its own radio. A radio with a red cross is plugged in but cannot be opened: the line says why."));
    ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##hwusb", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("##st", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4 * gUi);
    ImGui::TableSetupColumn(tr("Driver or radio"));
    ImGui::TableSetupColumn(tr("Used for"));
    ImGui::TableSetupColumn(tr("Status"));
    ImGui::TableHeadersRow();
    for (UsbGroup g : {UsbGroup::System, UsbGroup::Library, UsbGroup::Radio}) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::heading(), "%s", tr(usbGroupTitle(g)));
        bool any = false;
        for (const auto& l : s.usb) {
            if (l.group != g) continue;
            any = true;
            ImGui::PushID(&l);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); mark(l.ok ? Verdict::Pass : Verdict::Fail);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", l.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", l.usedFor.c_str());
            ImGui::TableNextColumn();
            const char* word = l.ok ? (g == UsbGroup::Radio ? tr("ready") : tr("installed")) : l.optional ? tr("missing (optional)") : g == UsbGroup::Radio ? tr("cannot be opened") : tr("missing");
            ImGui::TextWrapped("%s%s%s", word, l.note.empty() ? "" : ": ", l.note.c_str());
            ImGui::PopID();
        }
        if (!any && g == UsbGroup::Radio) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr("No radio OnAir knows is plugged in."));
        }
    }
    ImGui::EndTable();
}

} // namespace

void hardwareWindow(App& a) {
    if (!a.hardwareOpen) { State& s = st(); s.init = false; return; }
    State& s = st();
    if (!s.init) refresh(s);
    if (!s.probed && s.probe.valid() && s.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { Probe p = s.probe.get(); s.media = std::move(p.media); s.usb = std::move(p.usb); s.probed = true; }
    firstWindowSize(860 * gUi, 620 * gUi);
    if (!ImGui::Begin(trw("Hardware"), &a.hardwareOpen)) { ImGui::End(); return; }
    ImGui::BeginDisabled(!s.probed);
    if (ImGui::Button(tr("Copy to clipboard"))) {
        ImGui::SetClipboardText(reportText(ONAIR_VERSION, s.facts, s.checks, s.media, s.ffVersion, s.usb).c_str());
        s.copiedAt = ImGui::GetTime();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Copies everything on this page as text, for a bug report or a forum post"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Look again"))) { s.init = false; }
    if (s.copiedAt >= 0 && ImGui::GetTime() - s.copiedAt < 2.5) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "%s", tr("Copied")); }
    ImGui::Separator();
    if (ImGui::BeginChild("##hwbody")) {
        if (ImGui::CollapsingHeader(trw("This computer"), ImGuiTreeNodeFlags_DefaultOpen)) systemSection(s);
        if (ImGui::CollapsingHeader(trw("Minimum requirements"), ImGuiTreeNodeFlags_DefaultOpen)) requirementsSection(s);
        if (ImGui::CollapsingHeader(trw("Media support"), ImGuiTreeNodeFlags_DefaultOpen)) mediaSection(s);
        if (ImGui::CollapsingHeader(trw("USB radio drivers"), ImGuiTreeNodeFlags_DefaultOpen)) usbSection(s);
    }
    ImGui::EndChild();
    ImGui::End();
}
