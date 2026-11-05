// The typography system in one place: the font set (Inter for text, JetBrains Mono for numbers), the size scale, the spacing
// and the text colours. Three looks, chosen in View > Font look and saved as a preference (default Current):
//   0 = Current (system monospace font, current spacing)   1 = Clean   2 = Compact
#pragma once
#include "imgui.h"

namespace typo {

enum Variant { Classic = 0, Clean = 1, Compact = 2 };
Variant variant();
// set once at start-up from the saved preference, before loadFonts(); a change in the menu applies after a restart
void setVariant(Variant v);
inline bool on() { return variant() != Classic; }

// the size scale, in pixels at scale 1 (monotonic: tested)
enum Size { Caption, Small, Body, BodyLarge, Title, Display, Hero, kSizeCount };
extern const float kSizes[kSizeCount];   // 11 12 13 14 16 20 32
float baseSize();             // Clean: Body-large (14); Compact: Body (13); Current: 13

struct Fonts { ImFont *regular = nullptr, *medium = nullptr, *semibold = nullptr, *mono = nullptr, *monoBold = nullptr; };
Fonts& fonts();

// Loads the five fonts (embedded) when the look is not Classic; bundled Noto CJK is merged first for Chinese UI only;
// mergeFallback(io, font) adds the system fallbacks in every language.
// Returns false (and leaves everything as it was) for Classic or when a font fails to load.
bool loadFonts(ImGuiIO& io, void (*mergeFallback)(ImGuiIO&, ImFont*));
// the embedded JetBrains Mono at `size`: the Classic look when no system monospaced font exists (NixOS, minimal containers)
ImFont* loadEmbeddedMono(ImGuiIO& io, float size, void (*mergeFallback)(ImGuiIO&, ImFont*));
// spacing, rounding and text colours for the look; call after applyTheme() and before the style is scaled
void applyStyle(ImGuiStyle& st);

// Section headers in SemiBold, tabs in Medium (the selected one SemiBold); the same calls as ImGui's in the classic look
bool collapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0);
void separatorText(const char* label);
bool beginTabItem(const char* label, bool* open = nullptr, ImGuiTabItemFlags flags = 0);

// push/pop a font of the set (no-ops in the classic look)
struct Scoped {
    bool pushed = false;
    explicit Scoped(ImFont* f, float size = 0.f) { if (f && on()) { ImGui::PushFont(f, size); pushed = true; } }
    ~Scoped() { if (pushed) ImGui::PopFont(); }
};

} // namespace typo
