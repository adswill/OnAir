// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "videoin_impl.h"

namespace dect2 {

bool videoInCameraStart(VideoIn::Impl*, const std::string&, std::string& err) {
    err = "this build has no camera / capture-card support";
    return false;
}
void videoInCameraStop(VideoIn::Impl*) {}
std::vector<VideoInputDevice> videoInListCameras() { return {}; }

} // namespace dect2
