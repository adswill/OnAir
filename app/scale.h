// Interface scale for displays where the window is measured in physical pixels (Windows, Linux with scaling): 1 = 96 dpi.
// Every hard-coded pixel size in the UI is multiplied by it; the fonts and the ImGui style are scaled with it as well.
// (On macOS the window is measured in points and the Retina scaling is handled by the framebuffer, so it stays 1.)
#pragma once
inline float gUi = 1.f;
