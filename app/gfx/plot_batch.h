// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Many equal filled dots in one draw-list reservation (constellations and scatter plots draw thousands per frame).
// ImGui's AddCircleFilled(p, r, col, 8) builds the same 16 vertices (the anti-aliased octagon: inner ring and a faded outer ring) and 66
// indices for every dot, through its own path walk and one PrimReserve each. The shape only depends on r and col, never on where the dot
// is, so it is built once by ImGui itself (so the picture is identical) and then copied to every centre: a translation only.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <vector>

namespace plt {

class DotBatch {
public:
    // Adds the dots at pts[0..n) (screen pixels). Same pixels as n calls of dl->AddCircleFilled(p, r, col, 8).
    void draw(ImDrawList* dl, const ImVec2* pts, int n, float r, ImU32 col) {
        if (n <= 0) return;
        if (r != r_ || col != col_ || dl->Flags != flags_ || vtx_.empty()) build(dl, r, col);
        if (vtx_.empty()) { for (int i = 0; i < n; i++) dl->AddCircleFilled(pts[i], r, col, 8); return; }   // no template: the slow way
        const int nv = (int)vtx_.size(), ni = (int)idx_.size();
        const int chunk = 60000 / nv;   // 16-bit indices: keep one reservation under 64k vertices
        for (int s = 0; s < n; s += chunk) {
            const int m = std::min(chunk, n - s);
            dl->PrimReserve(m * ni, m * nv);
            ImDrawVert* v = dl->_VtxWritePtr;
            ImDrawIdx* ix = dl->_IdxWritePtr;
            unsigned base = dl->_VtxCurrentIdx;
            for (int k = 0; k < m; k++, base += (unsigned)nv) {
                const ImVec2 c = pts[s + k];
                for (int j = 0; j < nv; j++, v++) { v->pos = ImVec2(c.x + vtx_[j].pos.x, c.y + vtx_[j].pos.y); v->uv = vtx_[j].uv; v->col = vtx_[j].col; }
                for (int j = 0; j < ni; j++) *ix++ = (ImDrawIdx)(base + idx_[j]);
            }
            dl->_VtxWritePtr = v; dl->_IdxWritePtr = ix; dl->_VtxCurrentIdx = base;
        }
    }

private:
    float r_ = -1; ImU32 col_ = 0; ImDrawListFlags flags_ = 0;
    std::vector<ImDrawVert> vtx_;
    std::vector<ImDrawIdx> idx_;
    void build(ImDrawList* dl, float r, ImU32 col) {
        r_ = r; col_ = col; flags_ = dl->Flags; vtx_.clear(); idx_.clear();
        ImDrawList tmp(dl->_Data);   // same shared data (circle tables, white pixel) and flags (anti-aliasing) as the real list
        tmp._ResetForNewFrame();
        tmp.Flags = dl->Flags;
        tmp.PushClipRectFullScreen();
        tmp.AddCircleFilled(ImVec2(0, 0), r, col, 8);
        if (tmp.VtxBuffer.Size == 0 || tmp.VtxBuffer.Size > 64) return;
        for (const ImDrawCmd& c : tmp.CmdBuffer) if (c.VtxOffset != 0) return;
        vtx_.assign(tmp.VtxBuffer.Data, tmp.VtxBuffer.Data + tmp.VtxBuffer.Size);
        idx_.assign(tmp.IdxBuffer.Data, tmp.IdxBuffer.Data + tmp.IdxBuffer.Size);
    }
};

} // namespace plt
