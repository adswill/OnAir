// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The shared Enhance control (see enhance_ui.h): settings per mode in the preferences, the enhancement on the thread pool, the
// before/after view, the settings popup.
#include "../app.h"
#include "enhance_ui.h"
#include "../gfx/gfx.h"
#include "../platform/platform.h"
#include "dect2/common/threadpool.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

using namespace dect2::image;

namespace enhui {

namespace {

std::string keyOf(const std::string& source, const char* what) {
    std::string s = "enh_";
    for (char c : source) s += std::isalnum((unsigned char)c) ? c : '_';
    return s + "_" + what;
}

const char* const kPresetNames[] = {N_("Auto"), N_("Chart"), N_("Photo"), N_("Satellite"), N_("Colour SSTV"), N_("Custom")};
const char* const kPresetTips[] = {
    N_("Chooses for the picture: Chart for a black and white weather chart, Colour SSTV for a colour SSTV picture, Satellite for weather satellites, Photo for the rest"),
    N_("Weather charts: slant and line start fixed, broken lines repaired, specks removed, black and white levels tuned around the ink threshold, text sharpened. Never blurs thin lines"),
    N_("Photos and slides: specks removed, gentle edge-preserving denoise, auto levels, sharpening"),
    N_("Weather satellites: stripes evened out, broken lines repaired, specks removed, auto levels, local contrast (CLAHE), light sharpening"),
    N_("Colour SSTV: slant fixed, broken lines repaired, colour fringes (chroma offset) fixed, chroma noise smoothed, colour cast removed, auto levels"),
    N_("Your own settings (any slider in the settings switches to this)")};
const char* const kPalettes[] = {N_("No palette"), N_("Sea, land, cloud"), N_("Temperature (infrared)"), N_("Rainbow")};

struct Job {
    std::vector<uint8_t> in;
    int w = 0, h = 0;
    Params p;
    Preset preset = Preset::Auto;
    Kind kind = Kind::Generic;
    bool preview = false;
};

Params resolve(const ImageF& im, Kind k, Preset pr, const Params& custom, Preset* chosen) {
    if (pr == Preset::Custom) { if (chosen) *chosen = pr; return custom; }
    const Preset use = pr == Preset::Auto ? autoPreset(im, k) : pr;
    if (chosen) *chosen = use;
    return presetParams(use, k);
}

}  // namespace

// ---------------------------------------------------------------- Enhancer

void Enhancer::setSource(const std::string& source) {
    if (source == source_) return;
    source_ = source;
    kind_ = kindForSource(source);
    loadPrefs();
    on_ = autoNew_;
    gen_++;
    viewDirty_ = true;
}

void Enhancer::loadPrefs() {
    auto& pr = plat::prefs();
    autoNew_ = pr.getB(keyOf(source_, "auto").c_str(), false);
    preset_ = std::clamp((int)pr.getI(keyOf(source_, "preset").c_str(), 0), 0, kPresets - 1);
    custom_ = paramsFromString(pr.getS(keyOf(source_, "custom").c_str(), ""), presetParams(Preset::Photo, kind_));
}

void Enhancer::savePrefs() {
    auto& pr = plat::prefs();
    pr.setB(keyOf(source_, "auto").c_str(), autoNew_);
    pr.setI(keyOf(source_, "preset").c_str(), preset_);
    pr.setS(keyOf(source_, "custom").c_str(), paramsToString(custom_));
}

void Enhancer::feed(const uint8_t* rgba, int w, int h, bool finished) {
    if (!rgba || w <= 0 || h <= 0) return;
    raw_.assign(rgba, rgba + (size_t)w * h * 4);
    rw_ = w; rh_ = h; rawFinished_ = finished;
    gen_++;
    viewDirty_ = true;
    if (on_ && !job_.valid()) start();
}

void Enhancer::start() {
    if (raw_.empty()) return;
    auto j = std::make_shared<Job>();
    j->in = raw_; j->w = rw_; j->h = rh_;
    j->p = custom_; j->preset = (Preset)preset_; j->kind = kind_;
    j->preview = !rawFinished_ || (size_t)rw_ * rh_ > 8u * 1000 * 1000;   // still arriving, or very large (full disk): the cheap path
    const uint64_t g = gen_;
    job_ = dect2::ThreadPool::shared().submit([j, g]() {
        Result r;
        ImageF im = fromRgba(j->in.data(), j->w, j->h);
        Preset chosen;
        const Params p = resolve(im, j->kind, j->preset, j->p, &chosen);
        ImageF out = enhance(im, p, &r.rep, j->preview);
        r.rgba = toRgba(out); r.w = out.w; r.h = out.h; r.inW = j->w; r.inH = j->h; r.gen = g; r.preset = (int)chosen;
        return r;
    });
}

bool Enhancer::poll() {
    bool changed = false;
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Result r;
        try { r = job_.get(); } catch (...) { msg_ = "enhancement failed"; }
        job_ = std::future<Result>();
        if (!r.rgba.empty()) {
            enh_ = std::move(r.rgba); ew_ = r.w; eh_ = r.h; enhInW_ = r.inW; enhInH_ = r.inH; doneGen_ = r.gen; rep_ = r.rep; repPreset_ = r.preset;
            viewDirty_ = true;
            changed = on_;
        }
    }
    if (on_ && !job_.valid() && doneGen_ != gen_ && !raw_.empty()) start();
    return changed;
}

const std::vector<uint8_t>& Enhancer::view(int& w, int& h) {
    if (!on_ || hold_ || enh_.empty()) { w = rw_; h = rh_; return raw_; }
    if (!viewDirty_) { w = vw_; h = vh_; return view_; }
    viewDirty_ = false;
    const bool sameGeom = ew_ == enhInW_ && eh_ == enhInH_;
    if (sameGeom && ew_ == rw_) {
        // the raw picture may have grown since: the new lines raw under the enhanced ones
        vw_ = rw_; vh_ = rh_;
        view_ = raw_;
        const int rows = std::min(eh_, rh_);
        std::memcpy(view_.data(), enh_.data(), (size_t)rows * rw_ * 4);
    } else {
        vw_ = ew_; vh_ = eh_;
        view_ = enh_;
    }
    if (split_ > 0.f && !raw_.empty()) {
        // the before picture at the view size (nearest pixel), left of the split
        std::vector<uint8_t> before((size_t)vw_ * vh_ * 4);
        for (int y = 0; y < vh_; y++) {
            const int sy = std::min(rh_ - 1, (int)((int64_t)y * rh_ / vh_));
            for (int x = 0; x < vw_; x++) {
                const int sx = std::min(rw_ - 1, (int)((int64_t)x * rw_ / vw_));
                std::memcpy(&before[((size_t)y * vw_ + x) * 4], &raw_[((size_t)sy * rw_ + sx) * 4], 4);
            }
        }
        std::vector<uint8_t> out(view_.size());
        splitView(before.data(), view_.data(), vw_, vh_, split_, out.data());
        view_.swap(out);
    }
    w = vw_; h = vh_;
    return view_;
}

bool Enhancer::settings() {
    bool ch = false;
    // the first change of a slider takes the settings of the preset on show into Custom
    auto toCustom = [&]() {
        if (preset_ == (int)Preset::Custom) return;
        const Preset p = preset_ == (int)Preset::Auto ? (Preset)repPreset_ : (Preset)preset_;
        custom_ = presetParams(p == Preset::Auto ? Preset::Photo : p, kind_);
        preset_ = (int)Preset::Custom;
    };
    Params q = preset_ == (int)Preset::Custom ? custom_
                                              : presetParams(preset_ == (int)Preset::Auto ? (Preset)repPreset_ : (Preset)preset_, kind_);
    Params before = q;
    ImGui::PushItemWidth(170 * gUi);
    ImGui::SeparatorText(tr("Geometry"));
    ImGui::Checkbox(tr("Automatic slant"), &q.autoSlant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Measures the clock error from how the line starts drift down the picture and straightens it"));
    ImGui::SliderFloat(tr("Slant (px per line)"), &q.slant, -1.f, 1.f, "%.3f");
    ImGui::Checkbox(tr("Automatic line start"), &q.autoPhase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Finds the sync bar or margin and turns the picture round so it is not split in two"));
    ImGui::SliderInt(tr("Line start shift (px)"), &q.phase, -1000, 1000);
    ImGui::Checkbox(tr("Repair broken lines"), &q.repair);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Line jitter"), &q.registerRows);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lines up each line with the ones around it (timing jitter)"));
    const int iocs[] = {0, 288, 576};
    int ioc = q.ioc == 288 ? 1 : q.ioc == 576 ? 2 : 0;
    const char* iocNames[] = {tr("Width as received"), "IOC 288", "IOC 576"};
    if (ImGui::Combo(tr("Fax width"), &ioc, iocNames, 3)) q.ioc = iocs[ioc];
    ImGui::SeparatorText(tr("Noise"));
    ImGui::Checkbox(tr("Remove specks"), &q.impulse);
    if (q.impulse) { ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::SliderFloat("##ithr", &q.impulseThr, 0.05f, 0.6f, "%.2f"); }
    ImGui::Checkbox(tr("Even out stripes"), &q.destripe);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Even out bands"), &q.deband);
    ImGui::SliderInt(tr("Denoise"), &q.denoise, 0, 100);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Non-local means"), &q.nlm);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stronger and slower than the default bilateral filter (not used while a picture is still arriving)"));
    ImGui::Checkbox(tr("Deinterlace"), &q.deinterlace);
    ImGui::SeparatorText(tr("Tone"));
    ImGui::Checkbox(tr("Chart levels"), &q.chart);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Black ink and white paper found from the picture; the step between them made harder without making thin lines jagged"));
    if (q.chart) { ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::SliderFloat("##cst", &q.chartStrength, 0.f, 1.f, "%.2f"); }
    ImGui::Checkbox(tr("Auto levels"), &q.levels);
    if (q.levels) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ImGui::SliderFloat("##lo", &q.loPct, 0.f, 10.f, "%.1f%%");
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ImGui::SliderFloat("##hi", &q.hiPct, 90.f, 100.f, "%.1f%%");
    }
    ImGui::SliderFloat(tr("Gamma"), &q.gamma, 0.3f, 3.f, "%.2f");
    ImGui::SliderFloat(tr("Local contrast (CLAHE)"), &q.clahe, 0.f, 6.f, q.clahe < 1.f ? tr("off") : "%.1f");
    int pal = (int)q.palette;
    const char* pn[4] = {tr(kPalettes[0]), tr(kPalettes[1]), tr(kPalettes[2]), tr(kPalettes[3])};
    if (ImGui::Combo(tr("Palette"), &pal, pn, 4)) q.palette = (Palette)pal;
    ImGui::SeparatorText(tr("Detail and colour"));
    ImGui::SliderFloat(tr("Sharpen"), &q.sharpen, 0.f, 2.f, "%.2f");
    ImGui::SliderFloat(tr("Sharpen radius"), &q.sharpenRadius, 0.5f, 3.f, "%.1f px");
    ImGui::Checkbox(tr("Upscale x2"), &q.upscale);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lanczos, held to the range of the pixels around (no ringing); not used while a picture is still arriving"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Colour fringes"), &q.chroma);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Moves the colour back under the brightness (SSTV chroma offset) and smooths the colour noise more than the detail"));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Colour cast"), &q.greyWorld);
    ImGui::PopItemWidth();
    if (paramsToString(q) != paramsToString(before)) {
        toCustom();
        custom_ = q;
        ch = true;
    }
    return ch;
}

bool Enhancer::controls(const char* id) {
    ImGui::PushID(id);
    bool ch = false;
    if (ImGui::Checkbox(tr("Enhance"), &on_)) { ch = true; viewDirty_ = true; if (on_ && doneGen_ != gen_) start(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shows the picture enhanced. The picture as received is always kept and saved as it is"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!on_);
    ImGui::SetNextItemWidth(120 * gUi);
    std::string cur = tr(kPresetNames[preset_]);
    if (preset_ == (int)Preset::Auto && !enh_.empty()) cur += std::string(" (") + tr(kPresetNames[repPreset_]) + ")";
    if (ImGui::BeginCombo("##preset", cur.c_str())) {
        for (int i = 0; i < kPresets; i++) {
            if (ImGui::Selectable(tr(kPresetNames[i]), preset_ == i)) { preset_ = i; gen_++; ch = true; savePrefs(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(kPresetTips[i]));
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("##enhset");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Enhancement settings"));
    if (ImGui::BeginPopup("##enhset")) {
        if (settings()) { gen_++; ch = true; savePrefs(); }
        ImGui::Separator();
        if (!enh_.empty())
            ImGui::TextDisabled("%s: slant %+.3f px/line, line start %d, %d lines repaired, %d specks, %.0f ms", tr(kPresetNames[repPreset_]), rep_.slantPx, rep_.phase,
                                rep_.repaired, rep_.impulses, rep_.ms);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::SliderFloat("##split", &split_, 0.f, 1.f, split_ <= 0.f ? tr("after") : tr("before | after"))) { viewDirty_ = true; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Before and after: the part left of the line is the picture as received"));
    ImGui::SameLine();
    ImGui::SmallButton(tr("Hold to compare"));
    const bool held = ImGui::IsItemActive();
    if (held != hold_) { hold_ = held; ch = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save enhanced PNG..."))) {
        if (enh_.empty()) msg_ = "nothing enhanced yet";
        else {
            const std::string path = saveFileDialog(saveName_.c_str());
            if (!path.empty()) msg_ = gfx::writePng(path.c_str(), enh_.data(), ew_, eh_) ? "saved " + path : "could not write " + path;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Apply to all new images"), &autoNew_)) savePrefs();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", source_.c_str(), tr("new pictures start enhanced, and the gallery keeps an enhanced copy next to each one"));
    if (busy() && on_) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("enhancing...")); }
    else if (!msg_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", msg_.c_str()); }
    ImGui::PopID();
    if (ch) viewDirty_ = true;
    return ch;
}

// ---------------------------------------------------------------- LiveTex

gfx::Image* LiveTex::texture(gfx::Image* own) {
    if (e.poll()) dirty = true;
    if (!e.on()) return own;
    if (dirty && gGfx) {
        int w, h;
        const std::vector<uint8_t>& v = e.view(w, h);
        if (!v.empty() && w > 0 && h > 0) {
            if (!tex || tw != w || th != h) { delete tex; tex = gGfx->createImage(w, h, 0xFF000000u); tw = w; th = h; }
            if (tex) tex->update(0, 0, w, h, reinterpret_cast<const uint32_t*>(v.data()));
        }
        dirty = false;
    }
    return tex ? tex : own;
}

// ---------------------------------------------------------------- the gallery's enhanced copies

bool autoEnabled(const std::string& source) { return plat::prefs().getB(keyOf(source, "auto").c_str(), false); }

void autoEnhanceFile(const std::string& source, const uint8_t* rgba, int w, int h, const std::string& path) {
    if (!rgba || w <= 0 || h <= 0 || !autoEnabled(source)) return;
    auto& pr = plat::prefs();
    auto j = std::make_shared<Job>();
    j->kind = kindForSource(source);
    j->preset = (Preset)std::clamp((int)pr.getI(keyOf(source, "preset").c_str(), 0), 0, kPresets - 1);
    j->p = paramsFromString(pr.getS(keyOf(source, "custom").c_str(), ""), presetParams(Preset::Photo, j->kind));
    j->in.assign(rgba, rgba + (size_t)w * h * 4); j->w = w; j->h = h;
    // the same picture published again (it grew) before the last copy was written: only the newest one is kept
    static std::mutex m;
    static std::map<std::string, uint64_t> gens;
    uint64_t g;
    { std::lock_guard<std::mutex> l(m); g = ++gens[path]; }
    dect2::ThreadPool::shared().submit([j, path, g]() {
        ImageF im = fromRgba(j->in.data(), j->w, j->h);
        const Params p = resolve(im, j->kind, j->preset, j->p, nullptr);
        const ImageF out = enhance(im, p, nullptr, (size_t)j->w * j->h > 8u * 1000 * 1000);
        const std::vector<uint8_t> px = toRgba(out);
        const std::string tmp = path + ".tmp";
        std::lock_guard<std::mutex> l(m);
        if (gens[path] != g) return;
        if (gfx::writePng(tmp.c_str(), px.data(), out.w, out.h)) {
            std::error_code ec;
            std::filesystem::rename(std::filesystem::path(reinterpret_cast<const char8_t*>(tmp.c_str())), std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), ec);
        }
    });
}

}  // namespace enhui
