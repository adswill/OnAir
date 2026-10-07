// DRM screens. Placeholder written by the mode plumbing: the mode's author replaces it.
#include "app.h"

namespace {
void panels(App& a) { ImGui::TextDisabled("DRM: the analysis plots are not written yet"); ImGui::TextDisabled("%s", modeSummary(a.rx).c_str()); }
void status(App& a) { ImGui::TextDisabled("%s", a.engine.running() ? modeSummary(a.rx).c_str() : "stopped"); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = "DRM"; l2 = modeSummary(a.rx); }
void receiver(App& a) { ImGui::TextDisabled("%s", modeSummary(a.rx).c_str()); }
} // namespace

extern const ModeUi kDrmUi;
const ModeUi kDrmUi = {
    .sideTitle = "SERVICES",
    .receiver = receiver,
    .stream = false,
    .panels = panels,
    .status = status,
    .summary = summary,
};
