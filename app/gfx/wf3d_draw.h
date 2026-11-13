// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 3D waterfall's cached mesh (wf3d.h) into an ImDrawList: a copy of the screen-space vertices and the indices, chunk by chunk (each
// under 65536 vertices, so the 16-bit indices work; ImGui moves the vertex offset between them). No projection here: when nothing changed
// a frame costs only this copy. Shared by the app (wf3d_plot.cpp) and the benchmark in tests/app/test_wf3d.cpp.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"   // ImDrawListSharedData (the white pixel of the font texture)
#include "wf3d.h"

namespace w3d {

// ox, oy: added to every vertex (the mesh is built relative to the plot's corner, so moving the window does not rebuild it)
inline void drawMesh(ImDrawList* dl, const Mesh& m, float ox, float oy) {
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    for (const Chunk& c : m.chunks) {
        if (c.ni <= 0) continue;
        dl->PrimReserve(c.ni, c.nv);
        const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        ImDrawVert* vw = dl->_VtxWritePtr;
        const Vtx* src = m.v.data() + c.v0;
        for (int i = 0; i < c.nv; i++) { vw[i].pos = ImVec2(src[i].x + ox, src[i].y + oy); vw[i].uv = uv; vw[i].col = src[i].col; }
        ImDrawIdx* iw = dl->_IdxWritePtr;
        const uint16_t* is = m.idx.data() + c.i0;
        for (int i = 0; i < c.ni; i++) iw[i] = (ImDrawIdx)(base + is[i]);
        dl->_VtxWritePtr += c.nv; dl->_IdxWritePtr += c.ni; dl->_VtxCurrentIdx += (unsigned)c.nv;
    }
}

// the peak-hold ridge (pairs of points from peakLine, NaN breaks the line)
inline void drawRidge(ImDrawList* dl, const std::vector<float>& xy, float ox, float oy, ImU32 col, float thick) {
    const int n = (int)xy.size() / 2;
    std::vector<ImVec2> run;
    run.reserve((size_t)n);
    auto flush = [&] { if (run.size() >= 2) dl->AddPolyline(run.data(), (int)run.size(), col, 0, thick); run.clear(); };
    for (int i = 0; i < n; i++) {
        if (!(xy[2 * i] == xy[2 * i])) { flush(); continue; }
        run.push_back(ImVec2(xy[2 * i] + ox, xy[2 * i + 1] + oy));
    }
    flush();
}

} // namespace w3d
