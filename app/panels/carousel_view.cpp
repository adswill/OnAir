// The Carousels view of the TV data tab (DTMB and the DVB TV modes): the DSM-CC carousels with progress, their files, "Save all files", and a preview of the selected file:
// text as text, PNG/JPEG as a picture, MHEG-5 files as the parsed object tree and, for a Scene, a static drawing (experimental: no real broadcast stream was available;
// nothing is executed and the interactive links are not followed).
#include "../app.h"
#include "dect2/common/png.h"
#include "dect2/tv/mheg.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

namespace {
struct Pv {
    enum Kind { None, Text, Image, Mheg, Hex, Incomplete } kind = None;
    std::string key, text, error;
    std::unique_ptr<gfx::Image> img;
    int iw = 0, ih = 0;
    dect2::MhegRender render;
    bool haveRender = false;
    std::vector<std::unique_ptr<gfx::Image>> opImg;   // one texture per Bitmap draw op (null when it did not decode)
    std::vector<std::pair<int, int>> opImgSize;
};
struct State {
    std::map<int, std::vector<dect2::DsmccFile>> files;
    std::string sig;
    double lastFetch = -100;
    int pid = -1;
    std::string path;
    std::string saved;
    Pv pv;
    bool showTree = false;
};
State S;

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool decodeAny(const std::vector<uint8_t>& b, int& w, int& h, std::vector<uint32_t>& px) {
    std::vector<uint8_t> rgba;
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && dect2::decodePng(b.data(), b.size(), w, h, rgba) && w > 0 && h > 0) {
        px.resize((size_t)w * h);
        memcpy(px.data(), rgba.data(), px.size() * 4);
        return true;
    }
    if (b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8) {
        const std::string tmp = plat::cacheDir() + "/carousel_view.img";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f.write((const char*)b.data(), (std::streamsize)b.size()); }
        return plat::decodeImage(tmp, w, h, px) && w > 0 && h > 0 && w <= 8192 && h <= 8192;
    }
    return false;
}
std::unique_ptr<gfx::Image> makeImage(const std::vector<uint8_t>& b, int& w, int& h) {
    std::vector<uint32_t> px;
    if (!gGfx || !decodeAny(b, w, h, px)) return nullptr;
    std::unique_ptr<gfx::Image> im(gGfx->createImage(w, h, 0));
    if (im) im->update(0, 0, w, h, px.data());
    return im;
}
bool looksText(const std::vector<uint8_t>& d) {
    if (d.empty()) return false;
    size_t n = std::min<size_t>(d.size(), 4096), bad = 0;
    for (size_t i = 0; i < n; i++) if ((d[i] < 9 || (d[i] > 13 && d[i] < 32)) && d[i] != 27) bad++;
    return bad * 50 < n;
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

void buildPreview(const std::vector<dect2::DsmccFile>& all, const dect2::DsmccFile* f, bool complete, const std::string& key) {
    Pv& p = S.pv;
    p = Pv();
    p.key = key;
    if (!f || !complete) { p.kind = Pv::Incomplete; return; }
    const std::string ext = lower(f->path.substr(f->path.find_last_of('.') == std::string::npos ? f->path.size() : f->path.find_last_of('.') + 1));
    if (dect2::mhegLooksLikeApplication(f->data.data(), f->data.size())) {
        dect2::MhegParse r = dect2::mhegParse(f->data.data(), f->data.size());
        p.kind = Pv::Mheg;
        for (const auto& n : r.roots) p.text += dect2::mhegDump(n, 14);
        if (!r.error.empty()) p.error = r.error;
        if (!r.roots.empty() && r.roots[0].cls == 2 && r.roots[0].tag == 1) {
            p.render = dect2::mhegRenderScene(r.roots[0], [&](const std::string& ref, std::vector<uint8_t>& out) {
                const dect2::DsmccFile* g = dect2::mhegFindFile(all, ref);
                if (!g) return false;
                out = g->data;
                return true;
            });
            p.haveRender = true;
            for (const auto& op : p.render.ops) {
                int w = 0, h = 0;
                std::unique_ptr<gfx::Image> im;
                if (op.kind == dect2::MhegDrawOp::Bitmap && !op.image.empty()) im = makeImage(op.image, w, h);
                p.opImg.push_back(std::move(im));
                p.opImgSize.push_back({w, h});
            }
        }
        return;
    }
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || (f->data.size() > 4 && ((f->data[0] == 0x89 && f->data[1] == 'P') || (f->data[0] == 0xFF && f->data[1] == 0xD8)))) {
        p.img = makeImage(f->data, p.iw, p.ih);
        if (p.img) { p.kind = Pv::Image; return; }
    }
    if (looksText(f->data)) {
        p.kind = Pv::Text;
        p.text.assign((const char*)f->data.data(), std::min<size_t>(f->data.size(), 16384));
        for (char& c : p.text) if ((unsigned char)c < 9 || ((unsigned char)c > 13 && (unsigned char)c < 32)) c = '.';
        return;
    }
    p.kind = Pv::Hex;
    char b[16];
    for (size_t i = 0; i < f->data.size() && i < 512; i++) { snprintf(b, sizeof b, "%02X%s", f->data[i], (i % 16 == 15) ? "\n" : " "); p.text += b; }
}

ImU32 col(uint32_t rgba) { return IM_COL32(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255); }

void drawScene(const Pv& p) {
    const dect2::MhegRender& r = p.render;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sc = std::max(0.2f, std::min(1.5f, avail / (float)r.width));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(r.width * sc, r.height * sc));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + r.width * sc, o.y + r.height * sc), IM_COL32(24, 28, 36, 255));
    dl->PushClipRect(o, ImVec2(o.x + r.width * sc, o.y + r.height * sc), true);
    ImFont* font = ImGui::GetFont();
    for (size_t i = 0; i < r.ops.size(); i++) {
        const auto& op = r.ops[i];
        const ImVec2 a(o.x + op.x * sc, o.y + op.y * sc), b(o.x + (op.x + op.w) * sc, o.y + (op.y + op.h) * sc);
        if (op.kind == dect2::MhegDrawOp::Rectangle) {
            if ((op.fill & 255) && op.fill) dl->AddRectFilled(a, b, col(op.fill));
            if ((op.line & 255) && op.lineWidth > 0) dl->AddRect(a, b, col(op.line), 0, 0, std::max(1.f, op.lineWidth * sc));
        } else if (op.kind == dect2::MhegDrawOp::Bitmap) {
            if (i < p.opImg.size() && p.opImg[i]) dl->AddImage(p.opImg[i]->texture(), a, b);
            else { dl->AddRect(a, b, IM_COL32(200, 120, 60, 255)); dl->AddText(ImVec2(a.x + 3, a.y + 2), IM_COL32(200, 120, 60, 255), op.missing ? "bitmap not received" : "bitmap"); }
        } else {
            if (op.background & 255) dl->AddRectFilled(a, b, col(op.background));
            const float fs = std::max(8.f, (op.fontSize > 0 ? (float)op.fontSize : 24.f) * sc);
            const float wrapW = op.wrap ? (b.x - a.x) : 0.f;
            if (op.missing) { dl->AddText(font, fs, a, IM_COL32(200, 120, 60, 255), "text not received"); continue; }
            // lines: the text is split at line breaks; with wrapping a line is wrapped to the box
            std::vector<std::string> lines;
            size_t s = 0;
            while (s <= op.text.size()) { size_t e = op.text.find('\n', s); if (e == std::string::npos) e = op.text.size(); lines.push_back(op.text.substr(s, e - s)); s = e + 1; }
            std::vector<ImVec2> sz;
            float total = 0;
            for (auto& l : lines) { ImVec2 t = font->CalcTextSizeA(fs, 1e9f, wrapW, l.c_str(), l.c_str() + l.size()); sz.push_back(t); total += t.y; }
            float y = a.y;
            if (op.vJust == 2) y = b.y - total; else if (op.vJust == 3) y = a.y + ((b.y - a.y) - total) / 2;
            for (size_t k = 0; k < lines.size(); k++) {
                float x = a.x;
                if (op.hJust == 2) x = b.x - sz[k].x; else if (op.hJust == 3) x = a.x + ((b.x - a.x) - sz[k].x) / 2;
                dl->AddText(font, fs, ImVec2(x, y), col(op.textColour), lines[k].c_str(), lines[k].c_str() + lines[k].size(), wrapW);
                y += sz[k].y;
            }
        }
    }
    dl->PopClipRect();
    dl->AddRect(o, ImVec2(o.x + r.width * sc, o.y + r.height * sc), IM_COL32(120, 120, 120, 255));
}
} // namespace

void carouselView(App& a) {
    const TsSnapshot& ts = a.ts;
    if (!ts.carousels || ts.carousels->empty()) { ImGui::TextDisabled("%s", tr("no DSM-CC carousel found")); return; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::accent(), "%s", tr("EXPERIMENTAL: the file preview and the MHEG-5 scene drawing are untested on a real broadcast stream (no recording was available). Scripts and interactive links are not run or followed."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(tr("Save all files"))) {
        const std::string dir = plat::dataDir() + "/carousels";
        S.saved = std::to_string(a.engine.saveCarouselFiles(dir)) + tr(" files saved to ") + dir + tr("  (open index.html there for a gallery)");
    }
    if (!S.saved.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.saved.c_str()); }
    // the signature of what is complete, to refetch the file bodies when something new arrived (a copy of the files is taken under the engine's lock)
    std::string sig;
    for (const auto& c : *ts.carousels) { size_t done = 0; for (const auto& f : c.files) done += f.complete; sig += std::to_string(c.pid) + ":" + std::to_string(done) + ":" + std::to_string(c.files.size()) + ";"; }
    ImGui::SameLine();
    const bool manual = ImGui::SmallButton(tr("Refresh preview"));
    if (manual || (sig != S.sig && nowS() - S.lastFetch > 2.0)) { S.files = a.engine.carouselFiles(); S.sig = sig; S.lastFetch = nowS(); S.pv.key.clear(); }
    for (const auto& c : *ts.carousels) {
        const float frac = c.bytesTotal ? (float)((double)c.bytesDone / (double)c.bytesTotal) : 0.f;
        ImGui::TextColored(pal::accent(), "PID 0x%04X  %s", c.pid, c.objectCarousel ? (c.gatewayKnown ? tr("object carousel") : tr("object carousel (gateway not yet received)")) : tr("data carousel"));
        char ov[96];
        snprintf(ov, sizeof ov, "%.0f / %.0f KB, %zu files", (double)c.bytesDone / 1024, (double)c.bytesTotal / 1024, c.files.size());
        ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
        if (c.files.empty()) continue;
        ImGui::PushID(c.pid);
        if (ImGui::BeginTable("files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, std::min<float>(180.f, 24.f + 20.f * (float)c.files.size())))) {
            ImGui::TableSetupColumn(tr("Path"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90.f);
            ImGui::TableSetupColumn(tr("Complete"), ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < c.files.size() && i < 2000; i++) {
                const auto& f = c.files[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool sel = S.pid == c.pid && S.path == f.path;
                if (ImGui::Selectable((f.path + "##f" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) { S.pid = c.pid; S.path = f.path; S.pv.key.clear(); }
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)f.size);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.complete ? tr("yes") : tr("not yet"));
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    if (S.pid < 0 || S.path.empty()) { ImGui::TextDisabled("%s", tr("Select a file to preview it.")); return; }
    bool complete = false;
    for (const auto& c : *ts.carousels) if (c.pid == S.pid) for (const auto& f : c.files) if (f.path == S.path) complete = f.complete;
    const std::vector<dect2::DsmccFile> empty;
    auto it = S.files.find(S.pid);
    const std::vector<dect2::DsmccFile>& all = it == S.files.end() ? empty : it->second;
    const dect2::DsmccFile* sf = nullptr;
    for (const auto& f : all) if (f.path == S.path) sf = &f;
    const std::string key = std::to_string(S.pid) + "/" + S.path + "/" + std::to_string(sf ? sf->data.size() : 0) + (complete ? "c" : "i");
    if (S.pv.key != key) buildPreview(all, sf, complete && sf, key);
    const Pv& p = S.pv;
    ImGui::Separator();
    ImGui::TextColored(pal::accent(), "%s  (%s)", S.path.c_str(), sf ? (std::to_string(sf->data.size()) + " bytes").c_str() : "");
    switch (p.kind) {
    case Pv::Incomplete: ImGui::TextDisabled("%s", tr("This file is not complete yet (the carousel is still being received).")); break;
    case Pv::Image: if (p.img) ImGui::Image(p.img->texture(), ImVec2((float)p.iw, (float)p.ih)); break;
    case Pv::Text: case Pv::Hex: ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(p.text.c_str()); ImGui::PopTextWrapPos(); break;
    case Pv::Mheg:
        ImGui::TextDisabled("%s", tr("MHEG-5 file (ISO/IEC 13522-5). Interactive links not followed."));
        if (p.haveRender) {
            ImGui::Text(tr("Scene %dx%d: %zu drawn items, %d links and %d programs (not run)"), p.render.width, p.render.height, p.render.ops.size(), p.render.links, p.render.programs);
            drawScene(p);
            for (const auto& n : p.render.notes) ImGui::TextDisabled("%s", n.c_str());
        }
        ImGui::Checkbox(tr("Show the object tree"), &S.showTree);
        if (!p.error.empty()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%s %s", tr("parse stopped:"), p.error.c_str());
        if (S.showTree || !p.haveRender) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.text.c_str()); ImGui::PopFont(); }
        break;
    default: break;
    }
}
