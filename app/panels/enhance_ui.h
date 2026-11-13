// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Enhance control shared by every picture view (the gallery viewer and the image tabs of SSTV, weather fax, DRM and DAB slide
// shows, KAIT, RF camera, analog TV): a toggle, presets, the settings popup with sliders, a before/after split, hold to compare and
// Save enhanced PNG. The raw picture is never changed: the enhanced picture is a view (worked out on the thread pool, the cheap preview
// path while a picture is still arriving) and an extra file. "Apply to all new images" is kept per mode in the preferences: views of
// that mode start enhanced, and the gallery also saves an enhanced copy (<id>_e.png) next to every new raw picture of that mode.
// Core: core/include/dect2/image/enhance.h; docs/IMAGE-ENHANCE.md.
#pragma once
#include "dect2/image/enhance.h"
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace gfx { struct Image; }

namespace enhui {

class Enhancer {
public:
    // the mode the pictures come from ("SSTV", "Weather fax" ...): picks the kind and the remembered settings
    void setSource(const std::string& source);
    const std::string& source() const { return source_; }
    // a new raw picture (RGBA 8 bit, w x h). finished: false while the picture is still building line by line (preview path)
    void feed(const uint8_t* rgba, int w, int h, bool finished = true);
    // every frame: collects a finished enhancement and starts the next one; true when the view changed
    bool poll();
    bool on() const { return on_; }
    bool busy() const { return job_.valid(); }
    // what to show: the raw picture when off (or while held), else the enhanced one with the before/after split
    const std::vector<uint8_t>& view(int& w, int& h);
    // the enhanced picture alone (empty when there is none yet)
    const std::vector<uint8_t>& enhanced(int& w, int& h) const { w = ew_; h = eh_; return enh_; }
    // the controls on one line: [Enhance] [preset] [...] [split] [Hold] [Save]; true when the view changed
    bool controls(const char* id = "enh");
    void setSaveName(const std::string& name) { saveName_ = name; }

private:
    void start();
    void loadPrefs();
    void savePrefs();
    bool settings();

    struct Result { std::vector<uint8_t> rgba; int w = 0, h = 0, inW = 0, inH = 0; uint64_t gen = 0; dect2::image::Report rep; int preset = 0; };
    std::string source_ = "?";
    dect2::image::Kind kind_ = dect2::image::Kind::Generic;
    bool on_ = false, autoNew_ = false, hold_ = false;
    int preset_ = 0;                          // dect2::image::Preset
    dect2::image::Params custom_;
    float split_ = 0.f;                       // 0: all enhanced; left of the split = before
    std::vector<uint8_t> raw_; int rw_ = 0, rh_ = 0; bool rawFinished_ = true;
    uint64_t gen_ = 0, doneGen_ = 0;          // raw/settings generation, the one the enhanced picture was made from
    std::vector<uint8_t> enh_; int ew_ = 0, eh_ = 0, enhInW_ = 0, enhInH_ = 0;
    dect2::image::Report rep_; int repPreset_ = 0;
    std::future<Result> job_;
    std::vector<uint8_t> view_; int vw_ = 0, vh_ = 0; bool viewDirty_ = true;
    std::string saveName_ = "enhanced.png", msg_;
};

// An Enhancer with its own texture, for the image tabs: feed it the raw pixels when they change, draw texture(own) instead of own
struct LiveTex {
    Enhancer e;
    gfx::Image* tex = nullptr;
    int tw = 0, th = 0;
    bool dirty = true;
    // the texture to draw: own (the tab's texture of the raw picture) when Enhance is off
    gfx::Image* texture(gfx::Image* own);
    void feed(const uint32_t* px, int w, int h, bool finished = true) { e.feed(reinterpret_cast<const uint8_t*>(px), w, h, finished); }
    bool controls(const char* id = "enh") { const bool c = e.controls(id); if (c) dirty = true; return c; }
    LiveTex() = default;
    LiveTex(const LiveTex&) = delete;
    LiveTex& operator=(const LiveTex&) = delete;   // lives as long as its screen (like the screens' own textures, not freed at exit)
};

// Gallery: when "Apply to all new images" is set for source, the enhanced copy of a new raw picture is written to path (in the background)
void autoEnhanceFile(const std::string& source, const uint8_t* rgba, int w, int h, const std::string& path);
bool autoEnabled(const std::string& source);

}  // namespace enhui
