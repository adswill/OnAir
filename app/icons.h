// Small vector icons drawn with the ImGui draw list (no icon font needed, crisp at any size and DPI, coloured per use).
// Every icon is designed on a -1..1 square and scaled to `size` pixels.
#pragma once
#include "imgui.h"
#include "scale.h"
#include <cmath>

enum class Ic {
    Logo, Antenna, Play, Stop, Refresh, Chip, Gauge, Wave, Clock, Signal, Tv, Radio, Speaker, Mute, Scan, Compass, Grid, Layers, Chart,
    Lock, Warning, Check, Cross, File, Folder, Usb, Record, Sliders, Fullscreen, Popout, Info, Search, Star, Target, Echo, Bolt, Globe, Pulse,
    Book, Doc, Cpu, Down, Right, Gear, Subtitles, Link, Camera
};

namespace icons {

struct Pen {
    ImDrawList* dl; ImVec2 c; float h; ImU32 col; float th;
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * h, c.y + y * h); }
    void line(float x0, float y0, float x1, float y1) const { dl->AddLine(P(x0, y0), P(x1, y1), col, th); }
    void circle(float x, float y, float r) const { dl->AddCircle(P(x, y), r * h, col, 0, th); }
    void dot(float x, float y, float r) const { dl->AddCircleFilled(P(x, y), r * h, col); }
    void rect(float x0, float y0, float x1, float y1, float rr = 0.f) const { dl->AddRect(P(x0, y0), P(x1, y1), col, rr * h, 0, th); }
    void fillRect(float x0, float y0, float x1, float y1, float rr = 0.f) const { dl->AddRectFilled(P(x0, y0), P(x1, y1), col, rr * h); }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2) const { dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col); }
    void triLine(float x0, float y0, float x1, float y1, float x2, float y2) const { dl->AddTriangle(P(x0, y0), P(x1, y1), P(x2, y2), col, th); }
    void arc(float x, float y, float r, float a0, float a1) const { dl->PathArcTo(P(x, y), r * h, a0, a1, 14); dl->PathStroke(col, 0, th); }
    void poly(const float* xy, int n, bool closed = false) const {
        ImVec2 pts[16];
        for (int i = 0; i < n && i < 16; i++) pts[i] = P(xy[2 * i], xy[2 * i + 1]);
        dl->AddPolyline(pts, n, col, closed ? ImDrawFlags_Closed : 0, th);
    }
};

constexpr float kPi = 3.14159265f;

inline void draw(Ic id, ImVec2 centre, float size, ImU32 col, ImDrawList* dl = nullptr) {
    if (!dl) dl = ImGui::GetWindowDrawList();
    Pen p{dl, centre, size * 0.5f, col, std::max(1.f, size * 0.075f)};
    switch (id) {
    case Ic::Logo: p.rect(-0.8f, -0.8f, 0.8f, 0.8f); { const float w[] = {-0.8f, 0.f, -0.4f, -0.5f, 0.f, 0.5f, 0.4f, -0.5f, 0.8f, 0.f}; p.poly(w, 5); } break;
    case Ic::Antenna:
        p.line(0, -0.15f, 0, 0.9f); p.line(-0.5f, 0.9f, 0.5f, 0.9f); p.dot(0, -0.35f, 0.15f);
        p.arc(0, -0.35f, 0.5f, kPi * 1.15f, kPi * 1.85f); p.arc(0, -0.35f, 0.9f, kPi * 1.15f, kPi * 1.85f);
        p.arc(0, -0.35f, 0.5f, kPi * 0.15f, kPi * 0.85f); p.arc(0, -0.35f, 0.9f, kPi * 0.15f, kPi * 0.85f);
        break;
    case Ic::Play: p.tri(-0.55f, -0.8f, -0.55f, 0.8f, 0.85f, 0); break;
    case Ic::Stop: p.fillRect(-0.7f, -0.7f, 0.7f, 0.7f, 0.15f); break;
    case Ic::Refresh:
        p.arc(0, 0, 0.7f, kPi * 0.2f, kPi * 1.75f);
        p.tri(0.35f, -0.95f, 0.95f, -0.45f, 0.20f, -0.30f);
        break;
    case Ic::Chip: case Ic::Cpu:
        p.rect(-0.5f, -0.5f, 0.5f, 0.5f, 0.1f); p.fillRect(-0.2f, -0.2f, 0.2f, 0.2f);
        for (float t : {-0.28f, 0.28f}) { p.line(t, -0.5f, t, -0.9f); p.line(t, 0.5f, t, 0.9f); p.line(-0.5f, t, -0.9f, t); p.line(0.5f, t, 0.9f, t); }
        break;
    case Ic::Gauge:
        p.arc(0, 0.35f, 0.85f, kPi * 1.0f, kPi * 2.0f); p.line(-0.85f, 0.35f, 0.85f, 0.35f);
        p.line(0, 0.35f, 0.45f, -0.2f); p.dot(0, 0.35f, 0.12f);
        break;
    case Ic::Wave: {
        float pts[24];
        for (int i = 0; i < 12; i++) { const float x = -0.95f + 1.9f * i / 11.f; pts[2 * i] = x; pts[2 * i + 1] = -0.55f * std::sin(x * kPi * 1.5f); }
        p.poly(pts, 12);
        break; }
    case Ic::Clock: p.circle(0, 0, 0.85f); p.line(0, 0, 0, -0.5f); p.line(0, 0, 0.38f, 0.2f); break;
    case Ic::Signal:
        for (int i = 0; i < 4; i++) { const float x = -0.8f + i * 0.5f, hgt = 0.35f + 0.42f * i; p.fillRect(x, 0.85f - hgt * 1.0f, x + 0.3f, 0.85f, 0.06f); }
        break;
    case Ic::Tv:
        p.rect(-0.9f, -0.55f, 0.9f, 0.6f, 0.15f); p.line(-0.3f, 0.6f, -0.45f, 0.9f); p.line(0.3f, 0.6f, 0.45f, 0.9f);
        p.line(-0.15f, -0.55f, -0.5f, -0.9f); p.line(0.15f, -0.55f, 0.5f, -0.9f);
        break;
    case Ic::Radio:
        p.rect(-0.9f, -0.45f, 0.9f, 0.8f, 0.15f); p.circle(0.4f, 0.17f, 0.28f); p.line(-0.6f, 0.0f, -0.1f, 0.0f); p.line(-0.6f, 0.35f, -0.1f, 0.35f);
        p.line(-0.5f, -0.45f, 0.5f, -0.9f);
        break;
    case Ic::Speaker: case Ic::Mute: {
        const float pts[] = {-0.9f, -0.28f, -0.5f, -0.28f, -0.05f, -0.75f, -0.05f, 0.75f, -0.5f, 0.28f, -0.9f, 0.28f};
        p.poly(pts, 6, true);
        if (id == Ic::Speaker) { p.arc(-0.05f, 0, 0.5f, -kPi * 0.28f, kPi * 0.28f); p.arc(-0.05f, 0, 0.9f, -kPi * 0.28f, kPi * 0.28f); }
        else { p.line(0.3f, -0.35f, 0.9f, 0.35f); p.line(0.9f, -0.35f, 0.3f, 0.35f); }
        break; }
    case Ic::Scan:
        p.circle(0, 0, 0.85f); p.circle(0, 0, 0.45f); p.dot(0, 0, 0.1f); p.line(0, 0, 0.6f, -0.6f);
        break;
    case Ic::Compass:
        p.circle(0, 0, 0.88f); p.tri(0.0f, -0.62f, 0.2f, 0.0f, -0.2f, 0.0f);
        { ImU32 old = p.col; p.col = (old & 0x00FFFFFF) | (((old >> 24) * 45 / 100) << 24); p.tri(0.0f, 0.62f, 0.2f, 0.0f, -0.2f, 0.0f); }
        break;
    case Ic::Grid:
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) p.rect(-0.85f + i * 0.95f, -0.85f + j * 0.95f, -0.1f + i * 0.95f, -0.1f + j * 0.95f, 0.1f);
        break;
    case Ic::Layers: {
        const float top[] = {0, -0.85f, 0.9f, -0.35f, 0, 0.15f, -0.9f, -0.35f};
        p.poly(top, 4, true);
        const float mid[] = {-0.9f, 0.05f, 0, 0.55f, 0.9f, 0.05f}; p.poly(mid, 3);
        const float low[] = {-0.9f, 0.45f, 0, 0.95f, 0.9f, 0.45f}; p.poly(low, 3);
        break; }
    case Ic::Chart:
        p.line(-0.85f, -0.85f, -0.85f, 0.85f); p.line(-0.85f, 0.85f, 0.9f, 0.85f);
        { const float pts[] = {-0.6f, 0.4f, -0.15f, -0.1f, 0.25f, 0.2f, 0.8f, -0.6f}; p.poly(pts, 4); }
        break;
    case Ic::Lock:
        p.fillRect(-0.62f, -0.1f, 0.62f, 0.85f, 0.12f); p.arc(0, -0.12f, 0.38f, kPi, kPi * 2.0f); p.line(-0.38f, -0.12f, -0.38f, -0.1f); p.line(0.38f, -0.12f, 0.38f, -0.1f);
        break;
    case Ic::Warning:
        p.triLine(0, -0.88f, 0.95f, 0.78f, -0.95f, 0.78f); p.line(0, -0.25f, 0, 0.25f); p.dot(0, 0.52f, 0.07f);
        break;
    case Ic::Check: { const float pts[] = {-0.8f, 0.05f, -0.28f, 0.6f, 0.85f, -0.6f}; p.th *= 1.25f; p.poly(pts, 3); break; }
    case Ic::Cross: p.th *= 1.25f; p.line(-0.65f, -0.65f, 0.65f, 0.65f); p.line(0.65f, -0.65f, -0.65f, 0.65f); break;
    case Ic::File: case Ic::Doc: {
        const float pts[] = {-0.6f, -0.9f, 0.25f, -0.9f, 0.65f, -0.45f, 0.65f, 0.9f, -0.6f, 0.9f};
        p.poly(pts, 5, true); p.line(-0.3f, 0.1f, 0.35f, 0.1f); p.line(-0.3f, 0.45f, 0.35f, 0.45f);
        break; }
    case Ic::Folder: {
        const float pts[] = {-0.9f, -0.6f, -0.3f, -0.6f, -0.05f, -0.3f, 0.9f, -0.3f, 0.9f, 0.7f, -0.9f, 0.7f};
        p.poly(pts, 6, true);
        break; }
    case Ic::Usb:
        p.line(0, 0.85f, 0, -0.75f); p.dot(0, 0.85f, 0.13f); p.line(0, 0.15f, -0.55f, -0.25f); p.dot(-0.58f, -0.28f, 0.12f);
        p.line(0, -0.1f, 0.55f, -0.45f); p.fillRect(0.45f, -0.58f, 0.7f, -0.33f); p.tri(-0.2f, -0.75f, 0.2f, -0.75f, 0, -1.0f);
        break;
    case Ic::Record: p.dot(0, 0, 0.6f); break;
    case Ic::Sliders:
        for (int i = 0; i < 3; i++) { const float y = -0.6f + i * 0.6f; p.line(-0.9f, y, 0.9f, y); p.dot(-0.35f + (i == 1 ? 0.7f : 0.0f) + (i == 2 ? 0.1f : 0.0f), y, 0.2f); }
        break;
    case Ic::Fullscreen: {
        const float a[] = {-0.9f, -0.35f, -0.9f, -0.9f, -0.35f, -0.9f}; p.poly(a, 3);
        const float b[] = {0.35f, -0.9f, 0.9f, -0.9f, 0.9f, -0.35f}; p.poly(b, 3);
        const float c[] = {0.9f, 0.35f, 0.9f, 0.9f, 0.35f, 0.9f}; p.poly(c, 3);
        const float d[] = {-0.35f, 0.9f, -0.9f, 0.9f, -0.9f, 0.35f}; p.poly(d, 3);
        break; }
    case Ic::Popout:
        p.rect(-0.9f, -0.5f, 0.45f, 0.9f, 0.1f); p.line(0.1f, -0.1f, 0.9f, -0.9f);
        { const float a[] = {0.3f, -0.9f, 0.9f, -0.9f, 0.9f, -0.3f}; p.poly(a, 3); }
        break;
    case Ic::Info: p.circle(0, 0, 0.88f); p.line(0, -0.05f, 0, 0.45f); p.dot(0, -0.38f, 0.08f); break;
    case Ic::Search: p.circle(-0.2f, -0.2f, 0.58f); p.line(0.25f, 0.25f, 0.85f, 0.85f); break;
    case Ic::Star: {
        ImVec2 pts[10];
        for (int i = 0; i < 10; i++) { const float r = (i % 2 == 0) ? 0.95f : 0.4f, a = -kPi / 2 + i * kPi / 5; pts[i] = p.P(r * std::cos(a), r * std::sin(a)); }
        dl->AddConvexPolyFilled(pts, 10, col);
        break; }
    case Ic::Target: p.circle(0, 0, 0.88f); p.circle(0, 0, 0.5f); p.dot(0, 0, 0.14f); break;
    case Ic::Echo: p.line(-0.9f, 0.7f, 0.9f, 0.7f); p.line(-0.6f, 0.7f, -0.6f, -0.8f); p.line(0.3f, 0.7f, 0.3f, -0.15f); p.line(0.8f, 0.7f, 0.8f, 0.2f); break;
    case Ic::Bolt: { const float pts[] = {0.25f, -0.95f, -0.6f, 0.15f, -0.05f, 0.15f, -0.25f, 0.95f, 0.6f, -0.2f, 0.05f, -0.2f}; p.poly(pts, 6, true); break; }
    case Ic::Globe:
        p.circle(0, 0, 0.88f); p.line(-0.88f, 0, 0.88f, 0); p.arc(0, 0, 0.88f, -kPi * 0.5f, kPi * 0.5f);
        { dl->AddEllipse(p.P(0, 0), ImVec2(0.38f * p.h, 0.88f * p.h), col, 0, 0, p.th); }
        break;
    case Ic::Pulse: { const float pts[] = {-0.95f, 0.1f, -0.5f, 0.1f, -0.3f, -0.75f, 0.05f, 0.8f, 0.3f, 0.1f, 0.95f, 0.1f}; p.poly(pts, 6); break; }
    case Ic::Book: p.rect(-0.7f, -0.85f, 0.7f, 0.85f, 0.1f); p.line(-0.35f, -0.85f, -0.35f, 0.85f); p.line(-0.05f, -0.4f, 0.4f, -0.4f); break;
    case Ic::Down: { const float pts[] = {-0.6f, -0.3f, 0, 0.3f, 0.6f, -0.3f}; p.poly(pts, 3); break; }
    case Ic::Right: { const float pts[] = {-0.3f, -0.6f, 0.3f, 0, -0.3f, 0.6f}; p.poly(pts, 3); break; }
    case Ic::Gear:
        p.circle(0, 0, 0.5f); p.circle(0, 0, 0.16f);
        for (int i = 0; i < 8; i++) { const float a = i * kPi / 4; p.line(0.62f * std::cos(a), 0.62f * std::sin(a), 0.9f * std::cos(a), 0.9f * std::sin(a)); }
        break;
    case Ic::Subtitles: p.rect(-0.9f, -0.6f, 0.9f, 0.6f, 0.15f); p.line(-0.55f, -0.05f, -0.1f, -0.05f); p.line(0.1f, -0.05f, 0.55f, -0.05f); p.line(-0.55f, 0.3f, 0.2f, 0.3f); break;
    case Ic::Link:
        p.rect(-0.95f, -0.3f, 0.1f, 0.3f, 0.3f); p.rect(-0.1f, -0.3f, 0.95f, 0.3f, 0.3f);
        break;
    case Ic::Camera: p.rect(-0.9f, -0.5f, 0.9f, 0.7f, 0.15f); p.circle(0, 0.1f, 0.35f); p.fillRect(-0.4f, -0.8f, 0.0f, -0.5f); break;
    }
}

} // namespace icons

// ---- widgets -----------------------------------------------------------------------------------------------------

inline float iconSize() { return ImGui::GetFontSize() * 0.95f; }

// Icon inline with the text line, advances the cursor (use SameLine afterwards as usual)
inline void iconInline(Ic id, ImU32 col, float scale = 1.f) {
    if (pal::dev()) { ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight())); return; }   // the dev palette has no decorative icons
    const float s = iconSize() * scale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    icons::draw(id, ImVec2(p.x + s * 0.5f, p.y + lh * 0.5f), s, col);
    ImGui::Dummy(ImVec2(s, lh));
}

inline ImU32 iconDim() { return IM_COL32(120, 132, 148, 255); }
inline ImU32 iconAccent() { return pal::remap(IM_COL32(132, 158, 184, 255)); }

// Icon followed by a dim label ("icon  text") on the current line
inline void iconLabel(Ic id, const char* text, ImU32 col = 0) {
    iconInline(id, col ? col : iconDim());
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("%s", text);
}

// Rounded button with an icon and a label; returns true when clicked
inline bool iconButton(Ic id, const char* label, ImU32 bg, ImU32 bgHover, ImU32 fg = IM_COL32(245, 248, 250, 255), float padX = 10) {
    ImGui::PushID(label);
    const float s = pal::dev() ? 0.f : iconSize();
    const ImVec2 ts = label[0] ? ImGui::CalcTextSize(label) : ImVec2(0, 0);
    const ImVec2 sz(padX * 2 + s + (label[0] && s > 0 ? 6 : 0) + ts.x * (label[0] ? 1.f : 0.f), ImGui::GetFrameHeight());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##ib", sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (pal::dev()) {   // flat: an outline and the label, no fill, no icon
        const bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(255, 245, 220, 14));
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), hov ? IM_COL32(214, 200, 170, 255) : IM_COL32(60, 60, 56, 255));
        if (label[0]) dl->AddText(ImVec2(p.x + padX, p.y + (sz.y - ts.y) * 0.5f), IM_COL32(222, 218, 205, 255), label);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), pal::remap(ImGui::IsItemHovered() ? bgHover : bg), 0.f);
        icons::draw(id, ImVec2(p.x + padX + s * 0.5f, p.y + sz.y * 0.5f), s * 0.9f, fg);
        if (label[0]) dl->AddText(ImVec2(p.x + padX + s + 6, p.y + (sz.y - ts.y) * 0.5f), fg, label);
    }
    ImGui::PopID();
    return clicked;
}

// Square icon-only button, flat until hovered
inline bool iconFlat(Ic id, const char* tip, bool active = false) {
    ImGui::PushID(tip);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##if", ImVec2(h, h));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov || active) dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), active ? pal::remap(IM_COL32(24, 106, 166, 255)) : IM_COL32(46, 56, 68, 255), pal::rnd(5.f));
    icons::draw(id, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.62f, hov || active ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 182, 196, 255));
    if (hov && tip && tip[0]) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return clicked;
}

// Section title: icon, text and a hairline running to the right edge
inline void sectionHeader(Ic id, const char* text) {
    iconInline(id, iconAccent());
    ImGui::SameLine(0, 5 * gUi);
    ImGui::TextColored(pal::dev() ? ImVec4(0.66f, 0.64f, 0.60f, 1) : ImVec4(0.70f, 0.76f, 0.84f, 1), "%s", text);
    ImGui::SameLine(0, 8 * gUi);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(1.f, ImGui::GetContentRegionAvail().x), lh = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + lh * 0.5f), ImVec2(p.x + w, p.y + lh * 0.5f), pal::dev() ? IM_COL32(44, 44, 42, 255) : IM_COL32(44, 52, 63, 255));
    ImGui::Dummy(ImVec2(w, lh));
}

bool flowNext(float spacing);   // widgets.cpp
// Thin vertical divider between groups of controls on one line; a new line instead when the group after it does not fit (a narrow window)
inline void vSeparator() {
    const bool same = flowNext(9 * gUi);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    if (same) ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 3), ImVec2(p.x, p.y + h - 3), IM_COL32(52, 60, 72, 255));
    ImGui::Dummy(ImVec2(1, h));   // also at the start of a line: the group keeps its width from frame to frame
    ImGui::SameLine(0, 9 * gUi);
}
