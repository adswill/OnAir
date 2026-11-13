// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir UI kit: small components on top of Dear ImGui (ImGui stays). Rules (docs/UI-KIT.md): toolbars are FlowRow, never SameLine chains;
// no literal colours or sizes in panels (theme_tokens.h); one component per job so a fix lands everywhere.
#pragma once
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include "theme_tokens.h"
#include "../theme.h"
#include "../icons.h"
#include <string>
#include <map>
struct App;

namespace kit {

// ---- tabs: a flat strip, the selected tab filled with the accent tint, a thin rule under the strip
bool beginTabs(const char* id);               // closes with endTabs() only when it returned true
bool tab(const char* label, bool forceSelect = false);   // closes with endTab() only when it returned true
void endTab();
void endTabs();

// ---- buttons and switches
bool button(const char* label, bool primary = false);
bool toggleButton(const char* label, bool* on);          // looks pressed (accent tint) while on
bool checkbox(const char* label, bool* on);
bool sw(const char* label, bool* on);                    // a switch: track and knob, label on the right
bool iconButton(const char* id, Ic icon, const char* tip, bool active);   // a square icon button for rails and bars
bool sliderValue(const char* label, float* v, float lo, float hi, const char* fmt = "%.1f", float width = 0);   // the value is written on the track
bool combo(const char* label, int* cur, const char* const* items, int n, float width = 0);

// ---- containers
// a tool-window header: title, fold arrow, optional right-side actions drawn between beginHeaderRight() and endHeaderRight()
bool panelHeader(const char* title, bool* open, const char* rightText = nullptr);   // returns true when the arrow / bar was clicked
void sectionHeader(const char* title);                   // a small heading with a 1 px rule; no gap above the first one
bool collapsingSection(const char* title, bool openByDefault = true);

// a row that wraps: items go on one line while they fit, then continue on the next. Nothing is ever clipped.
void beginFlow(float rightReserve = 0);                  // rightReserve: width kept at the end for a right-aligned group
void flowItem(float width);                              // call before each item with its width; wraps when it does not fit
void flowRight(float width);                             // the next item goes to the right edge of the first line (if there is room)
void endFlow();

bool beginTable(const char* id, int columns, const char* const* headers = nullptr);   // thin row separators, rowH rows
void endTable();

void statusBarBegin(float width);                        // a 1 px rule on top, bg1 fill, monospace text
void statusBarEnd();

void tooltip(const char* text, const char* keyHint = nullptr);   // after an item: shown on hover with an optional "[F6]" hint

void gallery(App& a, ImVec2 disp);                // kit_gallery.cpp: every component on one page (--kitgallery)

// a flow row of composite items (each item may draw several widgets): the width of every item is remembered from the last frame, so an
// item that would pass the right edge starts the next line. Nothing is clipped at any width.
class FlowAuto {
public:
    FlowAuto(float x0, float rightEdge, float gap) : x0_(x0), right_(rightEdge), gap_(gap) {}
    template <class F> void item(const char* id, F&& draw) {
        float& w = widths()[ImGui::GetID(id)];
        if (!first_) {
            if (lastRight_ + gap_ + w > right_) { ImGui::SetCursorScreenPos(ImVec2(x0_, lineBottom_ + gap_)); lineBottom_ = 0; }
            else ImGui::SameLine(0, gap_);
        }
        const float sx = ImGui::GetCursorScreenPos().x;
        draw();
        lastRight_ = ImGui::GetItemRectMax().x;
        lineBottom_ = std::max(lineBottom_, ImGui::GetItemRectMax().y);
        w = lastRight_ - sx;
        first_ = false;
    }
private:
    static std::map<ImGuiID, float>& widths() { static std::map<ImGuiID, float> m; return m; }
    float x0_, right_, gap_, lastRight_ = 0, lineBottom_ = 0;
    bool first_ = true;
};

// ---- meters
void meter(float frac, float width, float warnAt = 0.8f, float errorAt = 0.95f);   // a level bar: ok colour, then warn, then error
void labelValue(const char* label, const char* fmt, ...);        // a dim label and a value, same line

}  // namespace kit
