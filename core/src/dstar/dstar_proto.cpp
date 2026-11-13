// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR protocol pieces: the sync pattern tables (see dstar_proto.h). The layer itself is Rust: dstar_proto_rs.cpp over rust/onair_dstar.
#include "dect2/dstar/dstar_proto.h"

namespace dect2 {
namespace dstar {

const uint8_t kFrameSync[15] = {1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0};
const uint8_t kDataSync[24] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0};
const uint8_t kEndPattern[48] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
                                 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0};
const uint8_t kSlowScramble[3] = {0x70, 0x4F, 0x93};

} // namespace dstar
} // namespace dect2
