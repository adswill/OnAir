// First-run tour with Onny the TV.
#include "app.h"

// First-run tour with Onny the TV: a friendly guide that explains the window, plays the synthetic demo signal and helps pick a radio.
// Included by main.cpp (needs App, setFamily, refreshDevices, savePrefs, gForceTab).


struct WizStep {
    const char* text;
    int target;
    mascot::Mood mood;
    bool wave;
    const char* action;    // label of an extra button (nullptr: none); handled in wizAction()
    const char* tab;       // tab to bring up while this step is shown (nullptr: leave)
};

static const WizStep kWiz[] = {
    {"Hi, I'm Onny! I'm a little TV who lives in OnAir. Want a quick tour? It takes about a minute, and I'll show you everything with a demo signal, so you don't need any hardware yet.",
     TgNone, mascot::Excited, true, nullptr, nullptr},
    {"Up here is the switch for what kind of broadcast you want: DVB for most of the world's digital TV, ATSC for North America, and DAB for digital radio. OnAir has all three in one place.",
     TgSwitch, mascot::Happy, false, nullptr, nullptr},
    {"Let's see it in action! I'll start a pretend signal. It is a test transmitter running inside the app, so nothing needs to be plugged in.",
     TgToolbar, mascot::Excited, false, "Start the demo signal", nullptr},
    {"See the wiggly line? That is the spectrum: how strong everything on the air is, from left to right. The tall bump in the middle is our channel. Below it, the waterfall shows the same thing slowly scrolling by in time.",
     TgMain, mascot::Happy, false, nullptr, "Overview"},
    {"And these four boxes show the dots the receiver sees. When the signal is good, the dots gather into tidy clusters. Messy dots mean a noisy signal. Right now it's a clean demo, so they should look neat!",
     TgConst, mascot::Happy, false, nullptr, nullptr},
    {"Over here you'll find the channels once the receiver locks on. Click one to watch it. There's also a player with picture and sound, a clock, and options to record or stream what you're watching.",
     TgRight, mascot::Happy, false, nullptr, nullptr},
    {"Now for the real thing. Pick your radio here, choose a frequency, and set the gain. Gain is how much your radio boosts the signal: too little and it's lost, too much and it distorts. The Auto-tune button finds a good value for you.",
     TgToolbar, mascot::Thinking, false, "Stop the demo and look for my radio", nullptr},
    {"Don't know where the channels are? The Scan tab searches for you. A good antenna, placed high and near a window, makes a bigger difference than anything else. I promise!",
     TgMain, mascot::Happy, false, nullptr, "Scan"},
    {"That's the whole tour! You can share your channels with other devices from the Stream tab (Outputs), and bring me back any time with the Tour button at the top right. Happy watching!",
     TgNone, mascot::Excited, true, nullptr, nullptr},
};
static const int kWizSteps = (int)(sizeof kWiz / sizeof *kWiz);

void wizFinish(App& a) {
    a.wizOpen = false;
    plat::prefs().setB("wizardDone", true);
    plat::prefs().flush();
}

void wizEnter(App& a, int step) {
    a.wizStep = std::max(0, std::min(kWizSteps - 1, step));
    a.wizStepT = ImGui::GetTime();
    a.wizAcked = false;
    if (kWiz[a.wizStep].tab) gForceTab = kWiz[a.wizStep].tab;
}

void wizAction(App& a, int step) {
    if (step == 2) {   // start the synthetic demo
        for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].kind == DeviceInfo::Synthetic) a.devIdx = i;
        setFamily(a, 0);
        a.stdMode = 2;                 // DVB-T: the synthetic DVB-T signal carries a real test programme
        a.tune.synth.dvbt = true; a.tune.synth.demoTv = true;
        if (a.engine.running()) a.engine.stop();
        a.wizStart = true;
        a.playReq = 1;                 // play the demo programme as soon as the receiver has found it
    } else if (step == 6) {   // back to real hardware
        if (a.engine.running()) a.engine.stop();
        refreshDevices(a);
        a.devIdx = std::max(0, std::min(a.devIdx, (int)a.devices.size() - 1));   // a radio unplugged since: the list is shorter now
        for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) { a.devIdx = i; break; }
    }
}

void wizard(App& a, ImVec2 disp) {
    if (!a.wizOpen) return;
    const double now = ImGui::GetTime();
    const WizStep& st = kWiz[a.wizStep];
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImU32 acc = ImGui::ColorConvertFloat4ToU32(pal::accent());

    // the part of the window the step talks about gets a pulsing frame
    ImVec2 tgt(disp.x * 0.5f, disp.y * 0.3f);
    bool havePt = false;
    if (st.target != TgNone && a.tgMax[st.target].x > a.tgMin[st.target].x) {
        const ImVec2 lo = a.tgMin[st.target], hi = a.tgMax[st.target];
        const float pulse = 0.55f + 0.45f * (float)std::sin(now * 4.0);
        fg->AddRectFilled(lo, hi, IM_COL32(115, 184, 209, 14), 4.f);
        fg->AddRect(ImVec2(lo.x - 2, lo.y - 2), ImVec2(hi.x + 2, hi.y + 2), (acc & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24), 4.f, 0, 2.f);
        tgt = ImVec2((lo.x + hi.x) * 0.5f, std::min(hi.y - 12.f, lo.y + 40.f));
        havePt = true;
    }

    // the panel sits in a bottom corner and moves to the other one when it would cover what it is pointing at
    const float u = gUi;
    const float W = std::min(780.f * u, disp.x - 24.f * u), H = 196.f * u;
    const bool right = havePt && tgt.x < disp.x * 0.5f && tgt.y > disp.y - H - 80.f * u;
    const float goalX = right ? disp.x - W - 12.f * u : 12.f * u;
    if (a.wizX < 0) a.wizX = goalX;
    a.wizX += (goalX - a.wizX) * std::min(1.f, ImGui::GetIO().DeltaTime * 8.f);
    const ImVec2 pos(a.wizX, disp.y - H - 12.f * u);

    // typewriter
    const std::string text = (a.wizStep == 6 && !a.engine.running())
        ? [&] {
              std::string n;
              for (auto& d : a.devices) if (d.isRadio()) { n = d.name; break; }
              return n.empty() ? std::string(st.text) + " (I can't see a radio right now. Plug it in and press the button again."
#ifdef _WIN32
                                                    " On Windows a HackRF needs its WinUSB driver once: install it with Zadig, at zadig.akeo.ie.)"
#else
                                                    ")"
#endif
                               : std::string(st.text) + " Good news: I can see \"" + n + "\"!";
          }()
        : std::string(st.text);
    const size_t shown = std::min(text.size(), (size_t)((now - a.wizStepT) * 75.0));
    const bool talking = shown < text.size();

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.082f, 0.09f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##onny", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Onny himself, in the left part of the panel (drawn on the foreground list so the antenna may leave the panel)
    mascot::Pose pose;
    pose.t = now; pose.talking = talking; pose.mood = st.mood; pose.wave = st.wave; pose.point = havePt; pose.target = tgt;
    mascot::draw(fg, ImVec2(pos.x + 100.f * u, pos.y + 98.f * u), 0.95f * u, pose, acc);

    // speech: a text area and the buttons
    const float tx = 200.f * u;
    ImGui::SetCursorPos(ImVec2(tx, 14 * u));
    ImGui::PushTextWrapPos(W - 18.f * u);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + shown);
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && talking) a.wizStepT = now - 1000;   // click: show everything at once

    // progress dots
    {
        const ImVec2 w0 = ImGui::GetWindowPos();
        for (int i = 0; i < kWizSteps; i++) {
            const ImVec2 c(w0.x + tx + 6 * u + i * 14 * u, w0.y + H - 62 * u);
            if (i == a.wizStep) ImGui::GetWindowDrawList()->AddCircleFilled(c, 4.f * u, acc);
            else ImGui::GetWindowDrawList()->AddCircle(c, 3.5f * u, IM_COL32(90, 100, 108, 255), 0, 1.2f);
        }
    }
    ImGui::SetCursorPos(ImVec2(tx, H - 44 * u));
    if (a.wizStep > 0 && ImGui::Button("Back", ImVec2(70 * gUi, 0))) { wizEnter(a, a.wizStep - 1); }
    if (a.wizStep > 0) ImGui::SameLine();
    if (st.action) {
        if (ImGui::Button(st.action)) { wizAction(a, a.wizStep); a.wizAcked = true; }
        ImGui::SameLine();
    }
    const bool last = a.wizStep == kWizSteps - 1;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.34f, 0.40f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.45f, 0.53f, 1));
    if (ImGui::Button(last ? "Finish" : "Next", ImVec2(80 * gUi, 0))) { if (last) wizFinish(a); else wizEnter(a, a.wizStep + 1); }
    ImGui::PopStyleColor(2);
    if (!last) {
        ImGui::SameLine(0, 18 * gUi);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button("Skip tour")) wizFinish(a);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

