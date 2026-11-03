#include <algorithm>
#include <cmath>
#include "typography.h"
#include "cjk_fonts.h"
#include "imgui_internal.h"
#include "dect2/common/inflate.h"
#include <vector>

namespace typo {

#include "typo_fonts.inc"

const float kSizes[kSizeCount] = {11, 12, 13, 14, 16, 20, 32};

static Variant gVariant = Classic;

Variant variant() { return gVariant; }
void setVariant(Variant v) { gVariant = v; }

float baseSize() { return variant() == Compact ? kSizes[Body] : variant() == Clean ? kSizes[BodyLarge] : 13.f; }

Fonts& fonts() { static Fonts f; return f; }

static std::vector<uint8_t>& store(int i) { static std::vector<uint8_t> v[5]; return v[i]; }

static ImFont* add(ImGuiIO& io, int slot, const unsigned char* z, size_t zn, float size, void (*merge)(ImGuiIO&, ImFont*), cjk::Weight weight = cjk::Regular) {
    std::vector<uint8_t>& raw = store(slot);
    if (!dect2::inflateRaw(z, zn, raw) || raw.empty()) return nullptr;
    ImFontConfig c;
    cjk::configureBase(c);
    c.FontDataOwnedByAtlas = false;   // the vector above keeps the bytes
    c.OversampleH = 2; c.OversampleV = 1;
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(raw.data(), (int)raw.size(), size, &c);
    if (f) cjk::merge(io, f, weight);
    if (f && merge) merge(io, f);   // ImGui 1.92 merges into the font added last, so each base font gets its fallbacks right away
    return f;
}

bool loadFonts(ImGuiIO& io, void (*merge)(ImGuiIO&, ImFont*)) {
    if (!on()) return false;
    Fonts f;
    const float s = baseSize();
    f.regular = add(io, 0, kFont_Inter_Regular, sizeof kFont_Inter_Regular, s, merge);
    f.medium = add(io, 1, kFont_Inter_Medium, sizeof kFont_Inter_Medium, s, merge);   // share the Regular Chinese subset with the Inter Medium role
    f.semibold = add(io, 2, kFont_Inter_SemiBold, sizeof kFont_Inter_SemiBold, s, merge, cjk::Bold);
    f.mono = add(io, 3, kFont_JetBrainsMono_Regular, sizeof kFont_JetBrainsMono_Regular, s - 0.5f, merge);
    f.monoBold = add(io, 4, kFont_JetBrainsMono_Bold, sizeof kFont_JetBrainsMono_Bold, s - 0.5f, merge, cjk::Bold);
    if (!f.regular || !f.medium || !f.semibold || !f.mono || !f.monoBold) return false;
    fonts() = f;
    return true;
}

ImFont* loadEmbeddedMono(ImGuiIO& io, float size, void (*merge)(ImGuiIO&, ImFont*)) {
    return add(io, 3, kFont_JetBrainsMono_Regular, sizeof kFont_JetBrainsMono_Regular, size, merge);
}

void applyStyle(ImGuiStyle& st) {
    if (!on()) return;
    const bool c = variant() == Compact;
    // a calmer rhythm: more air around text than the instrument look, 4 px steps
    st.FramePadding = c ? ImVec2(6, 3) : ImVec2(8, 4);
    st.ItemSpacing = c ? ImVec2(6, 4) : ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.WindowPadding = c ? ImVec2(8, 6) : ImVec2(10, 8);
    st.CellPadding = c ? ImVec2(5, 2) : ImVec2(6, 3);
    st.ScrollbarSize = 10; st.GrabMinSize = 6;
    st.FrameRounding = 4; st.TabRounding = 4; st.ChildRounding = 5; st.PopupRounding = 5; st.GrabRounding = 3;
    st.SeparatorTextBorderSize = 1; st.SeparatorTextPadding = ImVec2(c ? 14.f : 18.f, c ? 2.f : 4.f); st.SeparatorTextAlign = ImVec2(0.f, 0.5f);
    ImVec4* col = st.Colors;
    // three text levels with enough contrast on the dark plate: primary, secondary (disabled), and the dim hints
    col[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.92f, 1);
    col[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.59f, 0.62f, 1);
    col[ImGuiCol_Header] = ImVec4(0.14f, 0.15f, 0.16f, 1);
}

bool collapsingHeader(const char* label, ImGuiTreeNodeFlags flags) {
    Scoped f(fonts().semibold);
    return ImGui::CollapsingHeader(label, flags);
}

void separatorText(const char* label) {
    Scoped f(fonts().semibold);
    ImGui::SeparatorText(label);
}

bool beginTabItem(const char* label, bool* open, ImGuiTabItemFlags flags) {
    ImGuiTabBar* tb = ImGui::GetCurrentTabBar();
    const bool selected = tb && tb->SelectedTabId == ImGui::GetCurrentWindow()->GetID(label);
    Scoped f(selected ? fonts().semibold : fonts().medium);
    return ImGui::BeginTabItem(label, open, flags);
}

} // namespace typo
