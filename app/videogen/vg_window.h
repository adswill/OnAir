// onair-videogen: what the program (vg_main.cpp) calls of its window (vg_window.cpp, vg_state.h)
#pragma once
#include <string>

namespace vgui {

struct Options {
    std::string host, token;    // OnAir's address (127.0.0.1) and the token it started this generator with ("": started by itself)
    int port = 0;               // OnAir's RTP port (0: kVgLinkDefaultPort)
    int slot = 1;               // the generator number it asks for
    bool easy = false, advanced = false;   // --easy / --advanced for this run (screenshots)
};

struct FrameResult {
    bool raise = false;         // OnAir asked: come to the front
    bool quit = false;          // OnAir asked: quit (it is closing)
};

void init(const Options& o);    // after ImGui and the fonts exist
FrameResult frame();            // between ImGui::NewFrame and ImGui::Render
void shutdown(bool userClosed); // stops everything; userClosed: tells OnAir the user closed the window
void ensureSources(int n);      // --sources n (screenshots)
void setPreview(bool on);       // --preview (screenshots)
void selectTab(const std::string& name);   // --tab easy | source1 .. source8 | muxing | preview (screenshots)

} // namespace vgui
