// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #104: a pane whose content ends with SetCursorScreenPos and no item made ImGui report "extend window/parent
// boundaries" (an assert in a Debug build). ui::closeCursorMove() before EndChild must keep that report away and grow the pane.
#include "../../app/ui/setpos_guard.h"
#include <cstdio>
#include <cstring>

static int fails = 0, errors = 0;
static void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
static void onError(ImGuiContext*, void*, const char* msg) { if (strstr(msg, "SetCursorPos")) errors++; }

// one frame with a child that ends with a bare cursor move; returns the child's content height
static float frame(bool guard) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    ImGui::Begin("##w", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("##pane", ImVec2(300, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("line");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 40));   // as mode bodies do after drawing with the draw list
    if (guard) ui::closeCursorMove();
    const float h = ImGui::GetCurrentWindowRead()->DC.CursorMaxPos.y - ImGui::GetCurrentWindowRead()->DC.CursorStartPos.y;
    ImGui::EndChild();
    // a pane that ends on an item: the guard must not add anything
    ImGui::BeginChild("##plain", ImVec2(300, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("line");
    const float before = ImGui::GetCurrentWindowRead()->DC.CursorMaxPos.y;
    ui::closeCursorMove();
    check(ImGui::GetCurrentWindowRead()->DC.CursorMaxPos.y == before, "guard changes a pane that ends on an item");
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
    return h;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigErrorRecoveryEnableAssert = false;   // the check under test reports through the callback instead of aborting
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::GetCurrentContext()->ErrorCallback = onError;
    unsigned char* px; int tw, th;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);

    frame(false);
    check(errors > 0, "without the guard ImGui reports the cursor move (the test no longer exercises #104)");
    errors = 0;
    const float h = frame(true);
    check(errors == 0, "with the guard ImGui still reports the cursor move (#104)");
    check(h >= 40.f, "the pane does not grow to the moved cursor");

    ImGui::DestroyContext();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
