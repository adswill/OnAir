// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The shared picture gallery (View > Images): every picture the satellite, fax, SSTV and HamDRM receivers finish is handed to publish()
// below by the receiver's screen code (the DSP is not touched). The pictures are kept in <data folder>/images/ with an index file, capped by size.
#pragma once
#include "dect2/dsp/imgproc.h"
#include "dect2/geo/passes.h"
#include <cstdint>
#include <string>
#include <vector>

struct App;

namespace gallery {

struct Meta {
    std::string satellite;                       // "Meteor-M2 4", "GOES-19", "" when not known
    std::string detail;                          // the mode or product: "Robot 36", "channels 1 2 3", "IR full disk"
    std::vector<dect2::img::TrackPoint> track;   // sub-satellite point of each row of linesPerTrack lines (empty: no map projection)
    int linesPerTrack = 8;
    double swathKm = 2800;
    std::vector<std::vector<uint8_t>> planes;    // separate grey channels, each w x h (Meteor 1 .. 6; an empty vector for a channel not received)
};

// A picture, RGBA 8 bit, w x h. key names it: the same key again replaces the picture (a picture that grew), a new key adds one.
void publish(const std::string& key, const std::string& source, double unixTime, int w, int h, const uint8_t* rgba, const Meta& meta = Meta());
void publishGray(const std::string& key, const std::string& source, double unixTime, int w, int h, const uint8_t* grey, const Meta& meta = Meta());
void publishRgb(const std::string& key, const std::string& source, double unixTime, int w, int h, const uint8_t* rgb, const Meta& meta = Meta());
// A picture file as received (PNG, JPEG, GIF ...): decoded with the system's decoder
void publishFile(const std::string& key, const std::string& source, double unixTime, const std::vector<uint8_t>& bytes, const Meta& meta = Meta());

// The pass the scheduler is receiving (or just finished): pictures published from aos to validUntil that name no satellite get it, and
// all of them get "pass hh:mm, max N deg" added to their detail. An empty sat clears it.
void setPassContext(const std::string& sat, double aos, double maxElDeg, double validUntil);

}  // namespace gallery

void galleryDev(int mode);    // dev: --images-dev 1 | 2 (see gallery_ui.cpp)
void galleryWindow(App& a);   // gallery_ui.cpp: the window (a.galleryOpen), every frame
// passes_ui.cpp: the listed satellite whose name contains nameContains that is highest above the receiver at unixSec (null: none, no TLEs)
const dect2::Sgp4* passesFindSat(const std::string& nameContains, double unixSec, std::string* name = nullptr);
