// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > Light and High contrast: the finished draw lists are recoloured (lightness turned over, text kept readable).
// Split from main.cpp.
// Called by main() after ImGui::Render() for each frame while View > Light or High contrast is on.
#ifdef __linux__
#include <unistd.h>
#endif
#include "app.h"
#include "ui/contrast.h"
#include "dect2/source/iq_file.h"
#include "panels/gallery.h"
#include "dect2/engine/crash_report.h"
#include "platform/discord_rpc.h"
#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#include "../main_internal.h"

// View > Light: each colour the interface drew gets its lightness turned over (HSL: L -> 1 - L) with hue and saturation kept, so dark
// panes turn white, light text dark, and red, amber and green stay themselves. Done on the finished draw lists, so every panel of every
// mode follows without colours of its own. Only what is drawn from the font atlas (text, shapes, plots) is turned: pictures, map tiles
// and other textures keep their colours. Per channel c' = c + (255 - max - min), which keeps max - min and the hue.
// Light and High contrast also check the text: a glyph's vertices (not the solid shapes, whose texture coordinate is the atlas' white pixel)
// get a colour that holds the contrast ratio of contrast.h on that theme's panes (Light: 5:1 on white after the turn-over, so the amber and
// green status words and the dim grey do not fade; High contrast: 7:1 on black).
void lightenDrawData(ImDrawData* dd, bool light, bool highContrast) {
    if (!dd) return;
    static std::vector<unsigned char> keep;
    const ImVec2 wp = ImGui::GetFontTexUvWhitePixel();
    for (ImDrawList* dl : dd->CmdLists) {
        const int nv = dl->VtxBuffer.Size;
        keep.assign((size_t)nv, 0);
        for (const ImDrawCmd& c : dl->CmdBuffer) {
            if (c.UserCallback == nullptr && c.TexRef._TexData != nullptr) continue;   // the font atlas
            for (unsigned i = 0; i < c.ElemCount; i++) {
                const unsigned v = c.VtxOffset + dl->IdxBuffer[(int)(c.IdxOffset + i)];
                if (v < (unsigned)nv) keep[v] = 1;
            }
        }
        for (int i = 0; i < nv; i++) {
            if (keep[(size_t)i]) continue;
            ImU32& col = dl->VtxBuffer[i].col;
            const int r = (int)(col >> IM_COL32_R_SHIFT) & 255, g = (int)(col >> IM_COL32_G_SHIFT) & 255, b = (int)(col >> IM_COL32_B_SHIFT) & 255;
            const int d = light ? 255 - std::max(r, std::max(g, b)) - std::min(r, std::min(g, b)) : 0;
            auto cl = [](int x) { return (ImU32)std::clamp(x, 0, 255); };
            int nr = (int)cl(r + d), ng = (int)cl(g + d), nb = (int)cl(b + d);
            const ImDrawVert& vt = dl->VtxBuffer[i];
            const bool glyph = std::fabs(vt.uv.x - wp.x) > 1e-5f || std::fabs(vt.uv.y - wp.y) > 1e-5f;
            if (glyph) {   // text (and the textured lines): the legible colour, remembered for the next vertex of the same colour
                static int ir = -1, ig = -1, ib = -1, orr = 0, og = 0, ob = 0;
                static bool lastLight = false, lastHc = false;
                if (nr != ir || ng != ig || nb != ib || light != lastLight || highContrast != lastHc) {
                    const contrast::Rgb c{nr / 255.f, ng / 255.f, nb / 255.f};
                    const contrast::Rgb o = highContrast ? contrast::legibleOnDark(c) : contrast::legibleOnLight(c);
                    ir = nr; ig = ng; ib = nb; lastLight = light; lastHc = highContrast;
                    orr = (int)std::lround(o.r * 255); og = (int)std::lround(o.g * 255); ob = (int)std::lround(o.b * 255);
                }
                nr = orr; ng = og; nb = ob;
            }
            col = (col & IM_COL32_A_MASK) | (cl(nr) << IM_COL32_R_SHIFT) | (cl(ng) << IM_COL32_G_SHIFT) | (cl(nb) << IM_COL32_B_SHIFT);
        }
    }
}
