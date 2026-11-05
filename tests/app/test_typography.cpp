// Chinese text must use one family at every size, even when system fallbacks overlap.
#include "typography.h"
#include "cjk_fonts.h"
#include "i18n/i18n.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <set>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void checkChinese(ImFont* font, const std::set<ImWchar>& catalog) {
    CHECK(font != nullptr);
    if (!font) return;
    for (ImWchar c : catalog) CHECK(font->IsGlyphInFont(c));
    for (float size : {11.f, 13.f, 14.f, 20.f, 32.f}) {
        for (float density : {1.f, 1.5f, 2.f}) {
            ImFontBaked* baked = font->GetFontBaked(size, density);
            float advance = 0;
            int source = -1;
            for (ImWchar c : {(ImWchar)0x4e2d, (ImWchar)0x4e0e, (ImWchar)0x9891, (ImWchar)0x4e1a, (ImWchar)0xff0c}) {
                ImFontGlyph* glyph = baked->FindGlyphNoFallback(c);
                CHECK(glyph != nullptr);
                if (!glyph) continue;
                if (source == -1) { source = glyph->SourceIdx; advance = glyph->AdvanceX; }
                CHECK((int)glyph->SourceIdx == source);
                CHECK(std::fabs(glyph->AdvanceX - advance) < 0.01f);
                CHECK(std::strstr(font->Sources[glyph->SourceIdx]->Name, "Noto Sans CJK SC") != nullptr);
            }
            CHECK(std::fabs(advance / size - 0.9f) < 0.01f);   // readable Chinese em within the UI line height
            // The same Chinese em width must be preserved in body, mono and title roles.
            static float emRatio = 0;
            if (emRatio == 0) emRatio = advance / size;
            CHECK(std::fabs(advance / size - emRatio) < 0.01f);
        }
    }
}

int main() {
    for (int i = 1; i < typo::kSizeCount; i++) CHECK(typo::kSizes[i] > typo::kSizes[i - 1]);
    std::set<ImWchar> catalog;
    std::ifstream in(DECT2_SOURCE_DIR "/app/i18n/zh.txt");
    CHECK(in.good());
    std::string line;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        for (const char* p = line.c_str() + tab + 1; *p;) {
            unsigned c = 0;
            const int n = ImTextCharFromUtf8(&c, p, nullptr);
            if (!n) break;
            p += n;
            if ((c >= 0x2e80 && c <= 0x9fff) || (c >= 0xff00 && c <= 0xffef)) catalog.insert((ImWchar)c);
        }
    }
    CHECK(!catalog.empty());
    // A fixed GB2312 fixture also covers dynamic Chinese text outside the UI catalog.
    std::ifstream gb(DECT2_SOURCE_DIR "/third_party/fonts/noto-cjk/gb2312.txt");
    CHECK(gb.good());
    std::string gbText((std::istreambuf_iterator<char>(gb)), {});
    size_t gbCount = 0;
    for (const char* p = gbText.c_str(); *p;) {
        unsigned c = 0;
        const int n = ImTextCharFromUtf8(&c, p, nullptr);
        if (!n) break;
        p += n;
        catalog.insert((ImWchar)c);
        gbCount++;
    }
    CHECK(gbCount == 7445);
    size_t fontBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(DECT2_SOURCE_DIR "/third_party/fonts/noto-cjk"))
        if (entry.path().extension() == ".otf") fontBytes += entry.file_size();
    CHECK(fontBytes <= 10u * 1024u * 1024u);
    printf("CJK resources: %zu bytes\n", fontBytes);
    // SC glyphs must not override Japanese/Korean system fallbacks, including after a language switch.
    for (typo::Variant variant : {typo::Classic, typo::Clean, typo::Compact}) {
        typo::setVariant(variant);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(300, 200);
        for (const char* language : {"ja-JP", "zh_CN.UTF-8", "ko-KR", "en", "zh-Hans", "ja"}) {
            i18n::set(language);
            const bool chinese = !std::strcmp(i18n::current(), "zh");
            io.Fonts->Clear();
            ImFont* font = nullptr;
            if (variant == typo::Classic) font = typo::loadEmbeddedMono(io, 13.f, nullptr);
            else { CHECK(typo::loadFonts(io, nullptr)); font = typo::fonts().regular; }
            for (ImFont* role : io.Fonts->Fonts) {
                CHECK(role->Sources.Size == (chinese ? 2 : 1));
                CHECK((role->Sources[0]->GlyphExcludeRanges != nullptr) == chinese);
                CHECK(role->IsGlyphInFont(0x4e2d) == chinese);
            }
            CHECK(font != nullptr);
            ImGui::NewFrame();
            ImGui::Begin("Language switch");
            ImGui::TextUnformatted("A 中 時 한");
            ImGui::End();
            ImGui::Render();
        }
        ImGui::DestroyContext();
    }
    i18n::set("zh");
    for (typo::Variant variant : {typo::Classic, typo::Clean, typo::Compact}) {
        typo::setVariant(variant);
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        if (variant == typo::Classic) {
            ImFont* font = typo::loadEmbeddedMono(io, 13.f, nullptr);
            io.Fonts->Build();
            checkChinese(font, catalog);
        } else {
            CHECK(typo::loadFonts(io, nullptr));   // no OS font installed: bundled Chinese must still work
            io.Fonts->Build();
            const typo::Fonts& f = typo::fonts();
            for (ImFont* font : {f.regular, f.medium, f.semibold, f.mono, f.monoBold}) {
                checkChinese(font, catalog);
                for (ImWchar c : {(ImWchar)'A', (ImWchar)0x0105, (ImWchar)0x0151, (ImWchar)0x0416, (ImWchar)0x03a9, (ImWchar)0x00b0})
                    CHECK(font->IsGlyphInFont(c));
            }
        }
        ImGui::DestroyContext();
    }
    printf("typography: %zu Chinese/catalog characters, %d failures\n", catalog.size(), fails);
    return fails != 0;
}
