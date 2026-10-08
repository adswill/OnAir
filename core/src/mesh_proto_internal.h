// Shared by the mesh_proto*.cpp files only.
#pragma once
#include "dect2/mesh_proto.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct MeshEnumName { int value; const char* name; };       // table ends with value -1, name nullptr
extern const MeshEnumName kMeshHwModels[];
extern const MeshEnumName kMeshPortNums[];
extern const MeshEnumName kMeshRoles[];
extern const MeshEnumName kMeshRoutingErrors[];
const char* meshEnumName(const MeshEnumName* t, int v);      // nullptr if not in the table

struct MeshtasticChannel {
    std::string name;
    std::vector<uint8_t> key;           // empty: no encryption; 16 or 32 bytes
    uint8_t hash = 0;
    bool user = false;
};

struct MeshCoreChannel {
    std::string name;
    uint8_t secret[32] = {};            // a 16 byte secret is zero padded, as in MeshCore (the HMAC key is the whole 32 bytes)
    size_t keyLen = 16;
    uint8_t hash = 0;                   // SHA-256 of the secret (keyLen bytes), first byte
    bool user = false;
};

struct MeshProto::State {
    mutable std::mutex m;
    std::vector<MeshtasticChannel> mt;
    std::vector<MeshCoreChannel> mc;
};

std::string meshHex(const uint8_t* p, size_t n);
std::string meshFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const uint8_t* meshtasticDefaultKey();                          // 16 bytes
bool meshcoreMakeChannel(const std::string& name, const uint8_t* secret, size_t len, MeshCoreChannel& out);

} // namespace dect2
