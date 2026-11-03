// Shared Chinese font subsets for all desktop platforms; system fonts only fill remaining gaps.
#pragma once
#include "imgui.h"

namespace cjk {
enum Weight { Regular, Bold };
// Reserve CJK characters for the bundled subset and system fallback sources.
void configureBase(ImFontConfig& config);
// Call immediately after adding the base font, before merging system fallbacks.
void merge(ImGuiIO& io, ImFont* font, Weight weight = Regular);
}
