// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Recordings: one window over every folder the program records into (recordings, audio, tv, clips, goes, dab, sstv, screenshots): IQ, audio,
// transport streams, pictures, decoded logs and SigMF. Columns, search, preview (audio plays, pictures show, IQ replays through the engine as the
// file source), rename, trim to exact samples, export / convert, delete to a trash with undo, tags and notes, the per-folder size budget and the
// automatic cleanup. The logic (scan, search, cleanup rule, trim, trash) is core/src/sched/recmgr.cpp and is tested without a window; tags and
// notes live in recindex.json in each folder, deleted files in <folder>/.trash.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "dect2/sched/recmgr.h"
#include "dect2/sched/sched.h"
#include "dect2/source/iq_file.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>

bool recordingOpenPath(App& a, const std::string& path, uint64_t firstSample, uint64_t endSample);   // recording_library_ui.cpp

namespace {
namespace rm = dect2::recmgr;
namespace fs = std::filesystem;

struct Root {
    std::string name, path;
    rm::Index idx;
    uint64_t bytes = 0;
    int count = 0;
    double budgetGb = 0;
    int maxDays = 0;
};

struct Player {   // a WAV (16-bit PCM or 32-bit float) played through its own audio output
    std::unique_ptr<dect2::AudioOut> out;
    FILE* f = nullptr;
    rm::WavInfo wi;
    uint64_t pos = 0;   // frames
    std::string path;
    bool playing = false;
    void stop() {
        playing = false;
        if (out) { out->flush(); out->stop(); out.reset(); }
        if (f) { fclose(f); f = nullptr; }
    }
    bool start(const std::string& p, std::string& err) {
        stop();
        if (!rm::readWavInfo(p, wi, err)) return false;
        if (!((wi.format == 1 && wi.bits == 16) || (wi.format == 3 && wi.bits == 32)) || wi.channels < 1 || wi.channels > 2) { err = "only 16-bit and float WAV files play here"; return false; }
        f = dect2::u8fopen(p, "rb");
        if (!f) { err = "cannot read the file"; return false; }
        out = std::make_unique<dect2::AudioOut>();
        if (!out->start((int)wi.rate)) { err = "no audio output"; stop(); return false; }
        path = p; pos = 0; playing = true;
        seek(0);
        return true;
    }
    void seek(uint64_t frame) {
        if (!f) return;
        pos = std::min<uint64_t>(frame, wi.frames());
        fseek(f, (long)(wi.dataOffset + pos * (wi.bits / 8) * wi.channels), SEEK_SET);
        if (out) out->flush();
    }
    void pump() {   // keeps about half a second queued
        if (!playing || !f || !out) return;
        const int want = (int)wi.rate / 2;
        while (out->bufferedFrames() < want) {
            const uint64_t left = wi.frames() - pos;
            if (left == 0) { if (out->bufferedFrames() == 0) playing = false; return; }
            const int n = (int)std::min<uint64_t>(left, 4096);
            const int fb = (wi.bits / 8) * wi.channels;
            std::vector<unsigned char> raw((size_t)n * fb);
            const size_t got = fread(raw.data(), fb, (size_t)n, f);
            if (got == 0) { pos = wi.frames(); return; }
            std::vector<float> st(got * 2);
            for (size_t i = 0; i < got; i++) {
                float l, r;
                if (wi.bits == 16) {
                    const int16_t* s = (const int16_t*)raw.data();
                    l = s[i * wi.channels] / 32768.f; r = wi.channels == 2 ? s[i * 2 + 1] / 32768.f : l;
                } else {
                    const float* s = (const float*)raw.data();
                    l = s[i * wi.channels]; r = wi.channels == 2 ? s[i * 2 + 1] : l;
                }
                st[i * 2] = l; st[i * 2 + 1] = r;
            }
            out->write(st.data(), (int)got);
            pos += got;
        }
    }
};

struct Pic { gfx::Image* tex = nullptr; int w = 0, h = 0; std::string path; };

struct State {
    bool loaded = false;
    std::vector<Root> roots;
    std::vector<rm::Entry> all, rows;
    char query[128] = "";
    int kindFilter = -1, rootFilter = -1;
    rm::Column sortCol = rm::Column::Date;
    bool sortAsc = false;
    int sel = -1;   // index into rows
    std::string selPath;
    bool dirty = true;
    double lastScan = 0, lastAuto = 0;
    std::string msg;
    // edit buffers for the selection
    std::string bufFor;
    char tags[160] = "", notes[400] = "", rename[200] = "", outName[200] = "", trimStart[24] = "0", trimEnd[24] = "";
    double rateIn = 0;
    int convTo = 0;
    bool autoClean = false;
    std::string lastTrashRoot, lastTrashGroup;
    Player player;
    Pic pic;
    bool storageOpen = false;
};
State S;

std::string baseDir() { return plat::dataDir(); }

void loadRoots() {
    static const char* const names[] = {"recordings", "audio", "tv", "clips", "goes", "dab", "sstv", "screenshots", "images"};
    S.roots.clear();
    for (const char* n : names) {
        Root r; r.name = n; r.path = baseDir() + "/" + n;
        std::error_code ec;
        if (!fs::is_directory(dect2::u8file(r.path), ec)) { if (std::string(n) != "recordings") continue; fs::create_directories(dect2::u8file(r.path), ec); }
        r.idx.load(r.path + "/recindex.json");
        r.budgetGb = plat::prefs().getD((std::string("recmgr.gb.") + n).c_str(), 0);
        r.maxDays = (int)plat::prefs().getI((std::string("recmgr.days.") + n).c_str(), 0);
        S.roots.push_back(std::move(r));
    }
    const std::string extra = plat::prefs().getS("recmgr.extra", "");
    if (!extra.empty()) {
        Root r; r.name = "extra"; r.path = extra;
        r.idx.load(r.path + "/recindex.json");
        r.budgetGb = plat::prefs().getD("recmgr.gb.extra", 0);
        r.maxDays = (int)plat::prefs().getI("recmgr.days.extra", 0);
        S.roots.push_back(std::move(r));
    }
    S.autoClean = plat::prefs().getB("recmgr.auto", false);
}

Root* rootOf(const std::string& path) {
    Root* best = nullptr;
    for (Root& r : S.roots) if (path.compare(0, r.path.size(), r.path) == 0 && (!best || r.path.size() > best->path.size())) best = &r;
    return best;
}

void rescan() {
    S.all.clear();
    for (Root& r : S.roots) {
        auto v = rm::scan(r.path, r.idx);
        r.bytes = 0; r.count = (int)v.size();
        for (auto& e : v) { r.bytes += e.bytes; e.folder = r.name + (e.folder.empty() ? "" : "/" + e.folder); }
        S.all.insert(S.all.end(), v.begin(), v.end());
    }
    S.lastScan = std::time(nullptr);
    S.dirty = true;
}

void refilter() {
    S.rows.clear();
    for (const rm::Entry& e : S.all) {
        if (S.kindFilter >= 0 && (int)e.kind != S.kindFilter) continue;
        if (S.rootFilter >= 0 && S.rootFilter < (int)S.roots.size() && rootOf(e.path) != &S.roots[(size_t)S.rootFilter]) continue;
        if (!rm::matches(e, S.query)) continue;
        S.rows.push_back(e);
    }
    rm::sortEntries(S.rows, S.sortCol, S.sortAsc);
    S.sel = -1;
    for (size_t i = 0; i < S.rows.size(); i++) if (S.rows[i].path == S.selPath) S.sel = (int)i;
    S.dirty = false;
}

void saveIdx(Root& r) { std::string err; if (!r.idx.save(r.path + "/recindex.json", err)) S.msg = err; }

std::string sizeText(uint64_t b) {
    char t[32];
    if (b >= 1000000000ull) snprintf(t, sizeof t, "%.2f GB", b / 1e9);
    else if (b >= 1000000ull) snprintf(t, sizeof t, "%.1f MB", b / 1e6);
    else if (b >= 1000) snprintf(t, sizeof t, "%.0f kB", b / 1e3);
    else snprintf(t, sizeof t, "%llu B", (unsigned long long)b);
    return t;
}
std::string lenText(double s) {
    if (s <= 0) return "";
    const int t = (int)(s + 0.5);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
std::string dateText(int64_t t) {
    if (t <= 0) return "";
    const std::time_t tt = (std::time_t)t;
    std::tm l{};
#ifdef _WIN32
    localtime_s(&l, &tt);
#else
    localtime_r(&tt, &l);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &l);
    return b;
}
std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            size_t a = 0, b = cur.size();
            while (a < b && cur[a] == ' ') a++;
            while (b > a && cur[b - 1] == ' ') b--;
            if (b > a) v.push_back(cur.substr(a, b - a));
            cur.clear();
        } else cur += c;
    }
    return v;
}
std::string joinTags(const std::vector<std::string>& v) { std::string s; for (auto& t : v) { if (!s.empty()) s += ", "; s += t; } return s; }

std::string stemOf(const std::string& path) {   // the path without its extension (.sigmf-meta counts as one)
    std::string p = path;
    for (const char* e : {".sigmf-meta", ".sigmf-data"}) { const std::string x = e; if (p.size() > x.size() && p.compare(p.size() - x.size(), x.size(), x) == 0) return p.substr(0, p.size() - x.size()); }
    const size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
    return d != std::string::npos && (s == std::string::npos || d > s) ? p.substr(0, d) : p;
}

void trashEntry(const rm::Entry& e, int64_t now) {
    Root* r = rootOf(e.path);
    if (!r) return;
    std::string grp, err;
    if (rm::moveToTrash(e.files, r->path + "/.trash", now, grp, err)) {
        S.lastTrashRoot = r->path; S.lastTrashGroup = grp;
        const std::string key = rm::relKey(r->path, e.path);
        auto it = r->idx.meta.find(key);
        if (it != r->idx.meta.end()) { r->idx.meta.erase(it); saveIdx(*r); }   // the tags go with the file; Undo does not bring them back (the file does)
        S.msg = "moved to the trash: " + e.name;
    } else S.msg = "could not delete: " + err;
}

bool inUse(const rm::Entry& e, App& a) {
    const std::string p1 = a.engine.recordingStats().path;
    return (!p1.empty() && p1 == e.path) || e.mtime > (int64_t)std::time(nullptr) - 120;
}

// The automatic cleanup: each folder with a budget or an age limit. Deleted files go to the trash (kept 14 days), never straight off the disk.
void autoCleanup(App& a, bool report) {
    const int64_t now = std::time(nullptr);
    int removed = 0; uint64_t freed = 0;
    for (Root& r : S.roots) {
        if (r.budgetGb <= 0 && r.maxDays <= 0) continue;
        std::vector<rm::Entry> v = rm::scan(r.path, r.idx);
        rm::CleanupRule rule; rule.keepNewestBytes = (uint64_t)(r.budgetGb * 1e9); rule.olderThanDays = r.maxDays;
        const rm::CleanupPlan p = rm::planCleanup(v, rule, now);
        for (size_t i : p.remove) {
            if (inUse(v[i], a)) continue;
            std::string grp, err;
            if (rm::moveToTrash(v[i].files, r.path + "/.trash", now, grp, err)) { removed++; freed += v[i].bytes; }
        }
        rm::purgeTrash(r.path + "/.trash", 14, now);
    }
    if (removed) {
        a.engine.log("recordings cleanup: " + std::to_string(removed) + " file(s), " + sizeText(freed) + ", moved to the trash");
        rescan();
    }
    if (report) S.msg = removed ? "cleanup moved " + std::to_string(removed) + " recording(s) (" + sizeText(freed) + ") to the trash" : "nothing to clean up";
    S.lastAuto = (double)now;
}

void loadPic(const std::string& path) {
    if (S.pic.path == path) return;
    delete S.pic.tex; S.pic.tex = nullptr; S.pic.path = path; S.pic.w = S.pic.h = 0;
    std::vector<uint32_t> rgba; int w, h;
    if (!plat::decodeImage(path, w, h, rgba) || !gGfx) return;
    S.pic.tex = gGfx->createImage(w, h, 0xFF000000u);
    S.pic.tex->update(0, 0, w, h, rgba.data());
    S.pic.w = w; S.pic.h = h;
}

void syncBuffers(const rm::Entry& e) {
    if (S.bufFor == e.path) return;
    S.bufFor = e.path;
    snprintf(S.tags, sizeof S.tags, "%s", joinTags(e.tags).c_str());
    snprintf(S.notes, sizeof S.notes, "%s", e.notes.c_str());
    snprintf(S.rename, sizeof S.rename, "%s", fs::path(dect2::u8file(stemOf(e.path))).filename().string().c_str());
    snprintf(S.outName, sizeof S.outName, "%s_trim", S.rename);
    snprintf(S.trimStart, sizeof S.trimStart, "0");
    snprintf(S.trimEnd, sizeof S.trimEnd, "%.6g", e.seconds);
    S.rateIn = e.sampleRate;
}

void drawDetail(App& a, rm::Entry e) {
    syncBuffers(e);
    Root* root = rootOf(e.path);
    ImGui::SeparatorText(e.name.c_str());
    ImGui::TextDisabled("%s", e.path.c_str());
    if (e.sampleRate > 0 || e.freqHz > 0 || e.seconds > 0)
        ImGui::Text("%s  %s  %.3f MHz  %s  %s", rm::kindName(e.kind), e.mode.c_str(), e.freqHz / 1e6, lenText(e.seconds).c_str(), sizeText(e.bytes).c_str());

    // tags, notes, favourite
    bool fav = e.favourite;
    if (ImGui::Checkbox("Favourite (never cleaned up)", &fav) && root) {
        rm::Meta& m = root->idx.meta[rm::relKey(root->path, e.path)];
        m.favourite = fav; m.tags = e.tags; m.notes = e.notes; m.mode = e.mode; m.freqHz = e.freqHz;
        saveIdx(*root); rescan();
    }
    ImGui::SetNextItemWidth(320 * gUi);
    const bool tagsDone = ImGui::InputText("Tags (comma separated)", S.tags, sizeof S.tags) | false;
    (void)tagsDone;
    const bool tagsOut = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(420 * gUi);
    ImGui::InputText("Notes", S.notes, sizeof S.notes);
    const bool notesOut = ImGui::IsItemDeactivatedAfterEdit();
    if ((tagsOut || notesOut) && root) {
        rm::Meta& m = root->idx.meta[rm::relKey(root->path, e.path)];
        m.favourite = e.favourite; m.tags = splitTags(S.tags); m.notes = S.notes; m.mode = e.mode; m.freqHz = e.freqHz;
        saveIdx(*root);
        const std::string keep = S.selPath;
        rescan(); S.selPath = keep; S.bufFor.clear();
    }

    // preview
    if (e.kind == rm::Kind::Audio && rm::classify(e.path) == rm::Kind::Audio && e.path.size() > 4 && (e.path.substr(e.path.size() - 4) == ".wav" || e.path.substr(e.path.size() - 4) == ".WAV")) {
        S.player.pump();
        const bool mine = S.player.path == e.path && S.player.f;
        if (!mine || !S.player.playing) {
            if (ImGui::Button("Play")) { std::string err; if (!S.player.start(e.path, err)) S.msg = err; }
        } else if (ImGui::Button("Stop")) S.player.stop();
        if (mine && S.player.f) {
            ImGui::SameLine();
            float p = S.player.wi.frames() ? (float)((double)S.player.pos / (double)S.player.wi.frames()) : 0;
            ImGui::SetNextItemWidth(300 * gUi);
            if (ImGui::SliderFloat("##pos", &p, 0, 1, "")) S.player.seek((uint64_t)(p * (double)S.player.wi.frames()));
            ImGui::SameLine();
            ImGui::Text("%s / %s", lenText((double)S.player.pos / std::max(1u, S.player.wi.rate)).c_str(), lenText(e.seconds).c_str());
        }
    } else if (e.kind == rm::Kind::Image) {
        loadPic(e.path);
        if (S.pic.tex) {
            const float maxw = ImGui::GetContentRegionAvail().x, maxh = 260 * gUi;
            const float sc = std::min(1.f, std::min(maxw / S.pic.w, maxh / S.pic.h));
            ImGui::Image(S.pic.tex->texture(), ImVec2(S.pic.w * sc, S.pic.h * sc));
        } else ImGui::TextDisabled("%s", "This picture cannot be shown here.");
    }
    if (e.kind == rm::Kind::SigMF || e.kind == rm::Kind::Iq) {
        if (ImGui::Button("Open as IQ source")) { S.msg = recordingOpenPath(a, e.path, 0, 0) ? "set as the IQ file source: press Start" : "could not open it (stop the receiver first)"; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Replays the recording through the receiver as a file source");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) dect2::openUrl(e.path.substr(0, e.path.find_last_of("/\\")));

    // trim
    const bool iqKind = e.kind == rm::Kind::SigMF || (e.kind == rm::Kind::Iq && e.samples > 0);
    const bool wavKind = e.kind == rm::Kind::Audio && e.path.size() > 4 && (e.path.substr(e.path.size() - 4) == ".wav" || e.path.substr(e.path.size() - 4) == ".WAV");
    if ((iqKind || wavKind) && ImGui::CollapsingHeader("Trim")) {
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputText("from (s)", S.trimStart, sizeof S.trimStart);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputText("to (s)", S.trimEnd, sizeof S.trimEnd);
        double rate = e.sampleRate;
        if (iqKind && rate <= 0) { ImGui::SetNextItemWidth(130 * gUi); ImGui::InputDouble("sample rate (Hz)", &S.rateIn, 0, 0, "%.0f"); rate = S.rateIn; }
        const uint64_t total = iqKind ? e.samples : (uint64_t)std::llround(e.seconds * e.sampleRate);
        rm::SampleRange r; std::string err;
        const bool ok = rm::trimRange(atof(S.trimStart), atof(S.trimEnd), rate, total, r, err);
        if (ok) ImGui::TextDisabled("exact: samples %llu to %llu (%llu samples, %.6f s)", (unsigned long long)r.first, (unsigned long long)r.end, (unsigned long long)(r.end - r.first), (double)(r.end - r.first) / rate);
        else ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", err.c_str());
        ImGui::SetNextItemWidth(260 * gUi); ImGui::InputText("new file name", S.outName, sizeof S.outName);
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Save the cut as a new file")) {
            const std::string dir = e.path.substr(0, e.path.find_last_of("/\\") + 1);
            const std::string base = dir + S.outName;
            rm::SampleRange used; std::string er;
            bool done = false;
            if (e.kind == rm::Kind::SigMF) done = rm::trimSigmf(e.path, base, atof(S.trimStart), atof(S.trimEnd), &used, er);
            else if (wavKind) done = rm::trimWav(e.path, base + ".wav", atof(S.trimStart), atof(S.trimEnd), &used, er);
            else {
                const std::string x = e.path.substr(e.path.find_last_of('.'));
                int bps = x == ".cs8" || x == ".cu8" ? 2 : x == ".cf32" || x == ".fc32" ? 8 : 4;
                done = rm::trimRawIq(e.path, base + x, bps, r, er);
                used = r;
            }
            S.msg = done ? "saved " + base + " (samples " + std::to_string(used.first) + " to " + std::to_string(used.end) + ")" : "trim failed: " + er;
            if (done) rescan();
        }
        ImGui::EndDisabled();
    }

    // export / convert
    if (e.kind == rm::Kind::Iq && ImGui::CollapsingHeader("Export")) {
        const std::string x = e.path.find_last_of('.') == std::string::npos ? "" : e.path.substr(e.path.find_last_of('.'));
        const std::string dt = x == ".cs8" ? "ci8" : x == ".cu8" ? "cu8" : x == ".cf32" || x == ".fc32" ? "cf32_le" : x == ".ci16" ? "ci16_le" : "";
        if (!dt.empty()) {
            if (e.sampleRate <= 0) { ImGui::SetNextItemWidth(130 * gUi); ImGui::InputDouble("sample rate (Hz)##b", &S.rateIn, 0, 0, "%.0f"); }
            if (ImGui::Button("Write SigMF metadata beside it")) {
                std::string err;
                const double rate = e.sampleRate > 0 ? e.sampleRate : S.rateIn;
                if (rate <= 0) S.msg = "enter the sample rate first";
                else if (rm::writeSigmfSidecar(stemOf(e.path) + ".sigmf-meta", dt, rate, e.freqHz, e.mode, err)) { S.msg = "wrote the .sigmf-meta (the raw file is the data; rename it to .sigmf-data for other tools)"; rescan(); }
                else S.msg = err;
            }
            static const char* const fm[] = {"cs8", "cu8", "ci16", "cf32"};
            ImGui::SetNextItemWidth(100 * gUi);
            ImGui::Combo("convert to", &S.convTo, "8-bit signed (cs8)\0" "8-bit unsigned (cu8)\0" "16-bit (ci16)\0" "float (cf32)\0");
            ImGui::SameLine();
            if (ImGui::Button("Convert")) {
                const std::string src = x == ".cs8" ? "cs8" : x == ".cu8" ? "cu8" : x == ".ci16" ? "ci16" : "cf32";
                const std::string dst = stemOf(e.path) + "_" + fm[S.convTo] + "." + fm[S.convTo];
                std::string err;
                S.msg = rm::convertIq(e.path, src, dst, fm[S.convTo], err) ? "wrote " + dst : "convert failed: " + err;
                rescan();
            }
        } else ImGui::TextDisabled("%s", "This sample format cannot be converted here.");
    }

    // rename, delete
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText("##ren", S.rename, sizeof S.rename);
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        std::string np, err;
        const std::string oldKey = root ? rm::relKey(root->path, e.path) : "";
        if (S.player.path == e.path) S.player.stop();
        if (rm::renameEntry(e, S.rename, np, err)) {
            if (root) { root->idx.rename(oldKey, rm::relKey(root->path, np)); saveIdx(*root); }
            S.selPath = np; S.bufFor.clear(); rescan();
            S.msg = "renamed";
        } else S.msg = "rename failed: " + err;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.45f, 1));
    if (ImGui::Button("Delete")) {
        if (S.player.path == e.path) S.player.stop();
        if (!inUse(e, a)) { trashEntry(e, std::time(nullptr)); S.selPath.clear(); S.bufFor.clear(); rescan(); }
        else S.msg = "this recording is still being written";
    }
    ImGui::PopStyleColor();
}

void drawStorage(App& a) {
    uint64_t total = 0;
    for (Root& r : S.roots) total += r.bytes;
    ImGui::Text("All recording folders: %s", sizeText(total).c_str());
    if (ImGui::Checkbox("Clean up automatically (every 30 minutes; files go to the trash first)", &S.autoClean)) plat::prefs().setB("recmgr.auto", S.autoClean);
    if (ImGui::BeginTable("##stor", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Folder"); ImGui::TableSetupColumn("Files"); ImGui::TableSetupColumn("Used");
        ImGui::TableSetupColumn("Keep newest (GB, 0 = no limit)"); ImGui::TableSetupColumn("Delete older (days, 0 = never)"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < S.roots.size(); i++) {
            Root& r = S.roots[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.count);
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(sizeText(r.bytes).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::InputDouble("##gb", &r.budgetGb, 0, 0, "%.1f")) { r.budgetGb = std::max(0.0, r.budgetGb); plat::prefs().setD(("recmgr.gb." + r.name).c_str(), r.budgetGb); }
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::InputInt("##days", &r.maxDays, 0, 0)) { r.maxDays = std::max(0, r.maxDays); plat::prefs().setI(("recmgr.days." + r.name).c_str(), r.maxDays); }
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Open")) dect2::openUrl(r.path);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Clean up now")) autoCleanup(a, true);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", "Favourites and files tagged keep are never deleted. A file written in the last two minutes is left alone.");
    if (!S.lastTrashGroup.empty()) {
        if (ImGui::Button("Undo the last delete")) {
            std::string err;
            if (rm::restoreFromTrash(S.lastTrashRoot + "/.trash", S.lastTrashGroup, err)) { S.msg = "restored"; S.lastTrashGroup.clear(); rescan(); }
            else S.msg = "cannot restore: " + err;
        }
    }
    if (ImGui::TreeNode("Trash")) {
        for (Root& r : S.roots) {
            const auto items = rm::listTrash(r.path + "/.trash");
            if (items.empty()) continue;
            ImGui::TextDisabled("%s: %d file(s)", r.name.c_str(), (int)items.size());
            std::string lastGroup;
            for (const auto& t : items) {
                if (t.group == lastGroup) continue;
                lastGroup = t.group;
                ImGui::PushID(t.group.c_str());
                ImGui::TextUnformatted(fs::path(dect2::u8file(t.original)).filename().string().c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Restore")) { std::string err; S.msg = rm::restoreFromTrash(r.path + "/.trash", t.group, err) ? "restored" : "cannot restore: " + err; rescan(); ImGui::PopID(); break; }
                ImGui::PopID();
            }
            if (ImGui::SmallButton(("Empty the " + r.name + " trash").c_str())) { const uint64_t f = rm::purgeTrash(r.path + "/.trash", 0, std::time(nullptr)); S.msg = "freed " + sizeText(f); }
        }
        ImGui::TreePop();
    }
}

}  // namespace

void recmgrTick(App& a) {
    if (!S.loaded) { S.loaded = true; loadRoots(); }
    if (S.autoClean && std::time(nullptr) - (int64_t)S.lastAuto >= 1800) {
        if (S.lastAuto == 0) S.lastAuto = (double)std::time(nullptr);   // not at the very start of the program
        else autoCleanup(a, false);
    }
}

void recmgrWindow(App& a) {
    recmgrTick(a);
    if (!a.recmgrOpen) { if (S.player.f) S.player.stop(); return; }
    static bool wasOpen = false;
    if (!wasOpen) { wasOpen = true; rescan(); }
    firstWindowSize(1100 * gUi, 680 * gUi);
    if (!ImGui::Begin("Recordings###recmgr", &a.recmgrOpen)) { ImGui::End(); return; }
    if (!a.recmgrOpen) wasOpen = false;
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputTextWithHint("##q", "Search: words, tag:x kind:iq mode:fm fav >100MB <5min", S.query, sizeof S.query)) S.dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    {
        const char* kl = S.kindFilter < 0 ? "all kinds" : rm::kindName((rm::Kind)S.kindFilter);
        if (ImGui::BeginCombo("##kind", kl)) {
            if (ImGui::Selectable("all kinds", S.kindFilter < 0)) { S.kindFilter = -1; S.dirty = true; }
            for (int k = 0; k <= (int)rm::Kind::Other; k++) if (ImGui::Selectable(rm::kindName((rm::Kind)k), S.kindFilter == k)) { S.kindFilter = k; S.dirty = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    {
        const char* fl = S.rootFilter < 0 || S.rootFilter >= (int)S.roots.size() ? "all folders" : S.roots[(size_t)S.rootFilter].name.c_str();
        if (ImGui::BeginCombo("##root", fl)) {
            if (ImGui::Selectable("all folders", S.rootFilter < 0)) { S.rootFilter = -1; S.dirty = true; }
            for (size_t i = 0; i < S.roots.size(); i++) if (ImGui::Selectable(S.roots[i].name.c_str(), S.rootFilter == (int)i)) { S.rootFilter = (int)i; S.dirty = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) rescan();
    ImGui::SameLine();
    {
        uint64_t total = 0;
        for (Root& r : S.roots) total += r.bytes;
        ImGui::TextDisabled("%d recordings, %s", (int)S.all.size(), sizeText(total).c_str());
    }
    if (S.dirty) refilter();

    const float detailH = (S.sel >= 0 || S.storageOpen) ? 320 * gUi : 0;
    if (ImGui::BeginTable("##rec", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - detailH - (S.msg.empty() ? 0 : ImGui::GetTextLineHeightWithSpacing()) - 30 * gUi))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0, (ImGuiID)rm::Column::Name);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 0, (ImGuiID)rm::Column::Date);
        ImGui::TableSetupColumn("Kind", 0, 0, (ImGuiID)rm::Column::Kind);
        ImGui::TableSetupColumn("Mode", 0, 0, (ImGuiID)rm::Column::Mode);
        ImGui::TableSetupColumn("Frequency", 0, 0, (ImGuiID)rm::Column::Frequency);
        ImGui::TableSetupColumn("Length", 0, 0, (ImGuiID)rm::Column::Length);
        ImGui::TableSetupColumn("Size", 0, 0, (ImGuiID)rm::Column::Size);
        ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsDirty && sp->SpecsCount > 0) {
                S.sortCol = (rm::Column)sp->Specs[0].ColumnUserID; S.sortAsc = sp->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                sp->SpecsDirty = false; S.dirty = true;
            }
        }
        for (size_t i = 0; i < S.rows.size(); i++) {
            const rm::Entry& e = S.rows[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int)i);
            const std::string label = std::string(e.isProtected() ? "* " : "") + e.name;
            if (ImGui::Selectable(label.c_str(), (int)i == S.sel, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = (int)i; S.selPath = e.path; }
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.path.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(dateText(e.mtime).c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(rm::kindName(e.kind));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(e.mode.c_str());
            ImGui::TableSetColumnIndex(4); if (e.freqHz > 0) ImGui::Text("%.4f MHz", e.freqHz / 1e6);
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(lenText(e.seconds).c_str());
            ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(sizeText(e.bytes).c_str());
            ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(joinTags(e.tags).c_str());
            ImGui::TableSetColumnIndex(8); ImGui::TextUnformatted(e.notes.c_str());
            ImGui::TableSetColumnIndex(9); ImGui::TextUnformatted(e.folder.c_str());
        }
        ImGui::EndTable();
    }
    if (!S.msg.empty()) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", S.msg.c_str());
    if (ImGui::Button(S.storageOpen ? "Hide disk use and cleanup" : "Disk use, budgets and cleanup")) S.storageOpen = !S.storageOpen;
    if (S.storageOpen) {
        ImGui::BeginChild("##stor", ImVec2(0, 0), true);
        drawStorage(a);
        ImGui::EndChild();
    } else if (S.sel >= 0 && S.sel < (int)S.rows.size()) {
        ImGui::BeginChild("##detail", ImVec2(0, 0), true);
        drawDetail(a, S.rows[(size_t)S.sel]);
        ImGui::EndChild();
    }
    ImGui::End();
}
