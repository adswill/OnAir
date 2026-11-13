// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 protocol pieces: the frame sync table (see p25_proto.h). The layer itself is Rust: p25_proto_rs.cpp over rust/onair_p25.
#include "dect2/p25/p25_proto.h"

namespace dect2 {
namespace p25 {

const int kSync[kSyncSym] = {3, 3, 3, 3, 3, -3, 3, 3, -3, -3, 3, 3, -3, -3, -3, -3, 3, -3, 3, -3, -3, -3, -3, -3};

} // namespace p25
} // namespace dect2
