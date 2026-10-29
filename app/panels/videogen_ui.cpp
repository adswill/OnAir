// Tools > Digital video generator: a camera, a still picture or a video file encoded into an MPEG transport stream, written to a .ts
// file or sent over UDP / RTP (port 5000 unless changed). Three tabs: "Sources and encoding" (inputs, output, every encoder parameter and
// the bitrate estimate, which follows each change), "Overlay" (a logo, parameter blocks, free text and rolling text, placed on the
// picture by drag and drop, magnetic) and "Digital Television Mode Selector" (the DVB modes that carry the stream on a link,
// videogen_modes_ui.cpp). A standalone tool: it does not touch the receiver or the transmitter; a .ts file it writes can be given to the
// transmitter as its "MPEG-TS / video file". The engine is core/src/tv/videogen*.cpp.
#include "../app.h"
#include "../platform/platform.h"
#include "videogen_modes_ui.h"
#include "dect2/audio/audioin.h"
#include "dect2/audio/videoin.h"
#include "dect2/common/u8file.h"
#include "dect2/tv/videogen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace dect2::vgen;

namespace {

struct Res { int w, h; };
const Res kRes[] = {{320, 240}, {352, 288}, {640, 360}, {704, 576}, {720, 576}, {1280, 720}, {1920, 1080}};
const double kFps[] = {1, 2, 5, 10, 12.5, 15, 20, 25, 30};
const int kAudioKbps[] = {64, 96, 128, 160, 192, 256};
const int kSampleRates[] = {32000, 44100, 48000};
const char* const kSourceNames[] = {N_("Camera"), N_("Still picture"), N_("Video file"), N_("Test generator")};
const char* const kPatternNames[] = {N_("Test card"), N_("Colour bars"), N_("Grey ramp")};
const char* const kAudioNames[] = {N_("Silence"), N_("Test tone (1 kHz)"), N_("Sound of the video file"), N_("Microphone")};
const char* const kOutputNames[] = {N_("MPEG-TS file (.ts)"), N_("UDP stream"), N_("RTP stream")};
const char* const kFitNames[] = {N_("Stretch"), N_("Letterbox"), N_("Crop")};
const char* const kContentNames[] = {N_("Video only"), N_("Video + audio"), N_("Video + multiple audio tracks")};
const char* const kRcNames[] = {N_("CBR"), N_("Constrained VBR"), N_("VBR / CRF")};
const char* const kFrameNames[] = {N_("I/P/B"), N_("I/P"), N_("I-only")};
const char* const kProfileNames[] = {N_("Baseline"), N_("Main"), N_("High")};
const char* const kDirNames[] = {N_("Left to right"), N_("Right to left")};

// the parameters an overlay block can show, grouped as on the first tab ("where they come from")
struct ParamGroup { const char* name; std::vector<Param> params; };
const ParamGroup kGroups[] = {
    {N_("Video"), {Param::VideoCodec, Param::VideoBitrate, Param::Resolution, Param::FrameRate, Param::Gop, Param::FrameTypes, Param::Profile,
                   Param::RateControl, Param::PixelFormat, Param::Preset, Param::Latency}},
    {N_("Audio"), {Param::AudioCodec, Param::AudioBitrate, Param::SampleRate, Param::Channels}},
    {N_("Transport stream"), {Param::TsContent, Param::MuxRate, Param::TotalRate, Param::Service}},
};

// The "(?)" text of each settings row, by its English label (docs/VIDEO-GENERATOR.md says the same at more length)
struct RowHelp { const char* label; const char* help; };
const RowHelp kRowHelp[] = {
    {N_("Input"), N_("What is encoded. Camera: a webcam or an HDMI / SDI capture card, opened only when you press Start. Still picture: one image sent as a continuous stream (a test card, a slide). Video file: any file FFmpeg can read, played from the start and looped when the length is longer. Test generator: a copy of the transmitter's own moving test picture (the test transport stream the DVB-T2 T2Generator carries in its real PLP). Put a callsign or other text on it with the Overlay tab.")},
    {N_("Camera"), N_("The cameras and capture cards found when this window opened. Press Refresh after plugging one in.")},
    {N_("Picture"), N_("A PNG, JPEG, BMP or other picture. It is scaled to the resolution below with the picture fit you choose.")},
    {N_("Video file"), N_("The length below follows the file when you choose it. Picture and sound are decoded in step, so a file is encoded faster than real time when the output is a .ts file.")},
    {N_("Pattern"), N_("The test generator's picture. Test card: colour bars, a moving sweep and a chequerboard with a bouncing ball, so motion and lost packets are easy to see. Colour bars and Grey ramp: still pictures to check colours and levels. A white bar grows along the bottom once a second.")},
    {N_("Picture fit"), N_("When the source has another shape than the output (a 4:3 camera in a 16:9 picture). Stretch: fill the picture, shapes look wider or narrower. Letterbox: keep the whole source, with black bars. Crop: fill the picture and cut the edges.")},
    {N_("Sound"), N_("What the audio tracks carry. The test tone (1 kHz at -20 dBFS) is handy to check a receiver's sound. The sound of the video file is only there with a video file as input. Silence still makes a valid audio track.")},
    {N_("Microphone"), N_("The sound input to use. It is opened only when you press Start.")},
    {N_("Send to"), N_("MPEG-TS file (.ts): a transport stream file; give it to the transmitter as its \"MPEG-TS / video file\", it is sent as it is. UDP or RTP stream: sent over the network in real time, to watch in VLC or feed another program. RTP adds a small header with a sequence number, so a player can see lost packets.")},
    {N_("File"), N_("Where the .ts file is written. An existing file is replaced.")},
    {N_("Address"), N_("The PC that receives the stream (unicast, e.g. 192.168.1.20) or a multicast group (239.0.0.0 to 239.255.255.255) that any PC on the network can join. Port 5000 unless you change it. 127.0.0.1 is this PC.")},
    {N_("TTL"), N_("Multicast only: how many routers the stream may cross. 1 keeps it on this network.")},
    {N_("Length"), N_("How long the .ts file is. One minute for a still picture unless you change it; the file's own length for a video. A stream has no length: it runs until you press Stop.")},
    {N_("TS content"), N_("Which streams go into the transport stream. Audio uses part of the capacity. Multiple audio tracks carry the same sound on their own PIDs, to test receivers that offer a language choice.")},
    {N_("Audio tracks"), N_("How many audio tracks, each at the audio bitrate below.")},
    {N_("Video codec"), N_("H.264 is widely supported and the safe choice for DVB-T. H.265 needs about 30 % less bitrate, but only newer receivers (DVB-T2 sets) decode it: check your whole chain. MPEG-2 works everywhere but needs about twice the bitrate of H.264. Only the encoders that work on this computer are listed; the bundled FFmpeg has MPEG-2 only.")},
    {N_("Rate control"), N_("CBR: a constant rate, predictable, what a broadcast link needs. Constrained VBR: the rate varies with the scene up to 1.5 times the bitrate, so the mux rate needs headroom for the peaks. VBR / CRF: a constant quality instead of a rate (x264 and x265 only), capped at twice the bitrate.")},
    {N_("Video bitrate"), N_("More bitrate generally gives a better picture but uses more of the channel. Auto follows a rule of thumb for this codec, resolution, frame rate and frame types; the estimate on the right shows what the whole stream then needs. Untick Auto to set your own. MPEG-2 has a lowest bitrate for each picture size (about 160 bit/s per 16x16 block, e.g. 150 kbit/s at 640x360): a picture must reach the receiver within about 0.7 s.")},
    {N_("CRF"), N_("The quality level: lower is better and costs more bits. 18 is close to transparent, 23 is the x264 default, 28 is clearly softer.")},
    {N_("Frame rate"), N_("Pictures per second. Lower rates need less data for motion but look less smooth. Some equipment takes only certain rates: 25 fps in 50 Hz countries, 30 fps in 60 Hz countries.")},
    {N_("Resolution"), N_("The size of the encoded picture. Lower resolution needs less bitrate and less encoder time. Use a size your receiver supports: 720x576 is standard definition in Europe, 1280x720 and 1920x1080 are HD.")},
    {N_("Frame types"), N_("I/P/B: the best compression (B-frames predict from both sides). I/P: no B-frames, a little lower delay. I-only: every picture stands alone; possible, but it needs much more bitrate for the same quality.")},
    {N_("GOP"), N_("The time between two keyframes (I-frames). A receiver can start or recover only on a keyframe, so a short GOP helps after a dropout and when switching channel; a long GOP compresses better. 1 to 2 s is common for broadcast.")},
    {N_("Profile"), N_("H.264 only. Baseline: simple decoders, no B-frames. Main: the broadcast default, every receiver decodes it. High: better compression, for newer receivers.")},
    {N_("Pixel format"), N_("4:2:0 colour at 8 bits per sample, the common choice that every receiver decodes. Other formats would need the whole chain to support them.")},
    {N_("Encoder preset"), N_("x264 / x265 only. Slower presets compress better (a better picture at the same bitrate) but need more CPU. For a live camera or stream, choose the slowest preset that still keeps up; a warning shows frames that come late.")},
    {N_("Latency tuning"), N_("x264 / x265 only. zerolatency removes the encoder's look-ahead and B-frames, so a live picture arrives sooner, at some cost in compression.")},
    {N_("Audio codec"), N_("Choose one your receiver decodes. MPEG-1 Layer II is accepted by nearly every DVB receiver; AAC sounds better at the same bitrate; AC-3 is common on HD channels. Only the encoders of this FFmpeg are listed.")},
    {N_("Audio bitrate"), N_("Per track. 64 to 96 kbit/s is fine for speech in mono, 128 to 192 kbit/s for music in stereo (MPEG-1 Layer II needs the higher end).")},
    {N_("Sample rate"), N_("48 kHz is the broadcast standard; use 32 or 44.1 kHz only if your receiver needs it.")},
    {N_("Channels"), N_("Mono saves some bitrate; stereo needs more.")},
    {N_("TS mux rate"), N_("The total rate of the transport stream: video, audio, tables and null packets that pad it to a constant rate. Keep it at or below the net bitrate of the DVB link, e.g. DVB-T 8 MHz, 64-QAM, code rate 2/3, guard interval 1/4 carries about 19.9 Mbit/s; QPSK 1/2 about 5 Mbit/s. Auto uses the estimate on the right plus a small margin.")},
    {N_("Service ID"), N_("The programme number in the PAT and PMT, and the service ID in the SDT. Receivers use it to find the programme; 1 is fine unless you test something specific.")},
    {N_("Service name"), N_("The channel name a receiver shows in its list (from the SDT).")},
    {N_("Provider"), N_("The provider name in the SDT; some receivers show it with the channel.")},
    {N_("PIDs"), N_("The packet IDs of the PMT, the video and the audio. Automatic gives one programme with the usual PIDs (PMT 0x1000, video 0x100, audio 0x101). Set them yourself to test a receiver's handling of specific PIDs.")},
    {N_("PMT PID"), N_("Where the programme map table is sent (0x20 to 0x1FFE).")},
    {N_("Video PID"), N_("The packet ID of the video stream.")},
    {N_("Audio PID"), N_("The packet ID of the first audio track; further tracks follow it.")},
};

const char* const kToolHelp = N_("Encodes a camera, a still picture or a video file, with the overlay of the second tab, into an MPEG transport stream: a .ts file, or a UDP / RTP stream to watch in VLC. A .ts file can be given to the transmitter as its \"MPEG-TS / video file\". The estimate on the right of the first tab shows the bitrate the settings need, so you can fit the stream to your DVB link before encoding; the tab Digital Television Mode Selector shows which DVB modes carry it in your bandwidth at your C/N. Each setting has its own (?). The whole guide: docs/VIDEO-GENERATOR.md.");

struct DropItem { int kind; int param; };   // the drag-and-drop payload "VGBLOCK"

struct State {
    bool init = false;
    GenConfig cfg;
    bool lengthEdited = false;
    std::future<void> probe;   // encoders, cameras and microphones, looked up when the window first opens
    bool probed = false;
    std::vector<dect2::VideoInputDevice> cams, camsFound;
    std::vector<dect2::AudioInputDevice> mics, micsFound;
    VideoGenerator gen;
    std::string msg;
    std::string savedCfg, savedLayout, sentLayout;
    double lastSave = 0;
    double minRateNoticeUntil = 0;   // the video bitrate was raised to the MPEG-2 minimum: explain why until then
    // overlay editor
    int sel = -1;
    bool dragging = false, resizing = false, snap = true;
    ImVec2 grab{0, 0};
    OverlayRenderer ovr;
    std::shared_ptr<const std::vector<uint8_t>> font;
    gfx::Image* ovImg = nullptr;
    int ovW = 0, ovH = 0;
    std::string ovKey;
    double ovAt = 0;
    std::vector<uint8_t> ovRgba;
    gfx::Image* prevImg = nullptr;
    int prevW = 0, prevH = 0;
    uint64_t prevSerial = 0;
    std::vector<uint8_t> prevRgba;
    double startedAt = 0;
};

State& S() {
    static State s;
    return s;
}

std::string cfgPath() { return plat::dataDir() + "/videogen.txt"; }
std::string layoutPath() { return plat::dataDir() + "/videogen_overlay.txt"; }

std::string readText(const std::string& p) {
    std::string t;
    if (FILE* f = dect2::u8fopen(p, "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n);
        fclose(f);
    }
    return t;
}

void writeText(const std::string& p, const std::string& t) {
    if (FILE* f = dect2::u8fopen(p, "wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
}

// the sources and output, below the encoder settings in the same file ("g." keys, which loadSettings skips)
std::string saveCfg(const GenConfig& c) {
    char b[64];
    std::string o = saveSettings(c.enc);
    o += "g.src=" + std::to_string((int)c.source) + "\n" + "g.cam=" + c.cameraId + "\n" + "g.path=" + c.path + "\n";
    o += "g.pat=" + std::to_string(c.pattern) + "\n";
    o += "g.audio=" + std::to_string((int)c.audio) + "\n" + "g.mic=" + c.micId + "\n" + "g.out=" + std::to_string((int)c.output) + "\n";
    o += "g.ts=" + c.tsPath + "\n" + "g.host=" + c.host + "\n" + "g.port=" + std::to_string(c.port) + "\n" + "g.ttl=" + std::to_string(c.ttl) + "\n";
    snprintf(b, sizeof b, "%.3f", c.seconds);
    o += std::string("g.secs=") + b + "\n" + "g.fit=" + std::to_string((int)c.fit) + "\n";
    return o;
}

void loadCfg(const std::string& text, GenConfig& c) {
    loadSettings(text, c.enc);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (line.compare(0, 2, "g.") != 0 || eq == std::string::npos) continue;
        const std::string k = line.substr(2, eq - 2), v = line.substr(eq + 1);
        const int i = atoi(v.c_str());
        if (k == "src") c.source = (Source)std::clamp(i, 0, 3);
        else if (k == "pat") c.pattern = std::clamp(i, 0, 2);
        else if (k == "cam") c.cameraId = v;
        else if (k == "path") c.path = v;
        else if (k == "audio") c.audio = (AudioSource)std::clamp(i, 0, 3);
        else if (k == "mic") c.micId = v;
        else if (k == "out") c.output = (Output)std::clamp(i, 0, 2);
        else if (k == "ts" && !v.empty()) c.tsPath = v;
        else if (k == "host" && !v.empty()) c.host = v;
        else if (k == "port") c.port = std::clamp(i, 1, 65535);
        else if (k == "ttl") c.ttl = std::clamp(i, 1, 255);
        else if (k == "secs") c.seconds = std::clamp(atof(v.c_str()), 1.0, 86400.0);
        else if (k == "fit") c.fit = (Fit)std::clamp(i, 0, 2);
    }
}

// The interface's own TrueType font (Inter, or the monospaced font of the classic look) for the overlay text: the biggest base font the
// atlas was built from. The CJK fonts merged into them are left out: they are bigger, and a fallback such as Droid Sans Fallback has no
// Latin letters, so every character came out as an empty box.
std::shared_ptr<const std::vector<uint8_t>> atlasFont() {
    const ImFontAtlas* at = ImGui::GetIO().Fonts;
    const ImFontConfig* best = nullptr;
    for (const ImFontConfig& c : at->Sources)
        if (!c.MergeMode && c.FontData && c.FontDataSize > 1000 && (!best || c.FontDataSize > best->FontDataSize)) best = &c;
    if (!best) return nullptr;
    const uint8_t* d = (const uint8_t*)best->FontData;
    return std::make_shared<std::vector<uint8_t>>(d, d + best->FontDataSize);
}

bool inputText(const char* label, std::string& s, float width) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputText(label, buf, sizeof buf)) return false;
    s = buf;
    return true;
}

// A dimmed "(?)" after the last item; hovering it shows the text, wrapped (the same look as Propagation's helpMark)
void helpMark(const char* text) {
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("(?)");
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380 * gUi);
    ImGui::TextUnformatted(tr(text));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// a label in the left column of a settings row (English, translated here), with its "(?)" when kRowHelp has a text for it
void rowLabel(const char* english, float col) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(english));
    for (const RowHelp& h : kRowHelp)
        if (strcmp(h.label, english) == 0) { helpMark(h.help); break; }
    ImGui::SameLine(col);
}

bool combo(const char* id, int& v, const char* const* names, int n, float width, const std::vector<bool>* enabled = nullptr, const char* offTip = nullptr) {
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, tr(names[std::clamp(v, 0, n - 1)]))) {
        for (int i = 0; i < n; i++) {
            const bool on = !enabled || (*enabled)[(size_t)i];
            ImGui::BeginDisabled(!on);
            if (ImGui::Selectable(tr(names[i]), v == i)) { v = i; changed = true; }
            ImGui::EndDisabled();
            if (!on && offTip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(offTip));
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string kbpsText(double v) {
    char b[48];
    if (v >= 10000) snprintf(b, sizeof b, "%.2f Mbit/s", v / 1000.0);
    else snprintf(b, sizeof b, "%.0f kbit/s", v);
    return b;
}

void startProbe(State& s) {
    s.probed = false;
    s.probe = std::async(std::launch::async, [&s] {
        videoEncoders();
        audioEncoders();
        s.camsFound = dect2::listVideoInputs();   // lists only: no camera is opened until Start
        s.micsFound = dect2::listAudioInputs();
    });
}

void loadOnce(State& s) {
    if (s.init) return;
    s.init = true;
    s.cfg.tsPath = plat::dataDir() + "/onair_video.ts";
    s.cfg.port = 5000;
    const std::string c = readText(cfgPath());
    if (!c.empty()) { loadCfg(c, s.cfg); s.lengthEdited = true; }
    const std::string l = readText(layoutPath());
    if (!l.empty()) s.cfg.overlay = loadLayout(l);
    s.savedCfg = saveCfg(s.cfg);
    s.savedLayout = saveLayout(s.cfg.overlay);
    s.font = atlasFont();
    s.ovr.setFont(s.font);
    startProbe(s);
}

void saveSoon(State& s) {
    const double now = ImGui::GetTime();
    if (now - s.lastSave < 1.0) return;
    s.lastSave = now;
    const std::string c = saveCfg(s.cfg), l = saveLayout(s.cfg.overlay);
    if (c != s.savedCfg) { writeText(cfgPath(), c); s.savedCfg = c; }
    if (l != s.savedLayout) { writeText(layoutPath(), l); s.savedLayout = l; }
}

// ---- the first tab: sources, output, encoding ----

void sourceSection(State& s, float col, float w) {
    GenConfig& c = s.cfg;
    ImGui::SeparatorText(tr("Source"));
    rowLabel(N_("Input"), col);
    int src = (int)c.source;
    if (combo("##vgsrc", src, kSourceNames, 4, w)) {
        c.source = (Source)src;
        if ((c.source == Source::Picture || c.source == Source::Generator) && !s.lengthEdited) c.seconds = 60;   // one minute unless changed
        if (c.source != Source::VideoFile && c.audio == AudioSource::FromFile) c.audio = AudioSource::Tone;
    }
    if (c.source == Source::Camera) {
        rowLabel(N_("Camera"), col);
        std::string cur = tr("Default camera");
        for (const auto& d : s.cams) if (d.id == c.cameraId) cur = d.name;
        ImGui::SetNextItemWidth(w - 90 * gUi);
        if (ImGui::BeginCombo("##vgcam", cur.c_str())) {
            if (ImGui::Selectable(tr("Default camera"), c.cameraId.empty())) c.cameraId.clear();
            for (const auto& d : s.cams) if (!d.id.empty() && ImGui::Selectable(d.name.c_str(), d.id == c.cameraId)) c.cameraId = d.id;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.probed);
        if (ImGui::Button(tr("Refresh##vgcam"))) startProbe(s);
        ImGui::EndDisabled();
        if (s.probed && s.cams.empty()) ImGui::TextDisabled("%s", tr("No camera found. Connect one and press Refresh."));
    } else if (c.source == Source::Generator) {
        rowLabel(N_("Pattern"), col);
        combo("##vgpat", c.pattern, kPatternNames, 3, w);
    } else {
        rowLabel(c.source == Source::Picture ? N_("Picture") : N_("Video file"), col);
        if (inputText("##vgpath", c.path, w - 90 * gUi) && c.source == Source::VideoFile) {
            const double d = mediaSeconds(c.path);
            if (d > 0) { c.seconds = d; s.lengthEdited = false; }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##vgpath"))) {
            const std::string p = plat::openFileDialog();
            if (!p.empty()) {
                c.path = p;
                if (c.source == Source::VideoFile) {
                    const double d = mediaSeconds(p);   // the length follows the video
                    if (d > 0) { c.seconds = d; s.lengthEdited = false; }
                }
            }
        }
    }
    rowLabel(N_("Picture fit"), col);
    int fit = (int)c.fit;
    ImGui::BeginDisabled(c.source == Source::Generator);   // drawn at the output size
    if (combo("##vgfit", fit, kFitNames, 3, w)) c.fit = (Fit)fit;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When the source has another shape than the output (a 4:3 camera in 16:9): stretch it, keep it whole with black bars, or fill the picture and cut the edges"));
    rowLabel(N_("Sound"), col);
    int au = (int)c.audio;
    std::vector<bool> en = {true, true, c.source == Source::VideoFile, true};
    if (combo("##vgaudio", au, kAudioNames, 4, w, &en, N_("Only with a video file as input"))) c.audio = (AudioSource)au;
    if (c.audio == AudioSource::Microphone) {
        rowLabel(N_("Microphone"), col);
        std::string cur = tr("Default input");
        for (const auto& d : s.mics) if (d.id == c.micId) cur = d.name;
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##vgmic", cur.c_str())) {
            if (ImGui::Selectable(tr("Default input"), c.micId.empty())) c.micId.clear();
            for (const auto& d : s.mics) if (!d.id.empty() && ImGui::Selectable(d.name.c_str(), d.id == c.micId)) c.micId = d.id;
            ImGui::EndCombo();
        }
    }
}

void outputSection(State& s, float col, float w) {
    GenConfig& c = s.cfg;
    ImGui::SeparatorText(tr("Output"));
    rowLabel(N_("Send to"), col);
    int out = (int)c.output;
    if (combo("##vgout", out, kOutputNames, 3, w)) c.output = (Output)out;
    if (c.output == Output::File) {
        rowLabel(N_("File"), col);
        inputText("##vgts", c.tsPath, w - 90 * gUi);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##vgts"))) {
            const std::string p = plat::saveFileDialog("onair_video.ts");
            if (!p.empty()) c.tsPath = p;
        }
    } else {
        rowLabel(N_("Address"), col);
        inputText("##vghost", c.host, w * 0.6f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The receiving PC (unicast) or a multicast group such as 239.1.1.1"));
        ImGui::SameLine();
        ImGui::TextUnformatted(tr("Port"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60 * gUi, w * 0.4f - ImGui::CalcTextSize(tr("Port")).x - ImGui::GetStyle().ItemSpacing.x * 2));
        if (ImGui::InputInt("##vgport", &c.port, 0)) c.port = std::clamp(c.port, 1, 65535);
        if (isMulticast(c.host)) {
            rowLabel(N_("TTL"), col);
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::InputInt("##vgttl", &c.ttl)) c.ttl = std::clamp(c.ttl, 1, 255);
        }
    }
    rowLabel(N_("Length"), col);
    const bool stream = c.output != Output::File;
    ImGui::BeginDisabled(stream);   // a stream goes on until it is stopped
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::InputDouble("##vglen", &c.seconds, 10, 60, "%.1f s")) { c.seconds = std::clamp(c.seconds, 1.0, 86400.0); s.lengthEdited = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (stream) ImGui::TextDisabled("%s", tr("continuous while streaming"));
    else {
        const int t = (int)std::lround(c.seconds);
        ImGui::TextDisabled("%d:%02d", t / 60, t % 60);
    }
}

void encodingSection(State& s, float col, float w) {
    Settings& e = s.cfg.enc;
    const auto& venc = videoEncoders();
    const auto& aenc = audioEncoders();
    const EncoderCaps caps = capsOf(e.videoEncoder);

    ImGui::SeparatorText(tr("Video"));
    rowLabel(N_("TS content"), col);
    int content = (int)e.content;
    if (combo("##vgcontent", content, kContentNames, 3, w)) e.content = (TsContent)content;
    if (e.content == TsContent::VideoMultiAudio) {
        rowLabel(N_("Audio tracks"), col);
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::SliderInt("##vgtracks", &e.audioTracks, 2, 4);
    }
    rowLabel(N_("Video codec"), col);
    std::string cur = e.videoEncoder;
    for (const EncoderInfo& i : venc) if (i.name == e.videoEncoder) cur = i.label;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgvenc", cur.c_str())) {
        for (const EncoderInfo& i : venc) {
            if (ImGui::Selectable(i.label.c_str(), i.name == e.videoEncoder)) e.videoEncoder = i.name;
            if (i.hardware && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hardware encoder: preset, tuning and CRF do not apply"));
        }
        // the codecs this FFmpeg cannot make, shown so it is clear why they are missing
        for (Codec cd : {Codec::H264, Codec::Hevc}) {
            if (std::any_of(venc.begin(), venc.end(), [cd](const EncoderInfo& i) { return i.codec == cd; })) continue;
            ImGui::BeginDisabled();
            ImGui::Selectable((std::string(codecName(cd)) + " - " + tr("not in this FFmpeg")).c_str(), false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("The FFmpeg OnAir is built with has no encoder for this codec that works on this computer (the bundled LGPL build has MPEG-2 only; a system FFmpeg with libx264 / libx265 or a hardware encoder adds them)"));
        }
        ImGui::EndCombo();
    }
    rowLabel(N_("Rate control"), col);
    int rc = (int)e.rc;
    std::vector<bool> rcOn = {true, true, caps.crf};
    if (combo("##vgrc", rc, kRcNames, 3, w, &rcOn, N_("CRF needs x264 or x265"))) e.rc = (RateControl)rc;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("CBR gives a predictable rate; VBR varies with the scene and needs headroom for the peaks"));
    rowLabel(N_("Video bitrate"), col);
    const int sug = suggestVideoKbps(e);
    if (e.autoBitrate) e.videoKbps = sug;
    ImGui::BeginDisabled(e.autoBitrate);
    ImGui::SetNextItemWidth(130 * gUi);
    const int minKbps = minVideoKbps(e);
    if (ImGui::InputInt("##vgvkbps", &e.videoKbps, 50, 500)) e.videoKbps = std::clamp(e.videoKbps, 10, 100000);
    ImGui::EndDisabled();
    // asked below what MPEG-2 can send at this picture size (or a larger picture was chosen): raise it and say why
    if (e.videoKbps < minKbps) {
        e.videoKbps = minKbps;
        s.minRateNoticeUntil = ImGui::GetTime() + 10;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(tr("kbit/s"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Auto##vgvkbps"), &e.autoBitrate);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%d kbit/s)", tr("Follow the suggestion for this codec, resolution, frame rate and frame types"), sug);
    if (ImGui::GetTime() < s.minRateNoticeUntil) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.2f, 1));
        ImGui::TextWrapped("%s %dx%d: %d kbit/s.", tr("The lowest video bitrate for MPEG-2 at"), e.width, e.height, minKbps);
        ImGui::TextWrapped("%s", tr("MPEG-2 tells the receiver with each picture how long to wait for it, at most about 0.7 s. Even at the lowest quality, a picture of this size needs about 160 bit/s per 16x16 block to arrive in time; below that the encoder cannot make a valid stream. For a lower bitrate, choose a smaller picture, or H.264 / H.265 if this FFmpeg has them."));
        ImGui::PopStyleColor();
    }
    if (e.rc == RateControl::Crf && caps.crf) {
        rowLabel(N_("CRF"), col);
        ImGui::SetNextItemWidth(w);
        ImGui::SliderInt("##vgcrf", &e.crf, 15, 40);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lower is better quality and more bits. The rate is capped at twice the bitrate above."));
    }
    rowLabel(N_("Frame rate"), col);
    char fl[32];
    snprintf(fl, sizeof fl, "%.*f fps", e.fps == std::floor(e.fps) ? 0 : 1, e.fps);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgfps", fl)) {
        for (double f : kFps) {
            snprintf(fl, sizeof fl, "%.*f fps", f == std::floor(f) ? 0 : 1, f);
            if (ImGui::Selectable(fl, e.fps == f)) e.fps = f;
        }
        ImGui::EndCombo();
    }
    rowLabel(N_("Resolution"), col);
    char rl[32];
    snprintf(rl, sizeof rl, "%dx%d", e.width, e.height);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgres", rl)) {
        for (const Res& r : kRes) {
            snprintf(rl, sizeof rl, "%dx%d", r.w, r.h);
            if (ImGui::Selectable(rl, e.width == r.w && e.height == r.h)) { e.width = r.w; e.height = r.h; }
        }
        ImGui::EndCombo();
    }
    rowLabel(N_("Frame types"), col);
    int ft = (int)e.frames;
    std::vector<bool> ftOn = {caps.bFrames, true, true};
    if (combo("##vgft", ft, kFrameNames, 3, w, &ftOn, N_("This encoder makes no B-frames"))) e.frames = (FrameTypes)ft;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("I-only is possible but needs much more bitrate for the same quality"));
    rowLabel(N_("GOP"), col);
    ImGui::BeginDisabled(e.frames == FrameTypes::IOnly);
    ImGui::SetNextItemWidth(w * 0.6f);
    float gop = (float)e.gopSeconds;
    if (ImGui::SliderFloat("##vggop", &gop, 0.5f, 5.f, "%.1f s")) e.gopSeconds = std::round(gop * 10) / 10;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (e.frames == FrameTypes::IOnly) ImGui::TextDisabled("%s", tr("all-intra"));
    else ImGui::TextDisabled("%d %s", std::max(1, (int)std::lround(e.gopSeconds * e.fps)), tr("frames"));
    rowLabel(N_("Profile"), col);
    ImGui::BeginDisabled(!caps.profile);
    combo("##vgprofile", e.profile, kProfileNames, kNumProfiles, w);
    ImGui::EndDisabled();
    if (!caps.profile && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("H.264 profiles only"));
    rowLabel(N_("Pixel format"), col);
    ImGui::TextDisabled("%s", tr("yuv420p, 8-bit (the common choice every receiver takes)"));
    rowLabel(N_("Encoder preset"), col);
    ImGui::BeginDisabled(!caps.preset);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgpreset", kPresets[std::clamp(e.preset, 0, kNumPresets - 1)])) {
        for (int i = 0; i < kNumPresets; i++) if (ImGui::Selectable(kPresets[i], e.preset == i)) e.preset = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", caps.preset ? tr("Slower presets compress better but need more time") : tr("x264 and x265 only"));
    rowLabel(N_("Latency tuning"), col);
    ImGui::BeginDisabled(!caps.zeroLatency);
    ImGui::Checkbox(tr("zerolatency"), &e.zeroLatency);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", caps.zeroLatency ? tr("Less encoder buffering for live use, at some cost in compression (no B-frames)") : tr("x264 and x265 only"));

    const bool audio = e.content != TsContent::VideoOnly;
    ImGui::SeparatorText(tr("Audio"));
    ImGui::BeginDisabled(!audio);
    rowLabel(N_("Audio codec"), col);
    std::string acur = tr("none available");
    for (const EncoderInfo& i : aenc) if (i.name == e.audioEncoder) acur = i.label;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgaenc", acur.c_str())) {
        for (const EncoderInfo& i : aenc) if (ImGui::Selectable(i.label.c_str(), i.name == e.audioEncoder)) e.audioEncoder = i.name;
        ImGui::EndCombo();
    }
    rowLabel(N_("Audio bitrate"), col);
    char al[32];
    snprintf(al, sizeof al, "%d kbit/s", e.audioKbps);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgakbps", al)) {
        for (int k : kAudioKbps) { snprintf(al, sizeof al, "%d kbit/s", k); if (ImGui::Selectable(al, e.audioKbps == k)) e.audioKbps = k; }
        ImGui::EndCombo();
    }
    rowLabel(N_("Sample rate"), col);
    snprintf(al, sizeof al, "%.1f kHz", e.sampleRate / 1000.0);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vgarate", al)) {
        for (int r : kSampleRates) { snprintf(al, sizeof al, "%.1f kHz", r / 1000.0); if (ImGui::Selectable(al, e.sampleRate == r)) e.sampleRate = r; }
        ImGui::EndCombo();
    }
    rowLabel(N_("Channels"), col);
    int ch = e.channels - 1;
    const char* const chNames[] = {N_("Mono"), N_("Stereo")};
    if (combo("##vgach", ch, chNames, 2, w)) e.channels = ch + 1;
    ImGui::EndDisabled();

    ImGui::SeparatorText(tr("Transport stream"));
    const Budget b = estimate(e);
    rowLabel(N_("TS mux rate"), col);
    if (e.autoMux) e.muxKbps = b.suggestedMux;
    ImGui::BeginDisabled(e.autoMux);
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::InputInt("##vgmux", &e.muxKbps, 50, 500)) e.muxKbps = std::clamp(e.muxKbps, 100, 200000);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(tr("kbit/s"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Auto##vgmux"), &e.autoMux);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The total TS rate with signalling and null packets. Set it to what the DVB link carries, or let it follow the estimate."));
    rowLabel(N_("Service ID"), col);
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::InputInt("##vgsid", &e.serviceId)) e.serviceId = std::clamp(e.serviceId, 1, 65535);
    rowLabel(N_("Service name"), col);
    inputText("##vgsname", e.serviceName, w);
    rowLabel(N_("Provider"), col);
    inputText("##vgsprov", e.provider, w);
    rowLabel(N_("PIDs"), col);
    ImGui::Checkbox(tr("Automatic##vgpid"), &e.autoPids);
    if (!e.autoPids) {
        auto pid = [&](const char* label, const char* id, int& v, int hi) {
            rowLabel(label, col);
            ImGui::SetNextItemWidth(130 * gUi);
            if (ImGui::InputInt(id, &v, 1, 16, ImGuiInputTextFlags_CharsHexadecimal)) v = std::clamp(v, 0x20, hi);
            ImGui::SameLine();
            ImGui::TextDisabled("0x%04X", v);
        };
        pid(N_("PMT PID"), "##vgpmt", e.pmtPid, 0x1FFE);
        pid(N_("Video PID"), "##vgvpid", e.videoPid, 0x1FFE);
        pid(N_("Audio PID"), "##vgapid", e.audioPid, 0x1FFA);
        if (audioTrackCount(e) > 1) ImGui::TextDisabled("%s (0x%04X)", tr("Audio track n uses the audio PID + n, up to"), e.audioPid + audioTrackCount(e) - 1);
    }
}

// The estimate beside the settings: what each part takes of the mux rate, updated on every change
void estimatePanel(const Settings& e) {
    const Budget b = estimate(e);
    ImGui::SeparatorText(tr("Estimated bitrate"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("Indicative: a rule of thumb per codec, not a measurement"));
    ImGui::PopStyleColor();
    if (ImGui::BeginTable("##vgest", 2, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* k, double v, bool strong = false) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k);
            ImGui::TableNextColumn();
            const std::string t = kbpsText(v);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(t.c_str()).x);
            if (strong) ImGui::TextUnformatted(t.c_str());
            else ImGui::TextDisabled("%s", t.c_str());
        };
        row(tr("Video"), b.video, true);
        if (b.videoPeak > b.video) row(tr("Video peak"), b.videoPeak);
        row(tr("Audio"), b.audio, true);
        row(tr("PES and TS headers, PCR"), b.pesTs);
        row(tr("PAT, PMT, SDT"), b.psiSi);
        row(tr("Total"), b.total, true);
        row(tr("Suggested mux rate"), b.suggestedMux);
        row(tr("Mux rate used"), b.mux, true);
        ImGui::EndTable();
    }
    // the mux rate as a bar: video, audio, overhead, signalling, null packets
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x, bh = 16 * gUi;
    const double full = std::max<double>(b.mux, b.total);
    const ImU32 cols[] = {ImGui::GetColorU32(ImGuiCol_PlotHistogram), ImGui::GetColorU32(ImGuiCol_PlotLines), ImGui::GetColorU32(ImGuiCol_ButtonActive),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled)};
    const double parts[] = {b.video, b.audio, b.pesTs, b.psiSi};
    float x = p0.x;
    dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh), ImGui::GetColorU32(ImGuiCol_FrameBg));
    for (int i = 0; i < 4; i++) {
        const float pw = (float)(parts[i] / full) * bw;
        dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + pw, p0.y + bh), cols[i]);
        x += pw;
    }
    if (b.mux < b.total) dl->AddRect(p0, ImVec2(p0.x + (float)(b.mux / full) * bw, p0.y + bh), IM_COL32(230, 60, 60, 255), 0, 0, 2 * gUi);
    ImGui::Dummy(ImVec2(bw, bh));
    ImGui::TextDisabled("%s", tr("video | audio | headers | signalling | null packets"));
    ImGui::Spacing();
    ImGui::TextWrapped("%s: %d kbit/s, %dx%d, %.1f fps", tr("Video"), effectiveVideoKbps(e), e.width, e.height, e.fps);
    for (const std::string& w : checkSettings(e, b)) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1), "%s", tr(w.c_str()));
}

void sourcesTab(State& s) {
    const bool running = s.gen.running();
    const float right = std::min(340 * gUi, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##vgsettings", ImVec2(ImGui::GetContentRegionAvail().x - right - ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::BeginDisabled(running);
    const float col = 175 * gUi, w = std::max(160 * gUi, std::min(360 * gUi, ImGui::GetContentRegionAvail().x - col - 8 * gUi));
    sourceSection(s, col, w);
    outputSection(s, col, w);
    encodingSection(s, col, w);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##vgestimate", ImVec2(0, 0));
    estimatePanel(s.cfg.enc);
    ImGui::EndChild();
}

// ---- the second tab: overlay editor ----

Block newBlock(BlockKind k, Param p) {
    Block b;
    b.kind = k;
    b.param = p;
    switch (k) {
    case BlockKind::Logo: b.w = 0.18f; b.h = 0.15f; break;
    case BlockKind::Param: b.text = tr(paramLabel(p)); b.fontPx = 22; break;
    case BlockKind::Text: b.text = tr("Your text"); b.fontPx = 28; break;
    case BlockKind::Rolling: b.text = tr("Rolling text: news, call sign, frequency..."); b.x = 0; b.w = 1; b.fontPx = 26; break;
    }
    return b;
}

void paletteItem(const char* label, BlockKind k, Param p, const char* tip, State& s) {
    ImGui::Selectable(label, false);
    if (ImGui::BeginDragDropSource()) {
        const DropItem d{(int)k, (int)p};
        ImGui::SetDragDropPayload("VGBLOCK", &d, sizeof d);
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // double click: place it in the middle
        Block b = newBlock(k, p);
        if (k != BlockKind::Rolling) { b.x = 0.4f; b.y = 0.45f; } else b.y = 0.88f;
        if (k == BlockKind::Logo) { const std::string f = plat::openFileDialog(); b.text = f; }
        s.cfg.overlay.blocks.push_back(b);
        s.sel = (int)s.cfg.overlay.blocks.size() - 1;
    }
    if (tip && ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ImGui::SetTooltip("%s", tip);
}

void palette(State& s) {
    ImGui::TextDisabled("%s", tr("Drag onto the picture"));
    helpMark(N_("Drag a block from this list onto the picture, or double-click it to put it in the middle. Drag a block to move it, and the blue corner to resize a logo or the rolling band. Magnetic: blocks snap to each other's edges and to the picture's edges and centre; hold Alt to place freely. Click a block to change its text, font size, colour and box on the right; Delete removes it. Parameter blocks show the current value from the first tab and follow it. What you see is drawn by the same code as the encoded picture; while encoding, the picture here is the encoder's own and your changes go out on the next frame. More in docs/VIDEO-GENERATOR.md."));
    ImGui::SeparatorText(tr("Blocks"));
    paletteItem(tr("Logo"), BlockKind::Logo, Param::VideoCodec, tr("A logo from a PNG file (transparency kept), scaled into its box"), s);
    paletteItem(tr("Free text"), BlockKind::Text, Param::VideoCodec, tr("Your own text, more lines allowed"), s);
    paletteItem(tr("Rolling text"), BlockKind::Rolling, Param::VideoCodec, tr("A band with text running through it; direction and speed per block"), s);
    for (const ParamGroup& g : kGroups) {
        ImGui::SeparatorText(tr(g.name));
        for (Param p : g.params) {
            const std::string tip = std::string(tr("From the tab \"Sources and encoding\"")) + " > " + tr(g.name) + ". " + tr("Now:") + " " + paramValue(p, s.cfg.enc);
            ImGui::PushID((int)p);
            paletteItem(tr(paramLabel(p)), BlockKind::Param, p, tip.c_str(), s);
            ImGui::PopID();
        }
    }
}

void properties(State& s) {
    std::vector<Block>& bl = s.cfg.overlay.blocks;
    ImGui::Checkbox(tr("Magnetic"), &s.snap);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Blocks snap to each other's edges and to the picture's edges and centre. Hold Alt while dragging to place freely."));
    ImGui::SameLine();
    ImGui::BeginDisabled(bl.empty());
    if (ImGui::Button(tr("Clear all"))) { bl.clear(); s.sel = -1; }
    ImGui::EndDisabled();
    if (s.sel < 0 || s.sel >= (int)bl.size()) { ImGui::TextDisabled("%s", tr("Click a block to change it")); return; }
    Block& b = bl[(size_t)s.sel];
    const float w = ImGui::GetContentRegionAvail().x;
    const char* kinds[] = {N_("Logo"), N_("Parameter"), N_("Free text"), N_("Rolling text")};
    ImGui::SeparatorText(tr(kinds[(int)b.kind]));
    if (b.kind == BlockKind::Logo) {
        ImGui::TextUnformatted(tr("Picture file (PNG)"));
        inputText("##vglogo", b.text, w - 90 * gUi);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse...##vglogo"))) { const std::string f = plat::openFileDialog(); if (!f.empty()) b.text = f; }
        ImGui::SetNextItemWidth(w);
        float pw = b.w * 100, ph = b.h * 100;
        if (ImGui::SliderFloat("##vglw", &pw, 2, 100, tr("width %.0f %%"))) b.w = pw / 100;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##vglh", &ph, 2, 100, tr("height %.0f %%"))) b.h = ph / 100;
    } else {
        if (b.kind == BlockKind::Param) {
            ImGui::TextUnformatted(tr("Parameter"));
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginCombo("##vgparam", tr(paramLabel(b.param)))) {
                for (int i = 0; i < (int)Param::Count; i++)
                    if (ImGui::Selectable(tr(paramLabel((Param)i)), (int)b.param == i)) { b.param = (Param)i; b.text = tr(paramLabel(b.param)); }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted(tr("Label"));
            inputText("##vglabel", b.text, w);
        } else {
            ImGui::TextUnformatted(tr("Text"));
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", b.text.c_str());
            const float th = b.kind == BlockKind::Text ? 80 * gUi : ImGui::GetFrameHeight() * 2;
            if (ImGui::InputTextMultiline("##vgtext", buf, sizeof buf, ImVec2(w, th))) {
                b.text = buf;
                if (b.kind == BlockKind::Rolling) b.text.erase(std::remove(b.text.begin(), b.text.end(), '\n'), b.text.end());
            }
        }
        if (b.kind == BlockKind::Rolling) {
            ImGui::TextUnformatted(tr("Direction"));
            combo("##vgdir", b.direction, kDirNames, 2, w);
            ImGui::TextUnformatted(tr("Speed"));
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##vgcps", &b.charsPerSec, 0.2f, 20.f, tr("%.1f characters/s"), ImGuiSliderFlags_Logarithmic);
            float bw = b.w * 100;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##vgbandw", &bw, 10, 100, tr("band width %.0f %%"))) b.w = bw / 100;
        }
        ImGui::TextUnformatted(tr("Font size"));
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##vgfont", &b.fontPx, 8, 120, "%.0f px");
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(b.color);
        if (ImGui::ColorEdit4(tr("Colour##vgcol"), &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) b.color = ImGui::ColorConvertFloat4ToU32(c);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Dark box behind"), &b.background);
    }
    ImGui::TextUnformatted(tr("Position"));
    float px = b.x * 100, py = b.y * 100;
    ImGui::SetNextItemWidth(w * 0.5f - 4 * gUi);
    if (ImGui::DragFloat("##vgx", &px, 0.2f, -50, 150, "x %.1f %%")) b.x = px / 100;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w * 0.5f - 4 * gUi);
    if (ImGui::DragFloat("##vgy", &py, 0.2f, -50, 150, "y %.1f %%")) b.y = py / 100;
    if (ImGui::Button(tr("Duplicate"))) {
        Block d = b;
        d.x += 0.03f; d.y += 0.03f;
        bl.push_back(d);
        s.sel = (int)bl.size() - 1;
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Delete"))) { bl.erase(bl.begin() + s.sel); s.sel = -1; }
}

void canvas(State& s) {
    const Settings& e = s.cfg.enc;
    const int W = e.width, H = e.height;
    std::vector<Block>& bl = s.cfg.overlay.blocks;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sc = std::max(0.05f, std::min(avail.x / (float)W, (avail.y - ImGui::GetTextLineHeightWithSpacing()) / (float)H));
    const ImVec2 size((float)W * sc, (float)H * sc);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##vgcanvas", size);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), activated = ImGui::IsItemActivated();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = s.gen.running();

    // the picture: the encoder's own frame while it runs (overlay included), else a grey card with the overlay drawn by the same renderer
    if (running) {
        int pw = 0, ph = 0;
        if (s.gen.preview(s.prevRgba, pw, ph, s.prevSerial)) {
            if (!s.prevImg || pw != s.prevW || ph != s.prevH) { delete s.prevImg; s.prevImg = gGfx->createImage(pw, ph, 0xFF000000u); s.prevW = pw; s.prevH = ph; }
            s.prevImg->update(0, 0, pw, ph, reinterpret_cast<const uint32_t*>(s.prevRgba.data()));
        }
        if (s.prevImg) dl->AddImage(s.prevImg->texture(), p0, p1);
        else dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 20, 255));
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(58, 62, 70, 255));
        // title-safe area (90 %) as a guide
        dl->AddRect(ImVec2(p0.x + size.x * 0.05f, p0.y + size.y * 0.05f), ImVec2(p1.x - size.x * 0.05f, p1.y - size.y * 0.05f), IM_COL32(255, 255, 255, 40));
        const bool rolling = std::any_of(bl.begin(), bl.end(), [](const Block& b) { return b.kind == BlockKind::Rolling; });
        const std::string key = saveLayout(s.cfg.overlay) + saveSettings(e);
        const double now = ImGui::GetTime();
        if (key != s.ovKey || (rolling && now - s.ovAt > 1.0 / 15)) {
            s.ovKey = key;
            s.ovAt = now;
            s.ovr.render(s.cfg.overlay, e, now, W, H, s.ovRgba);
            if (!s.ovImg || s.ovW != W || s.ovH != H) { delete s.ovImg; s.ovImg = gGfx->createImage(W, H, 0); s.ovW = W; s.ovH = H; }
            s.ovImg->update(0, 0, W, H, reinterpret_cast<const uint32_t*>(s.ovRgba.data()));
        }
        if (s.ovImg) dl->AddImage(s.ovImg->texture(), p0, p1);
    }

    std::vector<Rect> rects;
    for (const Block& b : bl) rects.push_back(s.ovr.blockRect(b, e, W, H));
    auto toScreen = [&](const Rect& r) { return std::make_pair(ImVec2(p0.x + r.x * sc, p0.y + r.y * sc), ImVec2(p0.x + (r.x + r.w) * sc, p0.y + (r.y + r.h) * sc)); };
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float hs = 7 * gUi;   // resize handle
    auto resizable = [](const Block& b) { return b.kind == BlockKind::Logo || b.kind == BlockKind::Rolling; };

    // pick a block, or its resize handle
    if (activated) {
        s.sel = -1;
        s.dragging = s.resizing = false;
        for (int i = (int)bl.size() - 1; i >= 0; i--) {
            const auto [a, z] = toScreen(rects[(size_t)i]);
            const bool onHandle = resizable(bl[(size_t)i]) && m.x >= z.x - hs && m.x <= z.x + hs && m.y >= z.y - hs && m.y <= z.y + hs;
            if (onHandle || (m.x >= a.x && m.x <= z.x && m.y >= a.y && m.y <= z.y)) {
                s.sel = i;
                s.resizing = onHandle;
                s.dragging = !onHandle;
                s.grab = ImVec2(m.x - a.x, m.y - a.y);
                break;
            }
        }
    }
    bool snappedX = false, snappedY = false;
    if (active && s.sel >= 0 && s.sel < (int)bl.size()) {
        Block& b = bl[(size_t)s.sel];
        if (s.dragging) {
            Rect r = rects[(size_t)s.sel];
            r.x = (m.x - s.grab.x - p0.x) / sc;
            r.y = (m.y - s.grab.y - p0.y) / sc;
            if (s.snap && !ImGui::GetIO().KeyAlt) {
                std::vector<Rect> others;
                for (size_t i = 0; i < rects.size(); i++) if ((int)i != s.sel) others.push_back(rects[i]);
                const Rect sn = snapRect(r, others, (float)W, (float)H, 8 * gUi / sc);
                snappedX = sn.x != r.x;
                snappedY = sn.y != r.y;
                r = sn;
            }
            b.x = r.x / (float)W;
            b.y = r.y / (float)H;
            rects[(size_t)s.sel] = s.ovr.blockRect(b, e, W, H);
        } else if (s.resizing) {
            const Rect r = rects[(size_t)s.sel];
            b.w = std::clamp(((m.x - p0.x) / sc - r.x) / (float)W, 0.02f, 1.f);
            if (b.kind == BlockKind::Logo) b.h = std::clamp(((m.y - p0.y) / sc - r.y) / (float)H, 0.02f, 1.f);
            rects[(size_t)s.sel] = s.ovr.blockRect(b, e, W, H);
        }
    }
    if (ImGui::IsItemDeactivated()) s.dragging = s.resizing = false;

    // a block dropped from the palette lands where the mouse is
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("VGBLOCK")) {
            DropItem d{};
            if (pl->Data && pl->DataSize == (int)sizeof d) std::copy_n(static_cast<const unsigned char*>(pl->Data), sizeof d, reinterpret_cast<unsigned char*>(&d));
            Block b = newBlock((BlockKind)d.kind, (Param)d.param);
            const Rect r0 = s.ovr.blockRect(b, e, W, H);
            Rect r{(m.x - p0.x) / sc - (b.kind == BlockKind::Rolling ? 0 : r0.w / 2), (m.y - p0.y) / sc - r0.h / 2, r0.w, r0.h};
            if (s.snap) r = snapRect(r, rects, (float)W, (float)H, 8 * gUi / sc);
            b.x = r.x / (float)W;
            b.y = r.y / (float)H;
            if (b.kind == BlockKind::Logo) b.text = plat::openFileDialog();
            bl.push_back(b);
            rects.push_back(s.ovr.blockRect(b, e, W, H));
            s.sel = (int)bl.size() - 1;
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered && s.sel >= 0 && s.sel < (int)bl.size() && ImGui::IsKeyPressed(ImGuiKey_Delete)) { bl.erase(bl.begin() + s.sel); rects.erase(rects.begin() + s.sel); s.sel = -1; }

    // outlines, the selection and its handle, the snap guides
    dl->PushClipRect(p0, p1, true);
    for (size_t i = 0; i < bl.size(); i++) {
        const auto [a, z] = toScreen(rects[i]);
        const bool sel = (int)i == s.sel;
        dl->AddRect(a, z, sel ? IM_COL32(80, 200, 255, 255) : IM_COL32(255, 255, 255, 70), 0, 0, sel ? 2 * gUi : 1);
        if (bl[i].kind == BlockKind::Logo && bl[i].text.empty()) dl->AddText(ImVec2(a.x + 4 * gUi, a.y + 4 * gUi), IM_COL32(255, 255, 255, 160), tr("Logo: choose a PNG file"));
        if (sel && resizable(bl[i])) dl->AddRectFilled(ImVec2(z.x - hs, z.y - hs), ImVec2(z.x, z.y), IM_COL32(80, 200, 255, 255));
    }
    if (s.sel >= 0 && s.sel < (int)rects.size() && (snappedX || snappedY)) {
        const auto [a, z] = toScreen(rects[(size_t)s.sel]);
        const ImU32 g = IM_COL32(255, 210, 60, 200);
        if (snappedX) { dl->AddLine(ImVec2(a.x, p0.y), ImVec2(a.x, p1.y), g); dl->AddLine(ImVec2(z.x, p0.y), ImVec2(z.x, p1.y), g); }
        if (snappedY) { dl->AddLine(ImVec2(p0.x, a.y), ImVec2(p1.x, a.y), g); dl->AddLine(ImVec2(p0.x, z.y), ImVec2(p1.x, z.y), g); }
    }
    dl->PopClipRect();
    if (!s.ovr.hasFont()) ImGui::TextDisabled("%s", tr("No font for the overlay text was found: text blocks are not drawn."));
    else ImGui::TextDisabled("%dx%d. %s", W, H, tr("Drag to move, the corner to resize (logo, rolling text), Delete removes the selected block."));
}

void overlayTab(State& s) {
    const float leftW = 210 * gUi, rightW = 270 * gUi;
    ImGui::BeginChild("##vgpalette", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
    palette(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##vgcanvaschild", ImVec2(ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x, 0));
    canvas(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##vgprops", ImVec2(0, 0), ImGuiChildFlags_Borders);
    properties(s);
    ImGui::EndChild();
    // live: the running encoder takes the edited overlay on its next frame
    if (s.gen.running()) {
        const std::string l = saveLayout(s.cfg.overlay);
        if (l != s.sentLayout) { s.gen.setOverlay(s.cfg.overlay); s.sentLayout = l; }
    }
}

// ---- above both tabs: start / stop, progress, the address to open ----

void controlBar(State& s) {
    const bool running = s.gen.running();
    const GenStats st = s.gen.stats();
    const GenConfig& c = s.cfg;
    if (!running) {
        ImGui::BeginDisabled(!s.probed);
        if (ImGui::Button(c.output == Output::File ? tr("Start encoding") : tr("Start streaming"))) {
            GenConfig run = c;
            run.enc.videoEncoder = pickVideoEncoder(c.enc.videoEncoder, videoEncoders());
            run.font = s.font;
            std::string err;
            if (s.gen.start(run, err)) { s.msg.clear(); s.sentLayout = saveLayout(c.overlay); s.startedAt = ImGui::GetTime(); s.prevSerial = 0; }
            else s.msg = err;
        }
        ImGui::EndDisabled();
        helpMark(kToolHelp);
        if (!s.probed) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Looking for encoders and cameras...")); }
    } else {
        if (ImGui::Button(tr("Stop"))) s.gen.stop();
        helpMark(kToolHelp);
    }
    ImGui::SameLine();
    if (running) {
        char t[200];
        if (c.output == Output::File) {
            snprintf(t, sizeof t, "%s: %.1f / %.1f s, %.0f kbit/s", tr("Encoding"), st.seconds, c.seconds, st.kbps);
            ImGui::SetNextItemWidth(200 * gUi);
            ImGui::ProgressBar((float)st.progress, ImVec2(200 * gUi, 0));
            ImGui::SameLine();
        } else {
            const int sec = (int)st.seconds;
            snprintf(t, sizeof t, "%s: %d:%02d, %.0f kbit/s", tr("Streaming"), sec / 60, sec % 60, st.kbps);
        }
        ImGui::TextUnformatted(t);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s", st.videoEncoder.c_str(), st.audioEncoder.empty() ? "" : " + ", st.audioEncoder.c_str());
        if (st.lateFrames > 0) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1), tr("%d frames late: the encoder is too slow for real time"), st.lateFrames); }
    } else if (!st.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "%s", st.error.c_str());
    } else if (!s.msg.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "%s", tr(s.msg.c_str()));
    } else if (st.finished && c.output == Output::File && st.frames > 0) {
        ImGui::TextDisabled("%s: %s (%.1f s, %.1f MB). %s", tr("Written"), c.tsPath.c_str(), st.seconds, st.bytes / 1e6, tr("The transmitter takes it as its \"MPEG-TS / video file\"."));
    } else ImGui::NewLine();

    // where to watch the stream: the address set here, with a copy button and the VLC hint
    if (c.output != Output::File) {
        const std::string url = streamUrl(c.output, c.host, c.port);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(running ? tr("Streaming to") : tr("Will stream to"));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", url.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy##vgurl"))) ImGui::SetClipboardText(url.c_str());
        const std::string vlc = vlcOpenUrl(c.output, c.host, c.port);
        const std::string where = isMulticast(c.host) ? std::string(tr("In VLC on a receiving PC (multicast group)")) : std::string(tr("In VLC on the receiving PC")) + " (" + c.host + ")";
        ImGui::TextDisabled("%s: %s > %s", where.c_str(), tr("Media > Open Network Stream"), vlc.c_str());
    }
}

} // namespace

void videoGenWindow(App& a) {
    State& s = S();
    if (!a.videoGenOpen) {
        if (s.init && s.gen.running()) s.gen.stop();   // closing the window stops the stream or the file (it is closed properly)
        return;
    }
    loadOnce(s);
    if (!s.probed && s.probe.valid() && s.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        s.probe.get();
        s.cams = s.camsFound;
        s.mics = s.micsFound;
        s.probed = true;
        const std::string want = s.cfg.enc.videoEncoder;
        s.cfg.enc.videoEncoder = pickVideoEncoder(want, videoEncoders());
        const auto& ae = audioEncoders();
        if (!ae.empty() && std::none_of(ae.begin(), ae.end(), [&](const EncoderInfo& i) { return i.name == s.cfg.enc.audioEncoder; })) s.cfg.enc.audioEncoder = ae.front().name;
    }
    firstWindowSize(1180 * gUi, 820 * gUi);
    if (!ImGui::Begin(trw("Digital video generator"), &a.videoGenOpen)) { ImGui::End(); return; }
    controlBar(s);
    ImGui::Separator();
    if (ImGui::BeginTabBar("##vgtabs")) {
        if (ImGui::BeginTabItem(trw("Sources and encoding"))) { sourcesTab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Overlay"))) { overlayTab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Digital Television Mode Selector"))) { dvbModeSelectorTab(estimate(s.cfg.enc).mux); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
    saveSoon(s);
}
