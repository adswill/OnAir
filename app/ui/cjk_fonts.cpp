#include "cjk_fonts.h"
#include "i18n/i18n.h"
#include "dect2/common/inflate.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cjk {
namespace data {
extern const unsigned char regular[], bold[];
extern const size_t regularSize, boldSize;
}

void configureBase(ImFontConfig& config) {
    if (std::strcmp(i18n::current(), "zh") != 0) return;
    static const ImWchar exclude[] = {0x2e80, 0x9fff, 0xa960, 0xa97f, 0xac00, 0xd7ff, 0xf900, 0xfaff, 0xff00, 0xffef, 0};
    config.GlyphExcludeRanges = exclude;
}

void merge(ImGuiIO& io, ImFont* font, Weight weight) {
    if (std::strcmp(i18n::current(), "zh") != 0) return;
    static std::vector<uint8_t> fonts[2];
    auto& raw = fonts[weight];
    if (raw.empty()) {
        const unsigned char* bytes[] = {data::regular, data::bold};
        const size_t sizes[] = {data::regularSize, data::boldSize};
        if (!dect2::inflateRaw(bytes[weight], sizes[weight], raw) || raw.empty()) {
            std::fprintf(stderr, "Could not load the bundled Chinese font\n");
            std::abort();   // a corrupt embedded resource must not silently change the font family
        }
    }
    ImFontConfig config;
    config.MergeMode = true;
    config.DstFont = font;
    config.FontDataOwnedByAtlas = false;   // shared across font roles, sizes and contexts
    const char* names[] = {"Noto Sans CJK SC Regular subset", "Noto Sans CJK SC Bold subset"};
    std::snprintf(config.Name, sizeof config.Name, "%s", names[weight]);
    // Noto 2.004 has a 1000-unit em in a 1448-unit ascender/descender box.
    // stb scales by that larger box; normalize to a 0.9-line em so Chinese remains
    // readable alongside Inter/Menlo without overflowing the base font's line height.
    // Use the base reference size, not a fixed 13px, for identical scaling in every role.
    constexpr float lineBoxToEm = 1448.f / 1000.f;
    const float size = font->Sources[0]->SizePixels * lineBoxToEm * 0.9f;
    io.Fonts->AddFontFromMemoryTTF(raw.data(), (int)raw.size(), size, &config);
}
}
