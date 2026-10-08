#include "dect2/mesh_pb.h"

namespace dect2 {
namespace meshpb {

bool Reader::varint(uint64_t& v) {
    v = 0;
    for (int i = 0; i < 10; i++) {
        if (p_ >= end_) return false;
        const uint8_t b = *p_++;
        v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return true;
    }
    return false;                                           // longer than 10 bytes
}

bool Reader::next(Field& f) {
    if (!ok_ || p_ >= end_) return false;
    uint64_t key;
    if (!varint(key)) { ok_ = false; return false; }
    f = Field();
    f.num = (uint32_t)(key >> 3);
    f.wire = (int)(key & 7);
    if (f.num == 0) { ok_ = false; return false; }
    switch (f.wire) {
    case 0:
        if (!varint(f.value)) { ok_ = false; return false; }
        return true;
    case 1:
        if (end_ - p_ < 8) { ok_ = false; return false; }
        for (int i = 0; i < 8; i++) f.value |= (uint64_t)p_[i] << (8 * i);
        p_ += 8;
        return true;
    case 5:
        if (end_ - p_ < 4) { ok_ = false; return false; }
        for (int i = 0; i < 4; i++) f.value |= (uint64_t)p_[i] << (8 * i);
        p_ += 4;
        return true;
    case 2: {
        uint64_t n;
        if (!varint(n) || n > (uint64_t)(end_ - p_)) { ok_ = false; return false; }
        f.data = p_;
        f.len = (size_t)n;
        p_ += n;
        return true;
    }
    default:                                                // groups (3, 4) and 6, 7 are not used by these messages
        ok_ = false;
        return false;
    }
}

void readRepeatedFixed32(const Field& f, std::vector<uint32_t>& out) {
    if (f.wire == 5) { out.push_back((uint32_t)f.value); return; }
    if (f.wire != 2) return;
    for (size_t i = 0; i + 4 <= f.len; i += 4)
        out.push_back((uint32_t)f.data[i] | ((uint32_t)f.data[i + 1] << 8) | ((uint32_t)f.data[i + 2] << 16) | ((uint32_t)f.data[i + 3] << 24));
}

void readRepeatedVarint(const Field& f, std::vector<uint64_t>& out) {
    if (f.wire == 0) { out.push_back(f.value); return; }
    if (f.wire != 2) return;
    const uint8_t* p = f.data;
    const uint8_t* e = f.data + f.len;
    while (p < e) {
        uint64_t v = 0;
        int i = 0;
        for (; i < 10 && p < e; i++) {
            const uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) break;
        }
        out.push_back(v);
    }
}

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
