// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protobuf reader of the mesh messages on top of the Rust crate rust/onair_mesh: Reader::next and the repeated field readers of mesh_pb.h
// (mesh_pb.cpp keeps the Writer).
#include "dect2/mesh/mesh_pb.h"
#include "dect2/rs/mesh.h"

namespace dect2 {
namespace meshpb {

bool Reader::next(Field& f) {
    OnairMeshPbField c;
    size_t pos = 0;
    const bool got = onair_mesh_pb_next(p_, (size_t)(end_ - p_), &pos, &ok_, &c);
    p_ += pos;
    if (!got) return false;
    f = Field();
    f.num = c.num; f.wire = c.wire; f.value = c.value; f.data = c.data; f.len = c.len;
    return true;
}

void readRepeatedFixed32(const Field& f, std::vector<uint32_t>& out) {
    std::vector<uint32_t> v(f.len / 4 + 1);
    const size_t n = onair_mesh_pb_repeated_fixed32(f.wire, f.value, f.data, f.len, v.data(), v.size());
    out.insert(out.end(), v.begin(), v.begin() + (long)(n < v.size() ? n : v.size()));
}

void readRepeatedVarint(const Field& f, std::vector<uint64_t>& out) {
    std::vector<uint64_t> v(f.len + 1);
    const size_t n = onair_mesh_pb_repeated_varint(f.wire, f.value, f.data, f.len, v.data(), v.size());
    out.insert(out.end(), v.begin(), v.begin() + (long)(n < v.size() ? n : v.size()));
}

} // namespace meshpb
} // namespace dect2
