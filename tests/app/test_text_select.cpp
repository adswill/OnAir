// #344: text drawn with ImGui::Text and friends can be selected with the mouse and copied: drag over a part of it, Ctrl+C copies that part; Ctrl+C with the pointer
// over an unselected text copies all of it; a wrapped text is selected line by line. Headless: a context, no window, the input events are fed by hand.
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string gClip;
static void setClip(ImGuiContext*, const char* t) { gClip = t; }
static const char* getClip(ImGuiContext*) { return gClip.c_str(); }

static const char* gText = "hello world";
static float gWrap = 0;   // > 0: PushTextWrapPos
static ImVec2 gOrigin;
static void frame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 300));
    ImGui::Begin("w", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    gOrigin = ImGui::GetCursorScreenPos();
    if (gWrap > 0) ImGui::PushTextWrapPos(gOrigin.x - ImGui::GetWindowPos().x + gWrap);
    ImGui::TextUnformatted(gText);
    if (gWrap > 0) ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::Render();
}
static void frames(int n = 2) { for (int i = 0; i < n; i++) frame(); }
static void mouse(float x, float y, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
}
static void ctrlC() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    frame();
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
}
static float width(const char* a, const char* b) { return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), 1e9f, 0.f, a, b).x; }

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.ConfigMacOSXBehaviors = false;   // Ctrl is Ctrl
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClip;
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = getClip;
    frames(3);
    const float lh = ImGui::GetFontSize();
    {   // drag over "hello", Ctrl+C: only that part
        gText = "hello world"; gWrap = 0;
        frames();
        const float x0 = gOrigin.x + 0.5f, y = gOrigin.y + lh * 0.5f;
        mouse(x0, y, true); frames();
        mouse(gOrigin.x + width(gText, gText + 5) + 1, y, true); frames();
        mouse(gOrigin.x + width(gText, gText + 5) + 1, y, false); frames();
        gClip.clear();
        ctrlC();
        CHECK(gClip == "hello", "selected part: '%s'", gClip.c_str());
        // a click elsewhere drops the selection; Ctrl+C over the text then takes all of it
        mouse(300, 250, true); frames(); mouse(300, 250, false); frames();
        mouse(gOrigin.x + 3, y, false); frames();
        gClip.clear();
        ctrlC();
        CHECK(gClip == "hello world", "whole text: '%s'", gClip.c_str());
    }
    {   // a wrapped text of two lines: from the middle of the first word to the middle of the second line
        gText = "alpha beta gamma delta"; gWrap = width(gText, gText + 11) + 2;   // "alpha beta " fits, "gamma" starts line two
        frames();
        const float y0 = gOrigin.y + lh * 0.5f, y1 = gOrigin.y + lh * 1.5f;
        mouse(gOrigin.x + width(gText, gText + 2), y0, true); frames();
        mouse(gOrigin.x + width(gText + 11, gText + 14), y1, true); frames();
        mouse(gOrigin.x + width(gText + 11, gText + 14), y1, false); frames();
        gClip.clear();
        ctrlC();
        CHECK(gClip == "pha beta gam", "wrapped selection: '%s'", gClip.c_str());
    }
    ImGui::DestroyContext();
    printf(fails ? "text select: %d failures\n" : "text select: ok\n", fails);
    return fails != 0;
}
