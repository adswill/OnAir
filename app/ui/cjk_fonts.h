// A shared Chinese font family for all desktop platforms; system fonts only fill remaining gaps.
#pragma once
#include "imgui.h"

namespace cjk {
enum Weight { Regular, Medium, Bold };
// Keep Han, kana, Hangul and fullwidth punctuation together in the CJK source.
void configureBase(ImFontConfig& config);
// Call immediately after adding the base font, before merging system fallbacks.
void merge(ImGuiIO& io, ImFont* font, Weight weight = Regular);
}
