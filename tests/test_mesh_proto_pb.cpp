// Protobuf reader and writer, and the field layouts of the Meshtastic messages (mesh.proto) with hand-encoded messages.
// Wire format examples: protobuf encoding guide (field 1 varint 150 = 08 96 01; field 2 string "testing" = 12 07 ...).
#include "dect2/mesh_pb.h"
#include "dect2/mesh_proto.h"
#include "dect2/mesh_crypto.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::meshpb;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::vector<uint8_t> H(const char* s) { std::vector<uint8_t> v; meshcrypto::hexDecode(s, v); return v; }
static std::string hex(const std::vector<uint8_t>& v) { return meshcrypto::hexEncode(v.data(), v.size()); }

int main() {
    { // encoding guide examples
        auto b = H("08 96 01");
        Reader r(b.data(), b.size()); Field f;
        CHECK(r.next(f) && f.num == 1 && f.wire == 0 && f.value == 150, "varint 150");
        CHECK(!r.next(f) && r.ok(), "clean end");
        Writer w; w.varint(1, 150); w.string(2, "testing");
        CHECK(hex(w.data()) == "089601120774657374696e67", "writer %s", hex(w.data()).c_str());
        Reader r2(w.data().data(), w.data().size());
        CHECK(r2.next(f) && r2.next(f) && f.num == 2 && f.wire == 2 && f.asString() == "testing", "string field");
    }
    { // zigzag, negative int32, fixed and float
        Writer w; w.sint32(1, -1); w.sint32(2, 150); w.int32(3, -1); w.sfixed32(4, -2); w.floating(5, 1.5f); w.fixed32(6, 0xdeadbeef);
        const auto& d = w.data();
        CHECK(hex(d) == "0801" "10ac02" "18ffffffffffffffffff01" "25feffffff" "2d0000c03f" "35efbeadde", "mixed %s", hex(d).c_str());
        Reader r(d.data(), d.size()); Field f;
        CHECK(r.next(f) && f.asSint32() == -1, "sint -1");
        CHECK(r.next(f) && f.asSint32() == 150, "sint 150");
        CHECK(r.next(f) && f.asInt32() == -1, "int32 -1 from 10 byte varint");
        CHECK(r.next(f) && f.wire == 5 && f.asInt32() == -2, "sfixed32");
        CHECK(r.next(f) && f.asFloat() == 1.5f, "float");
        CHECK(r.next(f) && f.value == 0xdeadbeefu, "fixed32");
        CHECK(!r.next(f) && r.ok(), "end");
    }
    { // packed and unpacked repeated fields
        Writer w; w.packedFixed32(1, {1, 0x01020304}); w.packedVarint(2, {3, 300, (uint64_t)(int64_t)-4});
        Reader r(w.data().data(), w.data().size()); Field f;
        std::vector<uint32_t> a; std::vector<uint64_t> b;
        while (r.next(f)) { if (f.num == 1) readRepeatedFixed32(f, a); else readRepeatedVarint(f, b); }
        CHECK(a.size() == 2 && a[1] == 0x01020304u, "packed fixed32");
        CHECK(b.size() == 3 && b[1] == 300 && (int32_t)(uint32_t)b[2] == -4, "packed varint");
        Writer u; u.fixed32(1, 7); u.fixed32(1, 9); u.varint(2, 5); u.varint(2, 6);
        Reader r2(u.data().data(), u.data().size());
        a.clear(); b.clear();
        while (r2.next(f)) { if (f.num == 1) readRepeatedFixed32(f, a); else readRepeatedVarint(f, b); }
        CHECK(a.size() == 2 && a[0] == 7 && a[1] == 9 && b.size() == 2 && b[1] == 6, "unpacked");
    }
    { // malformed input
        const char* bad[] = {"08", "0880", "12 05 61", "0a ff ff ff ff ff ff ff ff ff ff 01", "0d 01 02", "11 01 02 03", "00 01", "0b", "0c", "0e 01"};
        for (const char* s : bad) {
            auto b = H(s); Reader r(b.data(), b.size()); Field f;
            while (r.next(f)) {}
            CHECK(!r.ok(), "malformed '%s' must be reported", s);
        }
        Reader r(nullptr, 0); Field f;
        CHECK(!r.next(f) && r.ok(), "empty message is fine");
    }
    if (fails) return 1;
    printf("mesh_proto_pb ok\n");
    return 0;
}
