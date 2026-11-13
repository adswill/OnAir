// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_net (network protocol layers). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Text writers return the length the text has (also when the buffer was too short: nothing is
// written past it); (size_t)-1 marks a failure. Null pointers are empty input / failure.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_net_version(void);

// A byte range of a text the caller gave.
typedef struct OnairNetSpan { uint32_t off, len; } OnairNetSpan;
// A request header field: name as sent and value, both trimmed (ranges of the head).
typedef struct OnairNetField { OnairNetSpan name, value; } OnairNetField;
// The request head. err: 0 ok, 1 bad request line, 2 bad target, 3 bad header, 4 duplicate header, 5 chunked, 6 bad Content-Length, 7 body too large, 8 length required.
typedef struct OnairNetHead { uint32_t err, n_fields, content_length; OnairNetSpan method, path, query; } OnairNetHead;
typedef struct OnairNetGreeting { uint8_t magic[4]; uint32_t tuner, n_gains, other; } OnairNetGreeting;
// One control of the SDR++ server's menu. kind: 0 combo, 1 checkbox, 2 radio, 3 int slider, 4 float slider, 5 int input, 6 text. Ranges are bytes of the draw list, or of the
// label arena when label_in_arena (combo items are ranges of the draw list, items_count of them from items_first).
typedef struct OnairNetWidget {
    uint32_t kind, disabled, b;
    int32_t i;
    uint32_t items_first, items_count, label_in_arena, reserved;
    double value, min_v, max_v, step;
    OnairNetSpan id, label;
} OnairNetWidget;
typedef struct OnairNetUiCounts { uint32_t ok, widgets, items, arena; } OnairNetUiCounts;
// One network radio. kind: 0 Pluto, 1 rtl_tcp, 2 SoapyRemote, 3 SpyServer, 4 Hermes Lite 2, 5 Red Pitaya, 6 SDR++ server.
typedef struct OnairNetRadio { uint32_t kind; int32_t port; OnairNetSpan host, remote; } OnairNetRadio;
// A member of a flat JSON object. kind: 0 null, 1 number, 2 string, 3 bool. key and string ranges are bytes of the arena.
typedef struct OnairNetJsonEntry { uint32_t kind, b; double num; uint32_t key_off, key_len, str_off, str_len; } OnairNetJsonEntry;
// A client WebSocket frame. kind: 0 need more bytes, 1 close with `code`, 2 a whole frame.
typedef struct OnairNetWsClient { uint32_t kind, code, op, fin; size_t used, len; } OnairNetWsClient;

// sizeof of the structs above in the order Head, Field, Greeting, Widget, UiCounts, Radio, JsonEntry, WsClient written to out[0..8]; the C++ side compares them at start-up.
void onair_net_struct_sizes(size_t* out);

// SHA-1 (20 bytes) and base64 (with padding) for the WebSocket handshake; the accept value is 28 bytes.
void onair_net_sha1(const uint8_t* data, size_t n, uint8_t* out);
size_t onair_net_base64(const uint8_t* data, size_t n, uint8_t* out, size_t cap);
void onair_net_ws_accept(const uint8_t* key, size_t n, uint8_t* out);

// WebSocket framing: server frames (unmasked) and the first client frame of the received bytes (payload buffer of 4096 bytes). Returns the kind (also in out).
size_t onair_net_ws_frame_len(size_t n);
size_t onair_net_ws_frame(uint32_t opcode, const uint8_t* payload, size_t n, uint8_t* out, size_t cap);
uint32_t onair_net_ws_parse_client(const uint8_t* input, size_t n, uint8_t* payload, size_t cap, OnairNetWsClient* out);

// URL text: percent decoding (also '+' to a space), query parameter, host part of a Host header, loopback names, constant time compare, reason phrase.
size_t onair_net_url_decode(const uint8_t* s, size_t n, uint8_t* out, size_t cap);
size_t onair_net_query_param(const uint8_t* q, size_t qn, const uint8_t* key, size_t kn, uint8_t* out, size_t cap, bool* found);
size_t onair_net_host_only_len(const uint8_t* h, size_t n);
bool onair_net_loopback_name(const uint8_t* h, size_t n);
bool onair_net_ct_equal(const uint8_t* a, size_t an, const uint8_t* b, size_t bn);
size_t onair_net_status_text(int32_t code, uint8_t* out, size_t cap);

// Flat JSON object (at most 32 keys): the number of entries, -1 for anything malformed. The arena needs n bytes.
int32_t onair_net_flatjson_parse(const uint8_t* t, size_t n, OnairNetJsonEntry* entries, size_t ecap, uint8_t* arena, size_t acap);

// HTTP request head: scan the received bytes for its end (0 need more, 1 found at *end, 2 over 8192 bytes), parse it (up to 48 fields), the status and text of an error id.
uint32_t onair_net_http_scan(const uint8_t* buf, size_t n, size_t* end);
void onair_net_http_parse(const uint8_t* head, size_t n, OnairNetField* fields, size_t cap, OnairNetHead* out);
size_t onair_net_http_error(uint32_t id, uint32_t* status, uint8_t* out, size_t cap);

// rtl_tcp: the greeting, the gain table (tenths of a dB) and name of a tuner type, the 5 byte command, "host:port" splitting (shared by the SpyServer and SDR++ clients).
bool onair_net_rtltcp_greeting(const uint8_t* h, size_t n, OnairNetGreeting* out);
size_t onair_net_rtltcp_gains(int32_t tuner, int32_t* out, size_t cap);
size_t onair_net_rtltcp_tuner_name(int32_t t, uint8_t* out, size_t cap);
void onair_net_rtltcp_command(uint32_t cmd, uint32_t v, uint8_t* out);
bool onair_net_split_host_port(const uint8_t* a, size_t n, OnairNetSpan* host, int32_t* port);

// SpyServer: DEVICE_INFO (12 words) and CLIENT_SYNC (9 words), commands, message header (type, flags, body size), the rates of a server, device type name.
void onair_net_spy_device_info(const uint8_t* b, size_t n, uint32_t* out);
void onair_net_spy_client_sync(const uint8_t* b, size_t n, uint32_t* out);
size_t onair_net_spy_command(uint32_t ty, const uint8_t* body, size_t n, uint8_t* out, size_t cap);
size_t onair_net_spy_hello(uint8_t* out, size_t cap);
size_t onair_net_spy_set_setting(uint32_t id, uint32_t v, uint8_t* out, size_t cap);
bool onair_net_spy_header(const uint8_t* h, size_t n, uint32_t* ty, uint32_t* flags, uint32_t* size);
size_t onair_net_spy_rates(uint32_t max_rate, uint32_t dec_stages, uint32_t min_dec, double* out, size_t cap);
size_t onair_net_spy_dev_name(uint32_t t, uint8_t* out, size_t cap);

// SDR++ server: the menu draw list (arrays of n/3+1 widgets, n+1 items and 9n+64 label bytes always suffice), which of the words sr, bw, lna, vga, mix, mixer, if, lin,
// sens (bits 0 to 8) an id holds, a label made from an id, a rate text ("2.5MHz", NUL terminated) in Hz, command packets, the UI_ACTION argument, packet header, error text.
void onair_net_sdrpp_parse_ui(const uint8_t* p, size_t n, OnairNetWidget* widgets, size_t wcap, OnairNetSpan* items, size_t icap, uint8_t* arena, size_t acap, OnairNetUiCounts* out);
uint32_t onair_net_sdrpp_id_flags(const uint8_t* id, size_t n);
size_t onair_net_sdrpp_label_from_id(const uint8_t* id, size_t n, uint8_t* out, size_t cap);
double onair_net_sdrpp_rate_text(const char* t);
size_t onair_net_sdrpp_command(uint32_t cmd, const uint8_t* arg, size_t n, uint8_t* out, size_t cap);
size_t onair_net_sdrpp_ui_action(const uint8_t* id, size_t n, uint32_t ty, int32_t i, float f, uint32_t b, uint32_t sync, uint8_t* out, size_t cap);
bool onair_net_sdrpp_packet_header(const uint8_t* h, size_t n, uint32_t* ty, uint32_t* body);
size_t onair_net_sdrpp_error_text(uint32_t c, uint8_t* out, size_t cap);

// The list of network radios of the settings: parse (the ';' count + 1 radios and n arena bytes suffice; host ranges index the arena, remote ranges the text), format
// (ranges index the pool), host clean-up, type name, default port and host, port connected to.
size_t onair_net_radios_parse(const uint8_t* s, size_t n, OnairNetRadio* radios, size_t rcap, uint8_t* arena, size_t acap);
size_t onair_net_radios_format(const OnairNetRadio* r, size_t n, const uint8_t* pool, size_t pn, uint8_t* out, size_t cap);
size_t onair_net_radio_clean_host(const uint8_t* h, size_t n, uint8_t* out, size_t cap);
size_t onair_net_radio_type_name(uint32_t t, uint8_t* out, size_t cap);
int32_t onair_net_radio_default_port(uint32_t t);
size_t onair_net_radio_default_host(uint32_t t, uint8_t* out, size_t cap);
int32_t onair_net_radio_port(uint32_t t, int32_t port);

#ifdef __cplusplus
}
#endif
