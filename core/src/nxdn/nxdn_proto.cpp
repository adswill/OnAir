// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN common air interface: the frame sync word table (see nxdn_proto.h). The layer itself is Rust: nxdn_proto_rs.cpp over rust/onair_nxdn.
#include "dect2/nxdn/nxdn_proto.h"

namespace dect2 {
namespace nxdn {

const int kFsw[kFswSym] = {-3, 1, -3, 3, -3, -3, 3, 3, -1, 3};

} // namespace nxdn
} // namespace dect2
