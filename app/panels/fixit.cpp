#include "fixit.h"
#include "../i18n/i18n.h"
#include <cctype>

namespace fixit {

namespace {
constexpr const char* kZadig = "https://zadig.akeo.ie";
constexpr const char* kSdrplay = "https://sdrplay.com/hardware-api/";
constexpr const char* kHackrfFw = "https://hackrf.readthedocs.io/en/latest/updating_firmware.html";
constexpr const char* kCodecs = "https://github.com/adswill/OnAir/blob/main/docs/CODECS.md";
constexpr const char* kDevices = "https://github.com/adswill/OnAir/blob/main/DEVICES.md";
constexpr const char* kAaronia = "https://aaronia.com/downloads";
constexpr const char* kSpectranDoc = "https://github.com/adswill/OnAir/blob/main/docs/SPECTRAN-V6.md";
constexpr const char* kSddcReleases = "https://github.com/renardspark/SDDC_Driver/releases";
constexpr const char* kUhdImages = "https://files.ettus.com/manual/page_images.html";
constexpr const char* kUdevCmd = "sudo ./install-udev-rules.sh   # in the OnAir folder; then unplug and replug the radio";
#define NONE {nullptr}

const Entry kTable[] = {
    // more specific patterns first: lookup() takes the first that occurs
    {"sdrplay_missing", "SDRplay API is not installed", N_("Install the SDRplay API"),
     N_("An SDRplay RSP is plugged in, but the SDRplay Hardware API is not installed. Install it separately even if SDRconnect already works with the radio."),
     {N_("Download the Hardware API for this computer from sdrplay.com/hardware-api and run the installer."), N_("Restart OnAir (the installer starts SDRplay's background service)."), N_("Press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenUrl, N_("Open the SDRplay API page"), kSdrplay, OsAll}, {Act::OpenDiagnostics, nullptr, "", 0}}},
    {"sdrplay_service", "SDRplay API service", N_("The SDRplay service is not running"),
     N_("OnAir found the SDRplay software, but its background service does not answer, so the RSP cannot be opened."),
     NONE,
     {N_("Install the SDRplay API from sdrplay.com if you have not."), N_("Restart the Mac, or run in Terminal: sudo launchctl kickstart -k system/com.sdrplay.service"), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Open Services (press Win+R, type services.msc)."), N_("Find \"SDRplay API Service\" and press Start (or Restart)."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Run in a terminal: sudo systemctl restart sdrplay"), N_("Press the refresh button next to the radio list."), nullptr},
     {{Act::OpenUrl, N_("Open driver page"), kSdrplay, OsAll}, {Act::CopyText, N_("Copy the restart command"), "sudo systemctl restart sdrplay", OsLinux}, {Act::OpenDiagnostics, nullptr, "", 0}}},
    {"sdrplay_old", "needs SDRplay API", N_("The SDRplay software is too old"),
     N_("OnAir needs SDRplay API 3.07 or a newer 3.x."),
     {N_("Download the current SDRplay API from sdrplay.com and install it."), N_("Restart OnAir."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenUrl, N_("Open driver page"), kSdrplay, OsAll}}},
    {"spectran_missing", "RTSA-Suite PRO is not installed", N_("Install the Aaronia RTSA-Suite PRO"),
     N_("OnAir drives the SPECTRAN V6 and V6 ECO through Aaronia's RTSA API library, which comes with the RTSA-Suite PRO. OnAir cannot include it."),
     NONE,
     {N_("Aaronia offers the RTSA-Suite PRO for Windows and Linux. On a Mac, use the SPECTRAN from a Windows or Linux computer."), nullptr},
     {N_("Download the RTSA-Suite PRO for Windows from aaronia.com/downloads and install it (with its SPECTRAN V6 driver)."), N_("Restart OnAir."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Download the RTSA-Suite PRO for Linux from aaronia.com/downloads and install it (to /opt/aaronia-rtsa-suite)."), N_("Add Aaronia's udev rule as its installer explains, then unplug and replug the SPECTRAN."), N_("Restart OnAir and press the refresh button next to the radio list."), nullptr},
     {{Act::OpenUrl, N_("Open Aaronia downloads"), kAaronia, OsWin | OsLinux}, {Act::OpenUrl, N_("SPECTRAN notes"), kSpectranDoc, OsAll}, {Act::OpenDiagnostics, nullptr, "", 0}}},
    {"kernel_driver", "msi2500", N_("Linux grabbed the SDRplay radio"),
     N_("A Linux driver for TV sticks took the radio before the SDRplay software could."),
     {N_("This is a Linux problem. On this system, unplug the radio, plug it in again and press the refresh button."), nullptr}, NONE, NONE,
     {N_("Run: sudo modprobe -r msi2500 msi001"), N_("Or install OnAir's blacklist file (the .deb package does it)."), N_("Unplug the radio and plug it in again."), nullptr},
     {{Act::CopyText, N_("Copy the command"), "sudo modprobe -r msi2500 msi001", OsLinux}}},
    {"kernel_audio", "snd-usb-audio", N_("Linux treats the radio as a sound card"),
     N_("The Linux sound driver holds the radio, so the SDRplay software cannot see it."),
     {N_("This is a Linux problem. On this system, unplug the radio, plug it in again and press the refresh button."), nullptr}, NONE, NONE,
     {N_("Install OnAir's blacklist file (the .deb package does it), or run: sudo systemctl restart sdrplay"), N_("Unplug the radio and plug it in again."), nullptr},
     {{Act::CopyText, N_("Copy the command"), "sudo systemctl restart sdrplay", OsLinux}}},
    {"rx888_winusb", "vid_04b4&pid_00f1", N_("Set up the RX888 MkII on Windows"),
     N_("OnAir talks to the RX888 MkII over USB with libusb. On Windows the radio needs the WinUSB driver from Zadig for both USB IDs: 04B4:00F3 (boot loader) and 04B4:00F1 (running, \"RX888mk2\"). Use a USB 3 port; official packages include SDDC_FX3.img in the codecs folder."),
     {N_("On macOS and Linux this Windows driver step does not apply; use udev rules on Linux if the radio is not listed."), nullptr},
     NONE,
     {N_("Install WinUSB with Zadig (zadig.akeo.ie): Options → List All Devices."),
      N_("Install WinUSB for 04B4:00F3, then again for 04B4:00F1 (RX888mk2)."),
      N_("Unplug the USB cable, plug into a USB 3 port on the computer, then press refresh in the radio list."),
      nullptr},
     {N_("Install OnAir's udev rules (install-udev-rules.sh), then unplug and replug the radio."), N_("Press the refresh button next to the radio list."), nullptr},
     {{Act::OpenUrl, N_("Open Zadig"), kZadig, OsWin}, {Act::OpenUrl, N_("RX888 notes"), kDevices, OsAll}}},
    {"rx888_firmware", "SDDC_FX3.img", N_("The RX888 needs its firmware file"),
     N_("The RX888 MkII has no program of its own in its USB chip: OnAir loads SDDC_FX3.img into RAM when the radio is in boot-loader mode (nothing is flashed to the radio). Official OnAir packages include this file; a copy built by hand may not."),
     {N_("Download SDDC_FX3.img from the Releases page of the SDDC_Driver project (button below)."), N_("Put the file in OnAir's codecs folder (button below)."), N_("Press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenUrl, N_("Open driver page"), kSddcReleases, OsAll}, {Act::OpenCodecs, N_("Open codecs folder"), "", OsAll}}},
    {"icom_firmware", "needs Icom firmware", N_("The IC-R8600 needs Icom's firmware file"),
     N_("The I/Q OUT port of the IC-R8600 has no program of its own: OnAir loads Icom's firmware into its RAM each time (nothing is flashed to the radio). The file is Icom's, so OnAir cannot include it."),
     {N_("On a Windows PC, install Icom's free \"USB I/Q Package for HDSDR\"; it puts IC-R8600_usb_iq.spt in C:\\Windows\\System32\\IC-R8600\\."), N_("Copy that file to this computer."), N_("Open Radio settings for the radio and choose Import Icom firmware."), N_("Press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"icom_winusb", "0c26:002", N_("Set up the IC-R8600 I/Q port on Windows"),
     N_("OnAir talks to the IC-R8600 I/Q port over USB with libusb. On Windows it needs the WinUSB driver for both USB IDs: 0C26:0022 (before the firmware) and 0C26:0023 (running). Icom's own driver, HDSDR and OnAir cannot share the port."),
     {N_("This is a Windows driver problem. On this system, close any other program that uses the radio, unplug the I/Q USB cable, plug it in again and press the refresh button."), nullptr},
     NONE,
     {N_("Close HDSDR and any other program that uses the radio."), N_("In Zadig (zadig.akeo.ie), List All Devices and install WinUSB for 0C26:0022, then again for 0C26:0023 once it has appeared."), N_("Unplug and replug the I/Q USB cable, then press the refresh button."), nullptr},
     NONE,
     {{Act::OpenUrl, N_("Open Zadig"), kZadig, OsWin}, {Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"rx888_usb2", "on a USB 2 connection", N_("The RX888 is on a USB 2 port"),
     N_("The RX888 MkII sends up to 256 MB per second. It needs a USB 3 (SuperSpeed) port and a USB 3 cable; on a USB 2 port it cannot stream."),
     {N_("Plug the radio into a USB 3 port directly on the computer, not through a USB 2 hub."), N_("Use a USB 3 cable (the one that came with the radio)."), N_("Press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"rx888_cypress", "cypress driver", N_("The RX888 came back on the Cypress driver"),
     N_("OnAir loaded the RX888 firmware, and the radio then reconnects as USB ID 04B4:00F1. The Cypress driver from the official RX-888 setup (the one HDSDR and ExtIO use) has that device. OnAir talks to the radio with libusb instead, and it does not ship a driver."),
     {N_("This is a Windows driver. On this system, unplug the radio, plug it into a USB 3 port and try again."), nullptr},
     NONE,
     {N_("Install the WinUSB driver once with Zadig (zadig.akeo.ie): Options, List All Devices, choose the device whose USB ID is 04B4:00F1, choose the driver WinUSB, then Install."),
      N_("Do the same for 04B4:00F3 if OnAir can no longer see the radio before it starts."),
      N_("Unplug the radio, plug it into a USB 3 port and press the refresh button."), nullptr},
     NONE,
     {{Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"rx888_reenum", "the firmware was loaded", N_("The RX888 did not come back after its firmware loaded"),
     N_("Starting the RX888 loads a program into its USB chip. The radio disconnects (that is the USB sound) and connects again as a new device. OnAir looked for that new device and it did not show up in time."),
     {N_("Unplug the radio, plug it into a USB 3 port on the computer and press the refresh button."), N_("Try once more."), nullptr},
     NONE,
     {N_("In Zadig (zadig.akeo.ie), List All Devices and install the WinUSB driver for USB ID 04B4:00F1. The USB package from the official RX-888 setup hides this device from OnAir, and OnAir does not ship a driver."),
      N_("Unplug the radio, plug it into a USB 3 port and press the refresh button."), nullptr},
     {N_("Install OnAir's udev rules (install-udev-rules.sh), then unplug and replug the radio."), N_("Press the refresh button."), nullptr},
     {{Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}}},
    {"usb_permission", "may not open it", N_("This user may not open the radio"),
     N_("The radio is plugged in, but the system does not let your account use it yet."),
     NONE,
     {N_("Unplug the radio and plug it in again, then press the refresh button."), nullptr},
     {N_("Install the WinUSB driver once with Zadig (zadig.akeo.ie): choose the radio, then the driver WinUSB, then Install."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Install the udev rules: run install-udev-rules.sh from the OnAir folder (the .deb package does it for you)."), N_("Unplug the radio and plug it in again."), N_("Press the refresh button next to the radio list."), nullptr},
     {{Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}, {Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}}},
    {"usb_error", "USB error", N_("The radio's USB connection failed"),
     N_("The radio was found but the USB link to it did not work. This is usually permission, the cable or the port."),
     {N_("Try another USB port, directly on the computer (not a hub), and a short cable."), N_("Close other programs that use the radio."), nullptr},
     NONE,
     {N_("Install the WinUSB driver once with Zadig (zadig.akeo.ie): choose the radio, then the driver WinUSB, then Install."), N_("Try another USB port and a short cable."), nullptr},
     {N_("Install the udev rules (install-udev-rules.sh), then unplug and replug the radio."), N_("Try another USB port, directly on the computer (not a hub)."), nullptr},
     {{Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}, {Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"hackrf_firmware", "USB API version", N_("The HackRF firmware does not match"),
     N_("The firmware inside the HackRF is older than the software talking to it."),
     {N_("Update the HackRF firmware (the page below has the steps; the file comes with the HackRF software)."), N_("Unplug the HackRF and plug it in again."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenUrl, N_("Open firmware page"), kHackrfFw, OsAll}}},
    {"hackrf_busy", "HackRF busy", N_("Another program is using the HackRF"),
     N_("Only one program can use the radio at a time."),
     {N_("Close the other program (a second OnAir, SDR software, hackrf_info)."), N_("Press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"radio_busy", "error_busy", N_("The radio is busy"),
     N_("Another program, or another receiver in OnAir, is using this radio. Only one can use it at a time."),
     {N_("Close the other program (another SDR program, a second OnAir, a command line tool)."), N_("Unplug the radio, plug it in again and press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"radio_busy2", "resource busy", N_("The radio is busy"),
     N_("Another program, or another receiver in OnAir, is using this radio. Only one can use it at a time."),
     {N_("Close the other program (another SDR program, a second OnAir, a command line tool)."), N_("Unplug the radio, plug it in again and press the refresh button next to the radio list."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"radio_access", "error_access", N_("This account may not use the radio"),
     N_("The radio is plugged in, but the system does not let your account open it."),
     NONE,
     {N_("Unplug the radio and plug it in again, then press the refresh button."), N_("If it still fails, close other SDR programs."), nullptr},
     {N_("Install the WinUSB driver once with Zadig (zadig.akeo.ie): choose the radio, then the driver WinUSB, then Install."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Install the udev rules: run install-udev-rules.sh from the OnAir folder (the .deb package does it for you)."), N_("Unplug the radio and plug it in again."), N_("Press the refresh button next to the radio list."), nullptr},
     {{Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}, {Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}}},
    {"radio_driver", "error_not_supported", N_("The radio has the wrong driver"),
     N_("The system gave the radio a driver OnAir cannot use."),
     NONE,
     {N_("Unplug the radio, plug it in again and press the refresh button."), N_("Close other SDR programs that may have taken the radio."), nullptr},
     {N_("Install the WinUSB driver once with Zadig (zadig.akeo.ie): choose the radio, then the driver WinUSB, then Install."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Install the udev rules (install-udev-rules.sh), then unplug and replug the radio."), N_("Press the refresh button next to the radio list."), nullptr},
     {{Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"hackrf_not_found", "HackRF not found", N_("The HackRF is not found"),
     N_("OnAir cannot see a HackRF on the USB ports."),
     {N_("Check the USB cable and port, then press the refresh button."), nullptr},
     NONE,
     {N_("On Windows a HackRF needs its WinUSB driver once: install it with Zadig (zadig.akeo.ie)."), N_("Press the refresh button next to the radio list."), nullptr},
     {N_("Install the udev rules (install-udev-rules.sh), then unplug and replug the radio."), nullptr},
     {{Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}}},
    {"rtltcp_bad_address", "bad address", N_("The rtl_tcp address is not valid"),
     N_("The address of the network radio must look like host:port, for example 192.168.1.20:1234."),
     {N_("Fix the address of the network radio (host:port)."), nullptr}, NONE, NONE, NONE, {}},
    {"rtltcp", "rtl_tcp: ", N_("The network radio (rtl_tcp) does not answer"),
     N_("OnAir could not use the rtl_tcp server, or the connection dropped."),
     {N_("Check that rtl_tcp is running on the other computer and prints \"listening\"."), N_("Check the address and port, and that nothing blocks them (firewall, Wi-Fi)."), N_("Only one program can use an rtl_tcp server at a time: close any other client."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"rtlsdr_missing", "not found (unplugged", N_("The RTL-SDR is gone"),
     N_("The radio you chose is no longer connected."),
     {N_("Plug the radio in again and press the refresh button."), nullptr}, NONE, NONE, NONE, {}},
    {"spyserver_empty", "has no radio connected", N_("The server has no radio"),
     N_("The SpyServer answered, but no radio is plugged into it."),
     {N_("Plug a radio into the computer that runs the SpyServer, and restart the SpyServer."), nullptr}, NONE, NONE, NONE, {}},
    {"usrp_images", "uhd_images_downloader", N_("The USRP needs its firmware images"),
     N_("The USRP software is installed, but the firmware images it loads are missing."),
     {N_("Run once in a terminal: uhd_images_downloader"), N_("Or set the UHD_IMAGES_DIR variable to the folder with the images."), N_("Unplug the USRP, plug it in again and press the refresh button."), nullptr},
     NONE, NONE, NONE,
     {{Act::CopyText, N_("Copy the command"), "uhd_images_downloader", OsAll}, {Act::OpenUrl, N_("Open driver page"), kUhdImages, OsAll}}},
    {"pluto_missing", "AD9361 devices were not found", N_("The PlutoSDR does not show its radio chip"),
     N_("The PlutoSDR answered, but without its radio chip. It may still be starting, or run unusual firmware."),
     {N_("Wait a minute after plugging it in, then press the refresh button."), N_("Check that the firmware is the stock one, or a known one such as Tezuka."), nullptr},
     NONE, NONE, NONE, {{Act::OpenUrl, N_("Open the device list"), kDevices, OsAll}}},
    {"freq_range", "or the signal may be weak or absent", N_("This frequency is outside what this radio is specified for"),
     N_("OnAir still asks the radio for it, but the radio or its driver may not reach this frequency, or the signal may be weak or absent."),
     {N_("Choose a frequency inside the range shown in the message."), N_("Or choose another radio from the list."), nullptr},
     NONE, NONE, NONE, {}},
    {"codec_mbelib", "mbelib not found", N_("The voice codec (mbelib) is not installed"),
     N_("Calls and data are shown, but the voice is silent until you install the codec yourself. OnAir does not ship it (patents)."),
     {N_("Follow the steps in the full instructions to install the codec."), N_("Put the library in the OnAir codecs folder (button below)."), N_("Press \"Look again\" in the ? help next to the sound."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenCodecs, N_("Open codecs folder"), "", OsAll}, {Act::OpenUrl, N_("Full instructions"), kCodecs, OsAll}}},
    {"codec_hdc", "HDC sound codec not installed", N_("The HD Radio sound codec (HDC) is not installed"),
     N_("The station, program data and pictures are shown, but HD Radio sound stays silent until you install the codec yourself. OnAir does not ship it (HDC is patented and owned by Xperi)."),
     {N_("Build the HDC-patched FAAD2 as the full instructions explain (it needs the open-source nrsc5 patch)."), N_("Put the library (libfaad_hdc) in the OnAir codecs folder (button below)."), N_("Press \"Look again\" in the ? help next to the sound."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenCodecs, N_("Open codecs folder"), "", OsAll}, {Act::OpenUrl, N_("Full instructions"), kCodecs, OsAll}}},
    {"codec_hdc_bad", "HDC sound codec cannot be used", N_("The HD Radio sound library cannot be used"),
     N_("OnAir found a library for HDC, but it is not a FAAD2 build with the HDC patch that OnAir knows how to call, so it was left alone."),
     {N_("Build the library again from FAAD2 2.10 or 2.11 with the nrsc5 patch, as the full instructions explain."), N_("Replace the file in the OnAir codecs folder (button below)."), N_("Press \"Look again\" in the ? help next to the sound."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenCodecs, N_("Open codecs folder"), "", OsAll}, {Act::OpenUrl, N_("Full instructions"), kCodecs, OsAll}}},
    {"codec_codec2", "codec2 not found", N_("The FreeDV / M17 voice codec (codec2) was not found"),
     N_("The text and data are shown, but speech needs the codec2 library. It comes with OnAir, so this copy is probably incomplete."),
     {N_("Download OnAir again and unpack or install the whole package: the codec2 library belongs next to the program."), N_("Put the library in the OnAir codecs folder (button below)."), N_("Press \"Look again\" in the ? help next to the sound."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenCodecs, N_("Open codecs folder"), "", OsAll}, {Act::OpenUrl, N_("Full instructions"), kCodecs, OsAll}}},
    {"audio_device", "sound device", N_("No sound output"),
     N_("OnAir could not open the speakers, so there will be no sound."),
     NONE,
     {N_("Open System Settings > Sound and pick an output device."), N_("Restart OnAir."), nullptr},
     {N_("Right-click the speaker icon, open Sound settings and pick an output device."), N_("Close programs that hold the sound device exclusively."), N_("Restart OnAir."), nullptr},
     {N_("Check that PulseAudio or PipeWire runs, and that an output is chosen in the sound settings."), N_("Restart OnAir."), nullptr},
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
    {"file_open", "cannot open ", N_("The file could not be opened"),
     N_("The recording file is missing, moved or not readable."),
     {N_("Check that the file still exists and that you may read it."), N_("Choose the file again."), nullptr}, NONE, NONE, NONE, {}},
    {"no_radio", "no radio", N_("No radio found"),
     N_("OnAir lists a radio when the computer sees it. You can still try everything with the synthetic signal or an IQ file."),
     NONE,
     {N_("Plug the radio in, directly or through a powered hub."), N_("Press the refresh button next to the source."), N_("For an RSP, install the SDRplay API first (sdrplay.com)."), nullptr},
     {N_("Plug the radio in on USB 3 for an RX888 MkII."), N_("Install WinUSB with Zadig (zadig.akeo.ie) for 04B4:00F3 and 04B4:00F1, then unplug and replug."), N_("A HackRF, Airspy or RTL-SDR needs WinUSB the same way."), N_("Press the refresh button next to the source."), nullptr},
     {N_("Plug the radio in, directly or through a powered hub."), N_("Install the udev rules: run install-udev-rules.sh from the OnAir folder (the .deb does it)."), N_("Unplug and replug the radio, then press the refresh button."), nullptr},
     {{Act::OpenUrl, N_("Open driver page"), kZadig, OsWin}, {Act::CopyText, N_("Copy the udev command"), kUdevCmd, OsLinux}, {Act::OpenUrl, N_("Open the device list"), kDevices, OsAll}}},
    {"start_failed", "could not start the radio", N_("The radio did not start"),
     N_("The radio opened badly or stopped at once. The message above says why; these steps solve most cases."),
     {N_("Unplug the radio, plug it in again and press the refresh button."), N_("Close other programs that use the radio."), N_("Try another USB port, directly on the computer."), nullptr},
     NONE, NONE, NONE,
     {{Act::OpenDiagnostics, N_("Copy diagnostics"), "", OsAll}}},
};
constexpr int kN = (int)(sizeof kTable / sizeof *kTable);

std::string lower(const std::string& s) { std::string o = s; for (char& c : o) c = (char)std::tolower((unsigned char)c); return o; }
}

const Entry* table(int& count) { count = kN; return kTable; }

const Entry* byId(const std::string& id) {
    for (const Entry& e : kTable) if (id == e.id) return &e;
    return nullptr;
}

const Entry* lookup(const std::string& message) {
    const std::string m = lower(message);
    for (const Entry& e : kTable) if (m.find(lower(e.pattern)) != std::string::npos) return &e;
    return nullptr;
}

int currentOs() {
#if defined(__APPLE__)
    return OsMac;
#elif defined(_WIN32)
    return OsWin;
#else
    return OsLinux;
#endif
}

std::vector<const char*> stepsFor(const Entry& e, int os) {
    const char* const* own = os == OsMac ? e.mac : os == OsWin ? e.win : e.lin;
    std::vector<const char*> v;
    for (const char* const* p = own[0] ? own : e.any; *p; p++) v.push_back(*p);
    return v;
}

}
