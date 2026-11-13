// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DotBatch (app/gfx/plot_batch.h) must produce exactly the geometry of one AddCircleFilled(p, r, col, 8) per dot, in every chunk size,
// for several radii and colours: the constellations must look the same as before the batching.
#include "../../app/gfx/plot_batch.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }

int main() {
    ImGui::CreateContext();
    ImDrawListSharedData sd;
    sd.InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AllowVtxOffset;
    sd.TexUvWhitePixel = ImVec2(0.25f, 0.75f);
    sd.SetCircleTessellationMaxError(0.3f);
    sd.CurveTessellationTol = 1.25f;
    std::mt19937 rng(3);
    dect2::uniform_real_distribution<float> pos(-20.f, 600.f);
    for (float r : {1.5f, 2.24f, 3.0f, 5.5f})
        for (ImU32 col : {IM_COL32(40, 200, 120, 255), IM_COL32(255, 255, 255, 100)})
            for (int n : {1, 7, 5000, 9000}) {   // 9000 dots of 16 vertices: more than one reservation
                std::vector<ImVec2> pts((size_t)n);
                for (auto& p : pts) p = ImVec2{pos(rng), pos(rng)};
                ImDrawList ref(&sd), bat(&sd);
                for (ImDrawList* d : {&ref, &bat}) { d->_ResetForNewFrame(); d->PushClipRectFullScreen(); }
                for (const ImVec2& p : pts) ref.AddCircleFilled(p, r, col, 8);
                plt::DotBatch b;
                b.draw(&bat, pts.data(), n, r, col);
                // compare what is drawn, not the buffers: the two may start a new command (vertex offset) at different dots, so every index
                // is looked up through its command's vertex offset and the vertices compared as a stream of triangle corners
                auto corners = [](const ImDrawList& d) {
                    std::vector<ImDrawVert> out;
                    for (const ImDrawCmd& c : d.CmdBuffer)
                        for (unsigned i = 0; i < c.ElemCount; i++) out.push_back(d.VtxBuffer[(int)c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i)]]);
                    return out;
                };
                const std::vector<ImDrawVert> ca = corners(ref), cb = corners(bat);
                bool same = ca.size() == cb.size() && ref.IdxBuffer.Size == bat.IdxBuffer.Size && ref.VtxBuffer.Size == bat.VtxBuffer.Size;
                for (size_t i = 0; i < ca.size() && same; i++)
                    same = std::fabs(ca[i].pos.x - cb[i].pos.x) < 1e-4f && std::fabs(ca[i].pos.y - cb[i].pos.y) < 1e-4f && ca[i].col == cb[i].col && ca[i].uv.x == cb[i].uv.x && ca[i].uv.y == cb[i].uv.y;
                char msg[96]; snprintf(msg, sizeof msg, "r=%.2f col=%08x n=%d vtx %d/%d idx %d/%d cmds %d/%d", r, col, n, ref.VtxBuffer.Size, bat.VtxBuffer.Size, ref.IdxBuffer.Size, bat.IdxBuffer.Size, ref.CmdBuffer.Size, bat.CmdBuffer.Size);
                check(same, msg);
            }
    ImGui::DestroyContext();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
