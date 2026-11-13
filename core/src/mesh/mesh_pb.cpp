// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/mesh/mesh_pb.h"

namespace dect2 {
namespace meshpb {

void Writer::packedFixed32(uint32_t field, const std::vector<uint32_t>& v) {
    if (v.empty()) return;
    tag(field, 2);
    raw(v.size() * 4);
    for (uint32_t x : v) for (int i = 0; i < 4; i++) b_.push_back((uint8_t)(x >> (8 * i)));
}

void Writer::packedVarint(uint32_t field, const std::vector<uint64_t>& v) {
    if (v.empty()) return;
    Writer t;
    for (uint64_t x : v) t.raw(x);
    bytes(field, t.b_);
}

} // namespace meshpb
} // namespace dect2
