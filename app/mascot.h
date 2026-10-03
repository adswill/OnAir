// Onny, the TV with a face: a small mascot drawn with the ImGui draw list (no image files). Used by the first-run tour.
#pragma once
#include "imgui.h"
#include <cmath>

namespace mascot {

enum Mood { Happy, Excited, Thinking, Sleepy };

struct Pose {
    double t = 0;              // animation time in seconds
    bool talking = false;      // the mouth moves
    Mood mood = Happy;
    bool wave = false;         // wave with the right arm
    bool point = false;        // point at `target` (screen position) with the nearer arm
    ImVec2 target{0, 0};
};

namespace detail {
inline ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
inline ImVec2 mul(ImVec2 a, float k) { return ImVec2(a.x * k, a.y * k); }
// a limb: two segments with a round joint and round ends
inline void limb(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, float th, ImU32 col) {
    dl->AddLine(a, b, col, th); dl->AddLine(b, c, col, th);
    dl->AddCircleFilled(a, th * 0.5f, col); dl->AddCircleFilled(b, th * 0.5f, col); dl->AddCircleFilled(c, th * 0.5f, col);
}
} // namespace detail

// `c` is the centre of the TV body, `k` the scale (1 = body 110 px wide).
inline void draw(ImDrawList* dl, ImVec2 c, float k, const Pose& p, ImU32 accent) {
    using namespace detail;
    const double t = p.t;
    const ImU32 casing = IM_COL32(40, 43, 47, 255), screen = IM_COL32(8, 12, 14, 255), limbCol = IM_COL32(176, 182, 188, 255), outline = IM_COL32(18, 19, 21, 255);
    const float bob = (float)std::sin(t * 2.2) * 2.f * k;
    c.y += bob;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * k, c.y + y * k); };

    // legs (behind the body): they step slowly, as if the little TV cannot stand still
    for (int s = -1; s <= 1; s += 2) {
        const float lift = (float)std::max(0.0, std::sin(t * 3.0 + (s > 0 ? 0 : 3.14159))) * 4.f;
        const ImVec2 hip = P(s * 20.f, 40.f), knee = P(s * 24.f, 56.f - lift * 0.5f), foot = P(s * 24.f, 72.f - lift);
        limb(dl, hip, knee, foot, 5.f * k, limbCol);
        dl->AddRectFilled(ImVec2(foot.x - 11 * k + (s > 0 ? 3 * k : -3 * k), foot.y - 2 * k), ImVec2(foot.x + 11 * k + (s > 0 ? 3 * k : -3 * k), foot.y + 6 * k), IM_COL32(222, 226, 230, 255), 4.f * k);
    }

    // antenna
    {
        const float w1 = (float)std::sin(t * 1.7) * 2.f, w2 = (float)std::sin(t * 1.3 + 1.f) * 2.f;
        const ImVec2 b1 = P(-8, -42), t1 = P(-30 + w1, -68), b2 = P(8, -42), t2 = P(28 + w2, -70);
        dl->AddLine(b1, t1, limbCol, 3.f * k); dl->AddLine(b2, t2, limbCol, 3.f * k);
        dl->AddCircleFilled(t1, 4.f * k, accent); dl->AddCircleFilled(t2, 4.f * k, accent);
    }

    // body
    dl->AddRectFilled(P(-55, -42), P(55, 42), casing, 14.f * k);
    dl->AddRect(P(-55, -42), P(55, 42), accent, 14.f * k, 0, std::max(1.5f, 2.f * k));
    // screen with a faint glow and scan lines
    dl->AddRectFilled(P(-47, -34), P(29, 34), screen, 10.f * k);
    for (float y = -32; y < 33; y += 4) dl->AddLine(P(-44, y), P(26, y), IM_COL32(255, 255, 255, 7), 1.f);
    dl->AddRect(P(-47, -34), P(29, 34), IM_COL32(70, 76, 82, 255), 10.f * k);
    // knobs and speaker slots
    for (int i = 0; i < 2; i++) { dl->AddCircleFilled(P(42, -18 + i * 18), 5.5f * k, IM_COL32(70, 74, 80, 255)); dl->AddLine(P(42, -21 + i * 18), P(42, -17 + i * 18), IM_COL32(190, 195, 200, 255), 1.5f); }
    for (int i = 0; i < 3; i++) dl->AddLine(P(36, 18 + i * 5), P(48, 18 + i * 5), IM_COL32(20, 22, 25, 255), 2.f * k);

    // face: the eyes follow the pointing target, and blink now and then
    float lookX = 0, lookY = 0;
    if (p.point) {
        const float dx = p.target.x - c.x, dy = p.target.y - c.y, d = std::sqrt(dx * dx + dy * dy) + 1e-3f;
        lookX = dx / d; lookY = dy / d;
    } else lookX = (float)std::sin(t * 0.6) * 0.5f;
    const double ph = std::fmod(t, 4.3);
    const bool blink = ph > 4.15;
    const float fx = -9.f + lookX * 3.f, fy = -2.f + lookY * 2.f;
    if (p.mood == Mood::Excited || p.mood == Mood::Happy) {
        if (p.mood == Mood::Excited || blink) {   // happy closed eyes ^ ^
            for (int s = -1; s <= 1; s += 2) {
                ImVec2 a = P(fx + s * 15 - 6, fy - 5), m = P(fx + s * 15, fy - 12), b = P(fx + s * 15 + 6, fy - 5);
                const ImVec2 pts[3] = {a, m, b}; dl->AddPolyline(pts, 3, accent, 0, 3.f * k);
            }
        } else {
            for (int s = -1; s <= 1; s += 2) dl->AddRectFilled(P(fx + s * 15 - 4.5f, fy - 15), P(fx + s * 15 + 4.5f, fy - 1), accent, 4.5f * k);
        }
    } else if (p.mood == Mood::Thinking) {
        dl->AddRectFilled(P(fx - 19.5f, fy - 15), P(fx - 10.5f, fy - 1), accent, 4.5f * k);
        dl->AddRectFilled(P(fx + 10.5f, fy - 12), P(fx + 19.5f, fy - 4), accent, 4.f * k);
    } else {   // sleepy: flat eyes
        for (int s = -1; s <= 1; s += 2) dl->AddLine(P(fx + s * 15 - 6, fy - 7), P(fx + s * 15 + 6, fy - 7), accent, 3.f * k);
    }
    // cheeks
    dl->AddCircleFilled(P(fx - 27, fy + 9), 4.5f * k, IM_COL32(230, 170, 90, 70));
    dl->AddCircleFilled(P(fx + 27, fy + 9), 4.5f * k, IM_COL32(230, 170, 90, 70));
    // mouth
    const float mx = fx + lookX * 1.5f, my = fy + 13.f;
    if (p.talking) {
        const float open = 3.f + (float)std::fabs(std::sin(t * 13.0)) * 8.f;
        dl->AddRectFilled(P(mx - 8, my - 2), P(mx + 8, my - 2 + open), accent, std::min(8.f, open * 0.5f) * k);
    } else if (p.mood == Mood::Thinking) {
        dl->AddCircle(P(mx + 4, my + 2), 3.5f * k, accent, 0, 2.5f * k);
    } else if (p.mood == Mood::Sleepy) {
        dl->AddLine(P(mx - 6, my + 2), P(mx + 6, my + 2), accent, 2.5f * k);
    } else {
        dl->PathArcTo(P(mx, my - 6), 11.f * k, 0.15f * 3.14159f, 0.85f * 3.14159f, 12);
        dl->PathStroke(accent, 0, 3.f * k);
    }

    // arms
    const ImVec2 shL = P(-55, 6), shR = P(55, 6);
    const ImU32 hand = IM_COL32(236, 238, 240, 255);
    auto hang = [&](ImVec2 sh, int s, float swing) {
        const ImVec2 el = add(sh, ImVec2(s * 14.f * k + swing * k, 16.f * k)), hd = add(sh, ImVec2(s * 18.f * k + swing * k * 1.6f, 36.f * k));
        limb(dl, sh, el, hd, 5.f * k, limbCol);
        dl->AddCircleFilled(hd, 6.5f * k, hand); dl->AddCircle(hd, 6.5f * k, outline, 0, 1.f);
    };
    bool pointR = false, pointL = false;
    if (p.point) { if (p.target.x >= c.x) pointR = true; else pointL = true; }
    auto reach = [&](ImVec2 sh, int s) {
        const float dx = p.target.x - sh.x, dy = p.target.y - sh.y, d = std::sqrt(dx * dx + dy * dy) + 1e-3f;
        const ImVec2 dir(dx / d, dy / d);
        const float len1 = 20.f * k, len2 = 22.f * k + (float)std::sin(t * 6.0) * 1.5f * k;
        const ImVec2 el = add(sh, mul(dir, len1)), hd = add(el, mul(dir, len2));
        // bend the elbow a little away from the body
        const ImVec2 elb = add(el, ImVec2(0, 5.f * k + (float)s * 0));
        limb(dl, sh, elb, hd, 5.f * k, limbCol);
        dl->AddCircleFilled(hd, 6.5f * k, hand); dl->AddCircle(hd, 6.5f * k, outline, 0, 1.f);
        dl->AddLine(hd, add(hd, mul(dir, 9.f * k)), hand, 3.f * k);   // the pointing finger
    };
    const float swing = (float)std::sin(t * 2.2 + 1.f) * 1.5f;
    if (pointL) reach(shL, -1); else hang(shL, -1, swing);
    if (pointR) reach(shR, 1);
    else if (p.wave) {
        const float a = (float)std::sin(t * 9.0) * 0.45f;
        const ImVec2 el = add(shR, ImVec2(18.f * k, -6.f * k)), hd = add(el, ImVec2(std::sin(a) * 18.f * k, -std::cos(a) * 20.f * k));
        limb(dl, shR, el, hd, 5.f * k, limbCol);
        dl->AddCircleFilled(hd, 6.5f * k, hand); dl->AddCircle(hd, 6.5f * k, outline, 0, 1.f);
    } else hang(shR, 1, -swing);
}

} // namespace mascot
