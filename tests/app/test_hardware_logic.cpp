// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Hardware (app/panels/hardware_logic.h): the media list, the OS / OpenGL parsers, the requirement verdicts, the USB radio lines and
// the clipboard text.
#include "../../app/panels/hardware_logic.h"
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;

static void fail(const char* fmt, ...) {
    std::string b(512, '\0');
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b.data(), b.size(), fmt, ap);
    va_end(ap);
    failures++;
    std::printf("FAIL: %s\n", b.c_str());
}
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

using namespace hwinfo;

static Facts goodPc() {
    Facts f;
    f.os = OsFamily::Windows; f.osText = "Windows 10.0 build 22631"; f.osMajor = 10; f.osBuild = 22631;
    f.is64bit = true; f.arch = "x86-64"; f.cpuText = "Test CPU (8 threads)"; f.threads = 8;
    f.x86 = true; f.avx2 = f.fma = true;
    f.ramBytes = 16 * kGiB;
    f.gpuText = "Test GPU"; f.glVersionText = "4.6.0 Test 1.0"; f.glMajor = 4; f.glMinor = 6;
    f.gpuCompute = true; f.gpuComputeText = "Test GPU (Direct3D 11)";
    return f;
}

static Verdict verdictOf(const std::vector<Check>& cs, const char* item) {
    for (const auto& c : cs) if (c.item == item) return c.verdict;
    return Verdict::Unknown;
}

int main() {
    // the media table: every row has a name, a label and a use; no row twice within its kind; each OS has the base codecs
    {
        std::set<std::string> seen;
        for (const auto& m : mediaTable()) {
            CHECK(m.name && *m.name && m.label && *m.label && m.usedFor && *m.usedFor, "row %s is complete", m.name);
            CHECK(m.os != 0, "row %s is for some OS", m.name);
            CHECK(seen.insert(std::to_string((int)m.kind) + ":" + m.name).second, "row %s is listed once in its kind", m.name);
            CHECK(!m.hardware || m.kind == MediaKind::VideoEncoder, "only video encoders are hardware (%s)", m.name);
            CHECK(*kindTitle(m.kind), "kind of %s has a title", m.name);
        }
        for (unsigned os : {(unsigned)kWin, (unsigned)kMac, (unsigned)kLinux}) {
            const auto rows = mediaFor(os);
            int kind = -1; bool ordered = true;
            for (const Media* m : rows) { if ((int)m->kind < kind) ordered = false; kind = (int)m->kind; }
            CHECK(ordered, "OS %u: rows grouped by kind", os);
            const auto has = [&rows](MediaKind k, const char* n) { for (const Media* m : rows) if (m->kind == k && std::string(m->name) == n) return true; return false; };
            CHECK(has(MediaKind::Decoder, "h264") && has(MediaKind::Decoder, "hevc") && has(MediaKind::Decoder, "aac"), "OS %u: the TV decoders", os);
            CHECK(has(MediaKind::VideoEncoder, "mpeg2video") && has(MediaKind::Muxer, "mpegts"), "OS %u: the generator's encoder and muxer", os);
            CHECK(has(MediaKind::VideoEncoder, "h264_mf") == (os == kWin), "OS %u: Media Foundation only on Windows", os);
            CHECK(has(MediaKind::VideoEncoder, "h264_videotoolbox") == (os == kMac), "OS %u: VideoToolbox only on macOS", os);
            CHECK(has(MediaKind::VideoEncoder, "h264_vaapi") == (os == kLinux), "OS %u: VA-API only on Linux", os);
        }
    }
    // parsers
    {
        int a = 0, b = 0, c = 0; bool es = false;
        CHECK(parseOsText("Windows 10.0 build 22631", a, b, c) == OsFamily::Windows && a == 10 && b == 0 && c == 22631, "Windows text: %d %d %d", a, b, c);
        CHECK(parseOsText("macOS 15.1", a, b, c) == OsFamily::MacOS && a == 15 && b == 1 && c == 0, "macOS text: %d %d %d", a, b, c);
        CHECK(parseOsText("Windows", a, b, c) == OsFamily::Windows && a == 0, "Windows without numbers");
        CHECK(parseOsText("Ubuntu 26.04 LTS (kernel 7.0.0)", a, b, c) == OsFamily::Linux && a == 0, "Linux text: family only");
        CHECK(parseOsText("", a, b, c) == OsFamily::Other, "empty: other");
        CHECK(parseGlVersion("3.3 (Core Profile) Mesa 24.0.5", a, b, es) && a == 3 && b == 3 && !es, "Mesa GL: %d.%d", a, b);
        CHECK(parseGlVersion("4.6.0 NVIDIA 550.54.14", a, b, es) && a == 4 && b == 6, "NVIDIA GL: %d.%d", a, b);
        CHECK(parseGlVersion("OpenGL ES 3.1 Mesa 23.2", a, b, es) && a == 3 && b == 1 && es, "GLES: %d.%d", a, b);
        CHECK(!parseGlVersion("no numbers here", a, b, es), "no version");
    }
    // verdicts
    {
        const auto cs = evaluate(goodPc());
        CHECK(overall(cs) == Verdict::Pass, "a good PC passes (%s)", verdictWord(overall(cs)));
        for (const auto& c : cs) CHECK(!c.minimum.empty() && !c.recommended.empty() && !c.found.empty(), "%s has all columns", c.item.c_str());

        Facts f = goodPc(); f.osBuild = 19045;   // Windows 10 22H2
        CHECK(verdictOf(evaluate(f), "Operating system") == Verdict::Warn, "Windows 10: works, Windows 11 recommended");
        f = goodPc(); f.osMajor = 6; f.osMinor = 1; f.osBuild = 7601;
        CHECK(verdictOf(evaluate(f), "Operating system") == Verdict::Fail, "Windows 7 fails");
        f = goodPc(); f.os = OsFamily::MacOS; f.osMajor = 14;
        CHECK(verdictOf(evaluate(f), "Operating system") == Verdict::Fail, "macOS 14 fails");
        f = goodPc(); f.is64bit = false; f.arch = "";
        CHECK(verdictOf(evaluate(f), "64-bit") == Verdict::Fail && overall(evaluate(f)) == Verdict::Fail, "32-bit fails");
        f = goodPc(); f.threads = 2;
        CHECK(verdictOf(evaluate(f), "Processor threads") == Verdict::Warn, "2 threads: below recommended");
        f.threads = 1;
        CHECK(verdictOf(evaluate(f), "Processor threads") == Verdict::Fail, "1 thread fails");
        f = goodPc(); f.avx2 = false;
        CHECK(verdictOf(evaluate(f), "Processor instructions") == Verdict::Warn, "no AVX2 on a baseline build: warn");
        f.avx2Build = true;
        CHECK(verdictOf(evaluate(f), "Processor instructions") == Verdict::Fail, "no AVX2 on an AVX2 build: fail");
        f = goodPc(); f.fma = false;
        CHECK(verdictOf(evaluate(f), "Processor instructions") == Verdict::Warn, "AVX2 without FMA: the fast paths are not used");
        f = goodPc(); f.x86 = false; f.arch = "arm64"; f.neon = true;
        CHECK(verdictOf(evaluate(f), "Processor instructions") == Verdict::Pass, "arm64 with NEON");
        f = goodPc(); f.ramBytes = 4 * kGiB - 300ull * 1024 * 1024;   // a 4 GB machine reports a little less
        CHECK(verdictOf(evaluate(f), "Memory (RAM)") == Verdict::Warn, "a 4 GB machine meets the minimum");
        f.ramBytes = 2 * kGiB;
        CHECK(verdictOf(evaluate(f), "Memory (RAM)") == Verdict::Fail, "2 GB fails");
        f.ramBytes = 0;
        CHECK(verdictOf(evaluate(f), "Memory (RAM)") == Verdict::Unknown && overall(evaluate(f)) == Verdict::Warn, "unknown RAM: warn overall");
        f = goodPc(); f.glMajor = 3; f.glMinor = 1;
        CHECK(verdictOf(evaluate(f), "Graphics") == Verdict::Warn, "OpenGL 3.1 (Raspberry Pi 5): works");
        f.glMajor = 2; f.glMinor = 1;
        CHECK(verdictOf(evaluate(f), "Graphics") == Verdict::Fail, "OpenGL 2.1 fails");
        f = goodPc(); f.glMajor = 3; f.glMinor = 2; f.gles = true;
        CHECK(verdictOf(evaluate(f), "Graphics") == Verdict::Warn, "OpenGL ES: works, desktop GL recommended");
        f = goodPc(); f.glMajor = 0; f.metal = true;
        CHECK(verdictOf(evaluate(f), "Graphics") == Verdict::Pass, "Metal passes");
        f = goodPc(); f.gpuCompute = false;
        CHECK(verdictOf(evaluate(f), "GPU computing") == Verdict::Warn && overall(evaluate(f)) == Verdict::Warn, "no GPU compute: only a warning");
        CHECK(*overallText(Verdict::Pass) && *overallText(Verdict::Warn) && *overallText(Verdict::Fail), "overall texts");
    }
    // USB radios: ids, a problem from the scan, hints to the right radio (longest name), hints no radio fits
    {
        CHECK(usbIdText(0x0bda, 0x2838) == "0bda:2838" && usbIdText(0x1D50, 0x60A1) == "1d50:60a1" && usbIdText(0, 0) == "0000:0000",
              "usb id text %s", usbIdText(0x0bda, 0x2838).c_str());
        const std::vector<UsbRadioSeen> radios = {
            {"RTL-SDR", 0x0bda, 0x2838, ""},
            {"HackRF", 0x1d50, 0x6089, ""},
            {"HackRF Jawbreaker", 0x1d50, 0x604b, ""},
            {"Airspy", 0x1d50, 0x60a1, "On Windows this radio needs the WinUSB driver: install it once with Zadig (zadig.akeo.ie)"},
            {"SDRplay RSP", 0x1df7, 0x3000, ""},
        };
        const std::vector<std::string> hints = {
            "The Linux TV driver dvb_usb_rtl28xxu holds the RTL-SDR: run sudo modprobe -r dvb_usb_rtl28xxu",
            "HackRF Jawbreaker is plugged in but this user may not open it: install OnAir's udev rules",
            "An SDRplay RSP is plugged in but the SDRplay API is not installed: install the free Hardware API",
            "An Aaronia SPECTRAN is plugged in but the Aaronia RTSA-Suite PRO is not installed",
        };
        const auto ls = usbRadioLines(radios, hints);
        CHECK(ls.size() == 6, "5 radios and 1 loose hint: %zu lines", ls.size());
        const auto line = [&ls](const std::string& name) -> const UsbLine* { for (const auto& l : ls) if (l.name == name) return &l; return nullptr; };
        const UsbLine* rtl = line("RTL-SDR (0bda:2838)");
        const UsbLine* hack = line("HackRF (1d50:6089)");
        const UsbLine* jaw = line("HackRF Jawbreaker (1d50:604b)");
        const UsbLine* air = line("Airspy (1d50:60a1)");
        const UsbLine* rsp = line("SDRplay RSP (1df7:3000)");
        CHECK(rtl && !rtl->ok && rtl->note.find("dvb_usb_rtl28xxu") != std::string::npos, "the TV driver hint goes to the RTL-SDR");
        CHECK(hack && hack->ok && hack->note.empty(), "the Jawbreaker hint does not go to the plain HackRF");
        CHECK(jaw && !jaw->ok && jaw->note.find("udev rules") != std::string::npos, "the Jawbreaker gets its hint");
        CHECK(air && !air->ok && air->note.find("Zadig") != std::string::npos, "the scan's driver problem stays");
        CHECK(rsp && !rsp->ok && rsp->note.find("SDRplay API") != std::string::npos, "the SDRplay API hint goes to the RSP");
        CHECK(ls.back().name == "USB device" && !ls.back().ok && ls.back().note.find("SPECTRAN") != std::string::npos, "a hint without a radio gets its own line");
        for (const auto& l : ls) CHECK(l.group == UsbGroup::Radio, "%s is a radio line", l.name.c_str());
        CHECK(usbRadioLines({}, {}).empty(), "nothing plugged in: no lines");
        const auto twice = usbRadioLines({{"RTL-SDR", 0x0bda, 0x2838, ""}, {"RTL-SDR", 0x0bda, 0x2832, ""}}, {hints[0], hints[0]});
        CHECK(twice.size() == 2 && !twice[0].ok && !twice[1].ok && twice[0].note == hints[0], "two RTL-SDRs both get the hint once");
        CHECK(*usbGroupTitle(UsbGroup::System) && *usbGroupTitle(UsbGroup::Library) && *usbGroupTitle(UsbGroup::Radio), "group titles");
    }
    // the clipboard text
    {
        const Facts f = goodPc();
        const auto cs = evaluate(f);
        const auto rows = mediaFor(kWin);
        std::vector<MediaLine> ml;
        for (const Media* m : rows) ml.push_back(MediaLine{m, MediaStatus{std::string(m->name) != "h264_nvenc", std::string(m->name) == "h264_nvenc" ? "no NVIDIA card" : ""}});
        std::vector<UsbLine> usb = {
            {UsbGroup::System, "WinUSB", "libusb radios", true, false, "part of Windows"},
            {UsbGroup::Library, "librtlsdr (RTL-SDR)", "RTL-SDR dongles", false, true, "not found (https://osmocom.org)"},
        };
        const std::string none = reportText("4.1.0", f, cs, ml, "8.0", usb);
        CHECK(none.find("  Radios plugged in\n    none of the radios OnAir knows\n") != std::string::npos, "report without radios says so");
        for (auto& l : usbRadioLines({UsbRadioSeen{"HackRF", 0x1d50, 0x6089, ""}}, {})) usb.push_back(l);
        const std::string t = reportText("4.1.0", f, cs, ml, "8.0 (libavcodec 62.11.100)", usb);
        CHECK(t.rfind("OnAir 4.1.0 - hardware\n", 0) == 0, "starts with the version");
        for (const char* want : {"Windows 10.0 build 22631", "Test CPU (8 threads), x86-64", "16.0 GB", "Test GPU (4.6.0 Test 1.0)", "8.0 (libavcodec 62.11.100)",
                                 "[x] Operating system", "[ ] H.264 NVIDIA NVENC (h264_nvenc): no NVIDIA card", "[x] MPEG-2 video (mpeg2video)", "  Decoders\n",
                                 "USB radio drivers ([x] works, [ ] missing)\n  Operating system\n    [x] WinUSB: part of Windows\n",
                                 "  Radio driver libraries\n    [ ] librtlsdr (RTL-SDR): not found (https://osmocom.org)\n",
                                 "  Radios plugged in\n    [x] HackRF (1d50:6089)\n"})
            CHECK(t.find(want) != std::string::npos, "report has '%s'", want);
        CHECK(t.find("VideoToolbox") == std::string::npos, "no macOS rows in a Windows report");
        CHECK(t.find("none of the radios") == std::string::npos, "a report with a radio does not say none");
        const std::string u = usbReportText(usb);
        CHECK(u.rfind("USB radio drivers ([x] works, [ ] missing)\n", 0) == 0 && t.find(u) != std::string::npos, "the USB copy is the USB part of the report");
        CHECK(u.find("Media support") == std::string::npos && u.find("Requirements") == std::string::npos, "the USB copy has only the USB part");
    }
    std::printf("%s\n", failures ? "FAILED" : "hardware_logic ok");
    return failures ? 1 : 0;
}
