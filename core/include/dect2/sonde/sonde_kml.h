// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The selected radiosonde for Google Earth, live: a KML with the flight path drawn down to the ground (extruded, translucent wall,
// green on the way up and red on the way down), the burst point, the current position with serial / height / climb rate / time, and
// the predicted rest of the flight as a dashed line to the landing point. OnAir rewrites the file every few seconds and a small
// NetworkLink KML (opened once in Google Earth) makes Google Earth reload it. Pure text building (sonde_kml.cpp); the files are
// written by app/sonde/sonde_live_ui.cpp.
#pragma once
#include "dect2/sonde/sonde_course.h"
#include "dect2/sonde/sonde_tel.h"
#include <string>
#include <vector>

namespace dect2 {

// pred: the predicted course (sondecourse::build), empty for none. nowUnix: the time shown as "updated".
std::string sondeLiveKml(const SondeInfo& s, const std::vector<sondecourse::Point>& pred, double nowUnix);

// A KML with one NetworkLink that reloads hrefUrl every refreshS seconds (refreshMode onInterval).
std::string sondeNetworkLinkKml(const std::string& hrefUrl, int refreshS, const std::string& name);

// A file path as a file:// URL (backslashes to slashes, a drive letter gets the leading slash, bytes outside the URL-safe set as %XX).
std::string fileUrlOfPath(const std::string& path);

}  // namespace dect2
