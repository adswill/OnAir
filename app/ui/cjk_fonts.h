// Chinese UI font subsets for all desktop platforms; other UI languages keep their system fallbacks.
#pragma once
#include "imgui.h"
#include <cstdint>
#include <vector>

namespace cjk {
enum Weight { Regular, Bold };
// Reserve CJK characters for the bundled subset and system fallbacks only when i18n::current() is Chinese.
void configureBase(ImFontConfig& config);
// Call immediately after adding the base font, before system fallbacks. No-op for non-Chinese UI languages.
void merge(ImGuiIO& io, ImFont* font, Weight weight = Regular);
// The bundled Regular face as OpenType bytes (drawn at 16 dots by the CP16 mode)
std::vector<uint8_t> regularFontData();
}
