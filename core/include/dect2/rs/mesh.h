// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_mesh (LoRa mesh packet layers: Meshtastic, MeshCore, LoRaWAN, LoRa APRS, MeshCom). Written by hand; the Rust test
// rust/onair/tests/headers.rs checks it against ffi.rs.
// Text in the records is a length plus bytes (no NUL), cut when too long; payloads are LoRa frames of at most 255 bytes. Text writers return the length written or
// SIZE_MAX when the buffer is too small. Flags are 0 / 1 bytes. "Not in this packet" numbers keep the C++ defaults (-1).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairMeshS64 { uint16_t len; uint8_t b[64]; } OnairMeshS64;
typedef struct OnairMeshS128 { uint16_t len; uint8_t b[128]; } OnairMeshS128;
typedef struct OnairMeshS256 { uint16_t len; uint8_t b[256]; } OnairMeshS256;
typedef struct OnairMeshS2048 { uint16_t len; uint8_t b[2048]; } OnairMeshS2048;

// A node update (MeshNodeUpdate without protocol and node id).
typedef struct OnairMeshNode {
    OnairMeshS256 long_name, short_name;
    OnairMeshS64 hw_model, role;
    OnairMeshS128 public_key;                       // MeshCore: 32 byte key in hex
    uint8_t has_position, has_env, signature_checked, signature_ok;
    int32_t sats;                                   // -1 = none
    uint32_t position_time;
    double lat, lon, alt_m;
    double battery_pct, voltage, channel_util_pct, air_util_tx_pct;   // -1 = not in this packet
    double temp_c, humidity, pressure_hpa;
    int64_t uptime_s;                               // -1 = not in this packet
} OnairMeshNode;
// A text message (MeshTextMessage without protocol and channel).
typedef struct OnairMeshMessage {
    OnairMeshS256 from;
    OnairMeshS64 to;
    OnairMeshS256 text;
    int32_t hops;                                   // -1 = unknown
    uint32_t packet_id, reply_id, sender_time;
} OnairMeshMessage;

// Meshtastic header (16 bytes on the air): flags bits 0..2 hop limit, 3 want-ack, 4 via MQTT, 5..7 hop start.
typedef struct OnairMeshMtHeader { uint32_t to, from, id; uint8_t flags, channel, next_hop, relay; } OnairMeshMtHeader;
typedef struct OnairMeshMtDecoded {
    int32_t portnum;
    uint8_t has_node, has_message;
    OnairMeshS64 type_name;
    OnairMeshS2048 detail;
    OnairMeshNode node;
    OnairMeshMessage message;
} OnairMeshMtDecoded;

// MeshCore: a header that passed the length checks; offsets in the packet.
typedef struct OnairMeshMcParsed {
    uint8_t route, kind, ver, hcnt, hsz;
    uint16_t path_off, pay_off, m;
    OnairMeshS64 type_name;
    OnairMeshS256 path;                             // path hashes in hex, comma separated
    OnairMeshS64 via;                               // ", 3 hops" / ", direct from sender" / empty
} OnairMeshMcParsed;
typedef struct OnairMeshMcOut {
    uint8_t need_crypto, decrypted, clear_path, has_node;
    OnairMeshS64 from, to;
    OnairMeshS128 note;
    OnairMeshS2048 detail;
    OnairMeshNode node;
} OnairMeshMcOut;
typedef struct OnairMeshMcGroup {
    OnairMeshS64 type_name;
    OnairMeshS128 note;
    OnairMeshS2048 detail;
    uint8_t has_message;
    OnairMeshMessage message;
} OnairMeshMcGroup;

// LoRaWAN: MeshDecodeResult::Lorawan plus the packet fields the decoder sets.
typedef struct OnairMeshLorawan {
    uint8_t ok, has_lorawan;
    int32_t mtype, major;
    uint8_t uplink, confirmed, adr, adr_ack_req, ack, fpending, class_b;
    int32_t net_id_type, fopts_len, fport, dev_status_battery, dev_status_margin, rejoin_type;
    uint16_t fcnt, dev_nonce;
    uint32_t dev_addr, nwk_id, net_id, mic, rejoin_net_id, frm_len;
    uint64_t join_eui, dev_eui;
    OnairMeshS64 network, vendor;
    OnairMeshS256 mac_detail;
    uint32_t mac_count;
    OnairMeshS64 mac_names[16];
    OnairMeshS64 type_name, from, to;
    OnairMeshS2048 detail;
    OnairMeshS128 note;
    uint32_t packet_id;
} OnairMeshLorawan;

typedef struct OnairMeshLoraAprs { uint8_t ok; OnairMeshS128 note; OnairMeshS2048 raw; OnairMeshS64 src, dest; OnairMeshS256 path, info; } OnairMeshLoraAprs;
// MeshCom: kind 0 ack, 1 text, 2 position, 3 HEY. detail is complete for ack, text and HEY; for a position the caller adds the APRS part and "(" tail ")".
typedef struct OnairMeshMeshCom {
    uint8_t ok, kind, has_aprs, decrypted;
    int32_t hop_limit, battery_pct;
    uint32_t packet_id;
    OnairMeshS128 note;
    OnairMeshS2048 raw;
    OnairMeshS64 type_name;
    OnairMeshS2048 detail;
    OnairMeshS256 tail;
    OnairMeshS64 src;
    OnairMeshS256 aprs_path, aprs_dest, dest_call, aprs_info, channel, text;
} OnairMeshMeshCom;

// One protobuf field (data is NULL unless wire is 2).
typedef struct OnairMeshPbField { uint32_t num; int32_t wire; uint64_t value; const uint8_t* data; size_t len; } OnairMeshPbField;

// ABI version of this module (2).
uint32_t onair_mesh_version(void);
// sizeof of Node, Message, MtDecoded, McParsed, McOut, McGroup, Lorawan, LoraAprs, MeshCom, PbField in this order (out has room for 10): checked at start-up.
void onair_mesh_struct_sizes(size_t* out);

// Protobuf reader: next field of buf[0..n] from *pos (advanced); *ok goes false on a malformed field; false at the end or on error.
bool onair_mesh_pb_next(const uint8_t* buf, size_t n, size_t* pos, bool* ok, OnairMeshPbField* f);
// Repeated scalar fields, packed or not: values into out (at most cap stored), returns how many there are.
size_t onair_mesh_pb_repeated_fixed32(int32_t wire, uint64_t value, const uint8_t* data, size_t len, uint32_t* out, size_t cap);
size_t onair_mesh_pb_repeated_varint(int32_t wire, uint64_t value, const uint8_t* data, size_t len, uint64_t* out, size_t cap);

// Meshtastic helpers: "!1a2b3c4d"; hardware model name ("HW_<n>" when unknown) and value (-1); channel hash; key expansion into out32 (returns the length, 0 = no
// encryption, -1 = key too long); enum names by table (0 hardware, 1 port, 2 role, 3 routing error), 0 when not in the table.
size_t onair_mesh_node_id(uint32_t num, uint8_t* out, size_t cap);
size_t onair_mesh_hw_model_name(int32_t hw, uint8_t* out, size_t cap);
int32_t onair_mesh_hw_model_value(const uint8_t* name, size_t n);
uint8_t onair_mesh_channel_hash(const uint8_t* name, size_t n, const uint8_t* key, size_t klen);
int32_t onair_mesh_expand_psk(const uint8_t* psk, size_t n, uint8_t* out32);
size_t onair_mesh_enum_name(int32_t table, int32_t value, uint8_t* out, size_t cap);

// Meshtastic packets: the header (false when n < 16); the decrypted Data of a packet with the sender, destination, hop fields and packet id of its header
// (false when the bytes are not a plausible Data).
bool onair_mesh_mt_header(const uint8_t* p, size_t n, OnairMeshMtHeader* out);
bool onair_mesh_mt_decode(const uint8_t* plain, size_t n, uint32_t from, uint32_t to, int32_t hop_start, int32_t hop_limit, uint32_t packet_id, OnairMeshMtDecoded* out);

// MeshCore: split a packet (false and a note when it is not one); everything that needs no key; decrypted group bytes (kind 5 text, 6 data) with the hop text;
// the note when no channel decrypted a group packet (any: a channel with that hash exists).
bool onair_mesh_mc_header(const uint8_t* p, size_t n, OnairMeshMcParsed* h, OnairMeshS128* note);
void onair_mesh_mc_decode(const uint8_t* p, size_t n, const OnairMeshMcParsed* h, uint32_t sig_ok, OnairMeshMcOut* out);
void onair_mesh_mc_group_plain(uint32_t kind, uint32_t hcnt, const uint8_t* via, size_t vlen, const uint8_t* d, size_t dlen, OnairMeshMcGroup* out);
void onair_mesh_mc_group_fail(uint32_t hash, uint32_t any, OnairMeshS128* out);

// LoRaWAN
void onair_mesh_lorawan_decode(const uint8_t* p, size_t n, OnairMeshLorawan* out);
size_t onair_mesh_lorawan_mtype_name(int32_t t, uint8_t* out, size_t cap);
size_t onair_mesh_lorawan_mac_name(uint32_t cid, uint32_t uplink, uint8_t* out, size_t cap);
size_t onair_mesh_lorawan_network(uint32_t dev_addr, uint8_t* out, size_t cap, int32_t* kind, uint32_t* nwk, uint32_t* net);   // kind / nwk / net may be NULL
size_t onair_mesh_lorawan_vendor(uint64_t eui, uint8_t* out, size_t cap);
size_t onair_mesh_lorawan_eui(uint64_t eui, uint8_t* out, size_t cap);
size_t onair_mesh_hex_string(const uint8_t* p, size_t n, uint8_t* out, size_t cap);

// LoRa APRS and MeshCom, and the two text helpers they share
size_t onair_mesh_printable(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_mesh_clean_text(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
void onair_mesh_lora_aprs(const uint8_t* p, size_t n, OnairMeshLoraAprs* out);
void onair_mesh_meshcom(const uint8_t* p, size_t n, OnairMeshMeshCom* out);

#ifdef __cplusplus
}
#endif
