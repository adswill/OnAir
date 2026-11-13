// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// net_proto.h on top of the Rust crate rust/onair_net: thin wrappers over the C ABI of core/include/dect2/rs/net.h.
#include "dect2/net/net_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/net.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::netp {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[8];
        onair_net_struct_sizes(s);
        dect2::rsAbiCheck("onair_net", s, {sizeof(OnairNetHead), sizeof(OnairNetField), sizeof(OnairNetGreeting), sizeof(OnairNetWidget), sizeof(OnairNetUiCounts), sizeof(OnairNetRadio), sizeof(OnairNetJsonEntry), sizeof(OnairNetWsClient)});
    }
} abiCheck;
inline const uint8_t* U(const std::string& s) { return (const uint8_t*)s.data(); }
inline const uint8_t* U(const unsigned char* p) { return (const uint8_t*)p; }
inline std::string sub(const std::string& t, OnairNetSpan s) { return t.substr(s.off, s.len); }
// text from a writer taking (buffer, capacity) and returning the full length
template <class F> std::string text(F f) {
    char small[64];
    const size_t n = f((uint8_t*)small, sizeof small);
    if (n <= sizeof small) return std::string(small, n);
    std::string s(n, '\0');
    f((uint8_t*)s.data(), s.size());
    return s;
}
} // namespace

std::string sha1(const std::string& d) {
    uint8_t o[20];
    onair_net_sha1(U(d), d.size(), o);
    return std::string((const char*)o, 20);
}
std::string base64(const std::string& d) {
    std::string o((d.size() + 2) / 3 * 4, '\0');
    onair_net_base64(U(d), d.size(), (uint8_t*)o.data(), o.size());
    return o;
}
std::string wsAcceptKey(const std::string& key) {
    uint8_t o[28];
    onair_net_ws_accept(U(key), key.size(), o);
    return std::string((const char*)o, 28);
}
std::string wsFrame(int opcode, const std::string& p) {
    std::string o(onair_net_ws_frame_len(p.size()), '\0');
    const size_t n = onair_net_ws_frame((uint32_t)opcode, U(p), p.size(), (uint8_t*)o.data(), o.size());
    o.resize(n == (size_t)-1 ? 0 : n);
    return o;
}
WsClient wsParseClient(const std::string& in) {
    WsClient r;
    uint8_t pl[4096];
    OnairNetWsClient o;
    onair_net_ws_parse_client(U(in), in.size(), pl, sizeof pl, &o);
    r.kind = (int)o.kind;
    if (o.kind == 1) r.code = (int)o.code;
    if (o.kind == 2) { r.op = (int)o.op; r.fin = o.fin != 0; r.used = o.used; r.payload.assign((const char*)pl, o.len); }
    return r;
}

std::string urlDecode(const std::string& s) {
    std::string o(s.size(), '\0');
    o.resize(std::min(onair_net_url_decode(U(s), s.size(), (uint8_t*)o.data(), o.size()), o.size()));
    return o;
}
std::string queryParam(const std::string& q, const std::string& key) {
    std::string o(q.size(), '\0');
    bool found = false;
    const size_t n = onair_net_query_param(U(q), q.size(), U(key), key.size(), (uint8_t*)o.data(), o.size(), &found);
    o.resize(found ? std::min(n, o.size()) : 0);
    return o;
}
std::string hostOnly(const std::string& h) { return h.substr(0, onair_net_host_only_len(U(h), h.size())); }
bool loopbackName(const std::string& n) { return onair_net_loopback_name(U(n), n.size()); }
bool ctEqual(const std::string& a, const std::string& b) { return onair_net_ct_equal(U(a), a.size(), U(b), b.size()); }
std::string statusText(int c) { return text([&](uint8_t* b, size_t n) { return onair_net_status_text(c, b, n); }); }

bool parseFlatJson(const std::string& t, std::map<std::string, JsonVal>& out) {
    out.clear();
    OnairNetJsonEntry e[32];
    std::vector<uint8_t> arena(t.size() + 1);
    const int32_t n = onair_net_flatjson_parse(U(t), t.size(), e, 32, arena.data(), arena.size());
    if (n < 0) return false;
    for (int32_t k = 0; k < n; k++) {
        JsonVal v;
        v.kind = (JsonVal::Kind)e[k].kind;
        v.num = e[k].num;
        v.b = e[k].b != 0;
        if (e[k].kind == JsonVal::Str) v.str.assign((const char*)arena.data() + e[k].str_off, e[k].str_len);
        out[std::string((const char*)arena.data() + e[k].key_off, e[k].key_len)] = v;
    }
    return true;
}

int httpScan(const std::string& buf, size_t& end) {
    end = 0;
    return (int)onair_net_http_scan(U(buf), buf.size(), &end);
}
HttpError httpHead(const std::string& head, HttpHead& rq) {
    OnairNetField f[48];
    OnairNetHead h;
    onair_net_http_parse(U(head), head.size(), f, 48, &h);
    if (h.err) {
        HttpError e;
        uint32_t st = 0;
        e.msg = text([&](uint8_t* b, size_t n) { return onair_net_http_error(h.err, &st, b, n); });
        e.status = (int)st;
        return e;
    }
    rq.method = sub(head, h.method);
    rq.path = sub(head, h.path);
    rq.query = sub(head, h.query);
    for (uint32_t k = 0; k < h.n_fields; k++) {
        std::string name = sub(head, f[k].name);
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        rq.h[name] = sub(head, f[k].value);
    }
    rq.contentLength = h.content_length;
    return HttpError();
}

bool rtlGreeting(const unsigned char* hdr, size_t n, RtlGreeting& g) {
    OnairNetGreeting o;
    if (!onair_net_rtltcp_greeting(U(hdr), n, &o)) return false;
    g.magic.assign((const char*)o.magic, 4);
    g.tuner = o.tuner;
    g.nGains = o.n_gains;
    g.other = o.other != 0;
    return true;
}
std::vector<int> rtlTunerGains(int tuner) {
    int32_t g[64];
    const size_t n = onair_net_rtltcp_gains(tuner, g, 64);
    return std::vector<int>(g, g + std::min<size_t>(n, 64));
}
std::string rtlTunerName(int t) { return text([&](uint8_t* b, size_t n) { return onair_net_rtltcp_tuner_name(t, b, n); }); }
std::string rtlCommand(uint8_t cmd, uint32_t v) {
    uint8_t o[5];
    onair_net_rtltcp_command(cmd, v, o);
    return std::string((const char*)o, 5);
}
bool splitHostPort(const std::string& a, std::string& host, int& port) {
    OnairNetSpan h;
    int32_t p = 0;
    if (!onair_net_split_host_port(U(a), a.size(), &h, &p)) return false;
    host = sub(a, h);
    port = p;
    return true;
}

void spyDeviceInfo(const unsigned char* b, uint32_t n, uint32_t out[12]) { onair_net_spy_device_info(U(b), n, out); }
void spyClientSync(const unsigned char* b, uint32_t n, uint32_t out[9]) { onair_net_spy_client_sync(U(b), n, out); }
std::string spyCommand(uint32_t type, const unsigned char* body, uint32_t n) {
    std::string o(8 + (size_t)n, '\0');
    o.resize(onair_net_spy_command(type, U(body), n, (uint8_t*)o.data(), o.size()));
    return o;
}
std::string spyHello() { return text([](uint8_t* b, size_t n) { return onair_net_spy_hello(b, n); }); }
std::string spySetSetting(uint32_t id, uint32_t v) { return text([&](uint8_t* b, size_t n) { return onair_net_spy_set_setting(id, v, b, n); }); }
bool spyHeader(const unsigned char* h, uint32_t& type, uint32_t& flags, uint32_t& size) { return onair_net_spy_header(U(h), 20, &type, &flags, &size); }
std::vector<double> spyRates(uint32_t maxRate, uint32_t decStages, uint32_t minDec) {
    double r[20];
    const size_t n = onair_net_spy_rates(maxRate, decStages, minDec, r, 20);
    return std::vector<double>(r, r + std::min<size_t>(n, 20));
}
std::string spyDevName(uint32_t t) { return text([&](uint8_t* b, size_t n) { return onair_net_spy_dev_name(t, b, n); }); }

bool sdrppParseUi(const unsigned char* p, size_t n, std::vector<UiWidget>& out) {
    out.clear();
    std::vector<OnairNetWidget> w(n / 3 + 1);
    std::vector<OnairNetSpan> it(n + 1);
    std::vector<uint8_t> ar(9 * n + 64);
    OnairNetUiCounts c;
    onair_net_sdrpp_parse_ui(U(p), n, w.data(), w.size(), it.data(), it.size(), ar.data(), ar.size(), &c);
    if (!c.ok) return false;
    for (uint32_t k = 0; k < c.widgets; k++) {
        const OnairNetWidget& s = w[k];
        UiWidget d;
        d.kind = (UiWidget::Kind)s.kind;
        d.id.assign((const char*)p + s.id.off, s.id.len);
        d.label.assign(s.label_in_arena ? (const char*)ar.data() + s.label.off : (const char*)p + s.label.off, s.label.len);
        for (uint32_t q = 0; q < s.items_count; q++) d.items.emplace_back((const char*)p + it[s.items_first + q].off, it[s.items_first + q].len);
        d.i = s.i;
        d.b = s.b != 0;
        d.value = s.value; d.minV = s.min_v; d.maxV = s.max_v; d.step = s.step;
        d.disabled = s.disabled != 0;
        out.push_back(std::move(d));
    }
    return true;
}
unsigned sdrppIdFlags(const std::string& id) { return onair_net_sdrpp_id_flags(U(id), id.size()); }
std::string sdrppLabelFromId(const std::string& id) {
    std::string o(9 * id.size() + 64, '\0');
    const size_t n = onair_net_sdrpp_label_from_id(U(id), id.size(), (uint8_t*)o.data(), o.size());
    o.resize(n == (size_t)-1 ? 0 : n);
    return o;
}
double sdrppRateText(const std::string& t) { return onair_net_sdrpp_rate_text(t.c_str()); }
std::string sdrppCommand(uint32_t cmd, const unsigned char* arg, size_t n) {
    std::string o(12 + n, '\0');
    o.resize(onair_net_sdrpp_command(cmd, U(arg), n, (uint8_t*)o.data(), o.size()));
    return o;
}
std::string sdrppUiAction(const std::string& id, uint8_t type, int32_t i, float f, bool b, bool sync) {
    std::string o(id.size() + 16, '\0');
    o.resize(onair_net_sdrpp_ui_action(U(id), id.size(), type, i, f, b, sync, (uint8_t*)o.data(), o.size()));
    return o;
}
bool sdrppPacketHeader(const unsigned char* h, uint32_t& type, uint32_t& bodySize) { return onair_net_sdrpp_packet_header(U(h), 8, &type, &bodySize); }
bool sdrppErrorText(uint8_t c, std::string& t) {
    char b[32];
    const size_t n = onair_net_sdrpp_error_text(c, (uint8_t*)b, sizeof b);
    if (n == (size_t)-1) return false;
    t.assign(b, n);
    return true;
}

std::vector<Radio> radiosFromString(const std::string& s) {
    std::vector<Radio> v;
    std::vector<OnairNetRadio> r((size_t)std::count(s.begin(), s.end(), ';') + 1);
    std::vector<uint8_t> ar(s.size() + 1);
    const size_t n = onair_net_radios_parse(U(s), s.size(), r.data(), r.size(), ar.data(), ar.size());
    if (n == (size_t)-1) return v;
    for (size_t k = 0; k < n; k++) {
        Radio o;
        o.kind = r[k].kind;
        o.port = r[k].port;
        o.host.assign((const char*)ar.data() + r[k].host.off, r[k].host.len);
        o.remote = sub(s, r[k].remote);
        v.push_back(std::move(o));
    }
    return v;
}
std::string radiosToString(const std::vector<Radio>& v) {
    std::string pool;
    std::vector<OnairNetRadio> r(v.size());
    for (size_t k = 0; k < v.size(); k++) {
        r[k].kind = v[k].kind;
        r[k].port = v[k].port;
        r[k].host = {(uint32_t)pool.size(), (uint32_t)v[k].host.size()};
        pool += v[k].host;
        r[k].remote = {(uint32_t)pool.size(), (uint32_t)v[k].remote.size()};
        pool += v[k].remote;
    }
    std::string out;
    size_t n = onair_net_radios_format(r.data(), r.size(), U(pool), pool.size(), nullptr, 0);
    out.resize(n);
    onair_net_radios_format(r.data(), r.size(), U(pool), pool.size(), (uint8_t*)out.data(), out.size());
    return out;
}
std::string cleanHost(const std::string& h) {
    std::string o(h.size(), '\0');
    const size_t n = onair_net_radio_clean_host(U(h), h.size(), (uint8_t*)o.data(), o.size());
    o.resize(n == (size_t)-1 ? 0 : n);
    return o;
}
std::string radioTypeName(uint32_t t) { return text([&](uint8_t* b, size_t n) { return onair_net_radio_type_name(t, b, n); }); }
int radioDefaultPort(uint32_t t) { return onair_net_radio_default_port(t); }
std::string radioDefaultHost(uint32_t t) { return text([&](uint8_t* b, size_t n) { return onair_net_radio_default_host(t, b, n); }); }
int radioPort(uint32_t t, int port) { return onair_net_radio_port(t, port); }

} // namespace dect2::netp
