// onair-videogen, the Digital video generator as its own program (#458): the state of its window.
//
// One generator = one multiplex: several sources (tabs "Source 1", "Source 2" ..., each an input with its own encoding and overlay),
// put together on the "Muxing" tab and sent to OnAir's transmitter over the link (vglink.h), UDP, RTP or a .ts file. The engine runs
// on its own threads (videogen_session.h); the window only draws and reads its status.
//   vg_main.cpp     the program: window, graphics, fonts, the frame loop, command line
//   vg_window.cpp   the window: Easy tab or the Advanced tabs, Muxing, Preview, start / stop, the link to OnAir
//   vg_sources.cpp  a source's rows (input, encoding), the bitrate estimate, the overlay editor and the preview picture
#pragma once
#include "gfx.h"
#include "i18n.h"
#include "imgui.h"
#include "dect2/audio/audioin.h"
#include "dect2/audio/videoin.h"
#include "dect2/tv/videogen.h"
#include "dect2/tv/videogen_session.h"
#include "dect2/tv/vglink.h"
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "scale.h"               // gUi: the interface scale
extern gfx::Backend* gGfx;

namespace vgui {
using namespace dect2::vgen;

struct SourceTab {
    int idx = 0;                // 0 = Source 1
    GenConfig cfg;
    bool lengthEdited = false;
    MuxNumbering num;           // Advanced Muxing: its programme number and PIDs (0 / empty: automatic)
    std::string msg, note;      // msg: a problem (red); note: what a drop just set
    std::string savedCfg, savedLayout, sentLayout;
    // overlay editor
    int sel = -1;
    bool dragging = false, resizing = false, snap = true;
    ImVec2 grab{0, 0};
    OverlayRenderer ovr;
    gfx::Image* ovImg = nullptr;
    int ovW = 0, ovH = 0;
    std::string ovKey;
    double ovAt = 0;
    std::vector<uint8_t> ovRgba;
    // the picture of the preview thread while this source is shown
    gfx::Image* prevImg = nullptr;
    int prevW = 0, prevH = 0;
    std::string previewKey;     // what a preview-only run was started with (pictureKey)
    double minRateNoticeUntil = 0;   // the video bitrate was raised to the MPEG-2 minimum: explain why until then
};

// The Muxing tab
struct MuxUi {
    Output output = Output::Transmitter;   // Transmitter (over the link), Udp, Rtp, File
    std::string tsPath, host = "127.0.0.1";
    int port = 5000, ttl = 8;
    bool capFromLink = true;    // the channel's capacity from the transmitter; off: capKbps as typed
    int capKbps = 10000;
    bool padding = true;
    int firstServiceId = 1, tsid = 1;
    std::string provider = "OnAir";
};

struct Gen {
    std::vector<std::unique_ptr<SourceTab>> src;
    int cur = 0;                // the source tab shown
    bool advanced = false;
    std::future<void> probe;    // encoders, cameras and microphones
    bool probed = false;
    std::vector<dect2::VideoInputDevice> cams, camsFound;
    std::vector<dect2::AudioInputDevice> mics, micsFound;
    std::shared_ptr<const std::vector<uint8_t>> font;
    Session ses;
    VgLinkClient link;
    std::string linkHost, token, name = "Video generator";
    int linkPort = kVgLinkDefaultPort, slot = 1;
    bool startedByOnAir = false;
    bool standalone = false;     // started on its own: neither by OnAir (token) nor pointed at one (--onair)
    int easyKbps = 0;            // Easy mode streaming: the video CBR and frame rate it runs with; a new channel rate re-fits them (#458)
    double easyFps = 0, easyDiffAt = -1;
    double startWanted = -1;     // OnAir's Start (#458): stream in Easy mode once linked and probed; the time it was asked (-1: not asked)
    MuxUi mux;
    int previewSrc = 0;
    bool previewOn = false;
    std::string msg;            // a problem with the multiplex (red)
    double startedAt = 0;
    std::string savedMux;
    double lastSave = 0;
};
Gen& G();

// ---- vg_sources.cpp ----
std::string cfgPath(int i);
std::string layoutPath(int i);
std::string readText(const std::string& p);
void writeText(const std::string& p, const std::string& t);
std::string saveCfg(const GenConfig& c, bool advanced);
void loadCfg(const std::string& text, GenConfig& c, bool& advanced);
std::shared_ptr<const std::vector<uint8_t>> atlasFont();
bool inputText(const char* label, std::string& s, float width);
void helpMark(const char* text);
void rowLabel(const char* english, float col);
bool combo(const char* id, int& v, const char* const* names, int n, float width, const std::vector<bool>* enabled = nullptr, const char* offTip = nullptr);
std::string kbpsText(double kbps);
void startProbe(Gen& g);
std::string pictureKey(const GenConfig& c);
void fitLength(SourceTab& s);
void applyDrops(SourceTab& s, bool locked);
void sourceSection(SourceTab& s, float col, float w, bool advanced);
void frameRows(Settings& e, float col, float w);
void encodingSection(SourceTab& s, float col, float w);
void lengthRow(SourceTab& s, float col, bool file);   // a source's length in a .ts file
void estimatePanel(const Settings& e);
// the picture: the preview thread's while it shows this source, else a grey card with the overlay; editable: the overlay editor's
// selection and dragging; sending: no red cross
void canvas(SourceTab& s, bool editable, bool sending, bool previewing);
void overlayEditor(SourceTab& s, bool sending, bool previewing);
extern const char* const kToolHelp;

} // namespace vgui

bool vgQueueDrop(int n, const char** paths);   // files dropped on the window (GLFW's drop callback)
