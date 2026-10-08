// Small Protocol Buffers reader and writer for the Meshtastic messages (wire format: protobuf encoding guide).
// Reader: varint, 64-bit, length-delimited and 32-bit fields, unknown fields are simply returned and ignored by the caller.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dect2 {
namespace meshpb {

struct Field {
    uint32_t num = 0;
    int wire = 0;                       // 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32
    uint64_t value = 0;                 // wire 0, 1, 5
    const uint8_t* data = nullptr;      // wire 2
    size_t len = 0;
    int32_t asInt32() const { return (int32_t)(uint32_t)value; }          // int32 / sfixed32 (a negative int32 is a 10 byte varint)
    int32_t asSint32() const { return (int32_t)((uint32_t)value >> 1) ^ -(int32_t)((uint32_t)value & 1); }
    float asFloat() const { uint32_t u = (uint32_t)value; float f; std::memcpy(&f, &u, 4); return f; }
    std::string asString() const { return data ? std::string((const char*)data, len) : std::string(); }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    // Next field; false at the end of the message or on a malformed field (then ok() is false).
    bool next(Field& f);
    bool ok() const { return ok_; }
    bool atEnd() const { return p_ >= end_; }
private:
    bool varint(uint64_t& v);
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

// Packed or unpacked repeated scalar: appends the values of a field that may be either form.
void readRepeatedFixed32(const Field& f, std::vector<uint32_t>& out);
void readRepeatedVarint(const Field& f, std::vector<uint64_t>& out);

class Writer {
public:
    void varint(uint32_t field, uint64_t v) { tag(field, 0); raw(v); }
    void int32(uint32_t field, int32_t v) { tag(field, 0); raw(v < 0 ? (uint64_t)(int64_t)v : (uint64_t)v); }
    void sint32(uint32_t field, int32_t v) { varint(field, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31)); }
    void boolean(uint32_t field, bool v) { varint(field, v ? 1 : 0); }
    void fixed32(uint32_t field, uint32_t v) { tag(field, 5); for (int i = 0; i < 4; i++) b_.push_back((uint8_t)(v >> (8 * i))); }
    void sfixed32(uint32_t field, int32_t v) { fixed32(field, (uint32_t)v); }
    void floating(uint32_t field, float f) { uint32_t u; std::memcpy(&u, &f, 4); fixed32(field, u); }
    void bytes(uint32_t field, const uint8_t* p, size_t n) { tag(field, 2); raw(n); b_.insert(b_.end(), p, p + n); }
    void bytes(uint32_t field, const std::vector<uint8_t>& v) { bytes(field, v.data(), v.size()); }
    void string(uint32_t field, const std::string& s) { bytes(field, (const uint8_t*)s.data(), s.size()); }
    void packedFixed32(uint32_t field, const std::vector<uint32_t>& v);
    void packedVarint(uint32_t field, const std::vector<uint64_t>& v);
    const std::vector<uint8_t>& data() const { return b_; }
private:
    void tag(uint32_t field, int wire) { raw(((uint64_t)field << 3) | (uint64_t)wire); }
    void raw(uint64_t v) { while (v >= 0x80) { b_.push_back((uint8_t)(v | 0x80)); v >>= 7; } b_.push_back((uint8_t)v); }
    std::vector<uint8_t> b_;
};

} // namespace meshpb
} // namespace dect2
