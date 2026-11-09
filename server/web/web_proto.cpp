#include "web_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace onair_web {

static const char* const kDemodNames[] = {"am", "nfm", "wfm", "usb", "lsb", "cw"};
const char* demodName(Demod d) { const int i = (int)d; return i >= 0 && i < (int)Demod::Count ? kDemodNames[i] : "nfm"; }
bool demodFromName(const std::string& s, Demod& d) {
    for (int i = 0; i < (int)Demod::Count; i++) if (s == kDemodNames[i]) { d = (Demod)i; return true; }
    return false;
}

bool isTxType(const std::string& t) {
    static const std::set<std::string> f = {"tx", "transmit", "ptt", "arm", "tx_arm", "carrier"};
    return f.count(t) > 0;
}

bool isForbiddenType(const std::string& t) {
    static const std::set<std::string> f = {"tx", "transmit", "ptt", "arm", "tx_arm", "carrier", "key", "keys", "import_key", "key_import",
                                            "voicekeys", "decrypt", "record", "recording", "save", "upload", "exec", "shell", "config", "admin"};
    return f.count(t) > 0;
}

const std::vector<std::string>& clientTypes() {
    static const std::vector<std::string> v = {"hello", "tune", "rx", "audio", "spectrum", "subscribe", "unsubscribe", "ping", "bye", "control"};
    return v;
}

namespace {
struct V {
    const Json& o;
    std::string& code;
    std::string& why;
    std::set<std::string> seen;
    bool bad(const char* c, const std::string& w) { code = c; why = w; return false; }
    const Json* field(const char* k) { seen.insert(k); return o.get(k); }
    bool num(const char* k, double lo, double hi, double& out, bool& has, bool required = false) {
        const Json* j = field(k);
        if (!j) { has = false; return required ? bad(err::BadMessage, std::string("missing ") + k) : true; }
        if (j->kind != Json::Num) return bad(err::BadMessage, std::string(k) + " must be a number");
        if (j->num < lo || j->num > hi) return bad(err::OutOfRange, std::string(k) + " out of range");
        out = j->num; has = true;
        return true;
    }
    bool integer(const char* k, int lo, int hi, int& out, bool required = false) {
        double v = 0; bool has = false;
        if (!num(k, lo, hi, v, has, required)) return false;
        if (has) { if (v != std::floor(v)) return bad(err::BadMessage, std::string(k) + " must be a whole number"); out = (int)v; }
        return true;
    }
    bool str(const char* k, size_t maxLen, std::string& out, bool& has, bool required = false) {
        const Json* j = field(k);
        if (!j) { has = false; return required ? bad(err::BadMessage, std::string("missing ") + k) : true; }
        if (j->kind != Json::Str) return bad(err::BadMessage, std::string(k) + " must be a string");
        if (j->str.size() > maxLen) return bad(err::OutOfRange, std::string(k) + " too long");
        out = j->str; has = true;
        return true;
    }
    bool boolean(const char* k, bool& out, bool required) {
        const Json* j = field(k);
        if (!j) return required ? bad(err::BadMessage, std::string("missing ") + k) : true;
        if (j->kind != Json::Bool) return bad(err::BadMessage, std::string(k) + " must be true or false");
        out = j->b;
        return true;
    }
    bool noOthers() {
        for (const auto& kv : o.obj) if (!seen.count(kv.first)) return bad(err::BadMessage, "unknown field " + kv.first.substr(0, 32));
        return true;
    }
};
bool idChars(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
// a display name: printable, no markup characters; trimmed
std::string cleanName(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '&' || c == '"' || c == '\'' || c == '`') continue;
        o += (char)c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    return o;
}
} // namespace

bool parseClientMsg(const std::string& text, ClientMsg& m, std::string& code, std::string& why, bool allowTx) {
    m = ClientMsg();
    if (text.size() > kMaxClientMsg) { code = err::BadMessage; why = "message too long"; return false; }
    Json j;
    std::string perr;
    if (!jsonParse(text, j, perr)) { code = err::BadJson; why = perr; return false; }
    if (j.kind != Json::Obj) { code = err::BadMessage; why = "a message is a JSON object"; return false; }
    V v{j, code, why, {}};
    std::string type; bool has = false;
    if (!v.str("type", 24, type, has, true)) return false;
    if (allowTx && isTxType(type)) {   // the host set web_allow_tx=1: the message is handed on as it came (the host's handler reads it)
        m.type = ClientMsg::Tx;
        m.txType = type;
        m.txRaw = text;
        return true;
    }
    if (isForbiddenType(type)) { code = err::Forbidden; why = "not available to web listeners"; return false; }
    double d = 0; bool hd = false;
    if (type == "hello") {
        m.type = ClientMsg::Hello;
        if (!v.integer("proto", 0, 1000, m.proto, true)) return false;
        if (m.proto != kProtoVersion) return v.bad(err::BadMessage, "unsupported protocol version");
        if (!v.str("password", 128, m.password, has)) return false;
        if (!v.str("invite", 128, m.invite, has)) return false;
        std::string name;
        if (!v.str("name", 4 * kMaxNameLen, name, has)) return false;
        m.name = cleanName(name);
        if (m.name.size() > (size_t)kMaxNameLen) m.name.resize(kMaxNameLen);
        // never cut inside a UTF-8 sequence
        while (!m.name.empty() && ((unsigned char)m.name.back() & 0xC0) == 0x80) m.name.pop_back();
        if (!m.name.empty() && (unsigned char)m.name.back() >= 0xC0) m.name.pop_back();
    } else if (type == "control") {
        m.type = ClientMsg::Control;
        const Json* j = v.field("take");
        if (!j || j->kind != Json::Bool) return v.bad(err::BadMessage, "take must be true or false");
        m.take = j->b;
    } else if (type == "tune") {
        m.type = ClientMsg::Tune;
        if (!v.num("freq_hz", 0, 1e11, m.freqHz, m.hasFreq)) return false;
        std::string mode;
        if (!v.str("mode", 8, mode, has)) return false;
        if (has) { if (!demodFromName(mode, m.demod)) return v.bad(err::OutOfRange, "unknown mode"); m.hasDemod = true; }
        bool hl = false, hh = false;
        if (!v.num("low_hz", -150000, 150000, m.lowHz, hl)) return false;
        if (!v.num("high_hz", -150000, 150000, m.highHz, hh)) return false;
        if (hl != hh) return v.bad(err::BadMessage, "low_hz and high_hz go together");
        if (hl) { if (m.highHz - m.lowHz < 50) return v.bad(err::OutOfRange, "passband narrower than 50 Hz"); m.hasPass = true; }
        if (!m.hasFreq && !m.hasDemod && !m.hasPass) return v.bad(err::BadMessage, "tune needs freq_hz, mode or a passband");
    } else if (type == "rx") {
        m.type = ClientMsg::Rx;
        if (!v.num("squelch_db", -200, 0, m.squelchDb, m.hasSquelch)) return false;
        std::string agc;
        if (!v.str("agc", 8, agc, has)) return false;
        if (has) {
            if (agc == "off") m.agc = AgcMode::Off; else if (agc == "slow") m.agc = AgcMode::Slow; else if (agc == "fast") m.agc = AgcMode::Fast;
            else return v.bad(err::OutOfRange, "agc is off, slow or fast");
            m.hasAgc = true;
        }
        if (!v.num("gain_db", -20, 60, m.gainDb, m.hasGain)) return false;
        if (!v.num("deemph_us", 0, 75, m.deemphUs, m.hasDeemph)) return false;
        if (m.hasDeemph && m.deemphUs != 0 && m.deemphUs != 50 && m.deemphUs != 75) return v.bad(err::OutOfRange, "deemph_us is 0, 50 or 75");
    } else if (type == "audio") {
        m.type = ClientMsg::Audio;
        if (!v.boolean("on", m.on, true)) return false;
        std::string c;
        if (!v.str("codec", 8, c, has)) return false;
        if (has) { if (c == "pcm16") m.codec = AudioCodec::Pcm16; else if (c == "adpcm") m.codec = AudioCodec::Adpcm; else return v.bad(err::OutOfRange, "codec is adpcm or pcm16"); }
        if (!v.integer("rate", 8000, 48000, m.rateHz)) return false;
        if (m.rateHz != 8000 && m.rateHz != 12000 && m.rateHz != 16000 && m.rateHz != 24000 && m.rateHz != 48000) return v.bad(err::OutOfRange, "rate is 8000, 12000, 16000, 24000 or 48000");
    } else if (type == "spectrum") {
        m.type = ClientMsg::Spectrum;
        if (!v.boolean("on", m.on, true)) return false;
        if (!v.integer("bins", 64, 4096, m.bins)) return false;
        if (!v.integer("fps", 1, 30, m.fps)) return false;
        bool hs = false, he = false;
        if (!v.num("start_hz", 0, 1e11, m.startHz, hs)) return false;
        if (!v.num("stop_hz", 0, 1e11, m.stopHz, he)) return false;
        if (hs != he) return v.bad(err::BadMessage, "start_hz and stop_hz go together");
        if (hs) { if (m.stopHz - m.startHz < 1000) return v.bad(err::OutOfRange, "span narrower than 1 kHz"); m.hasSpan = true; }
    } else if (type == "subscribe" || type == "unsubscribe") {
        m.type = type == "subscribe" ? ClientMsg::Subscribe : ClientMsg::Unsubscribe;
        if (!v.str("decoder", 24, m.decoder, has, true)) return false;
        if (!idChars(m.decoder)) return v.bad(err::BadMessage, "decoder is a lower-case id");
        if (!v.num("freq_hz", 0, 1e11, m.decFreqHz, m.hasDecFreq)) return false;
    } else if (type == "ping") {
        m.type = ClientMsg::Ping;
        if (!v.num("t", -1e15, 1e15, d, hd)) return false;
        m.t = d;
    } else if (type == "bye") {
        m.type = ClientMsg::Bye;
    } else {
        code = err::UnknownType; why = "unknown message type";
        return false;
    }
    return v.noOthers();
}

// ---------------------------------------------------------------- WebSocket
WsIn wsParse(const std::string& in, size_t maxPayload) {
    WsIn r;
    if (in.size() < 2) return r;
    const uint8_t b0 = (uint8_t)in[0], b1 = (uint8_t)in[1];
    const bool fin = b0 & 0x80;
    const int op = b0 & 0x0F;
    if (b0 & 0x70) { r.kind = 1; r.closeCode = 1002; return r; }   // no extensions were negotiated
    if (!(b1 & 0x80)) { r.kind = 1; r.closeCode = 1002; return r; } // client frames must be masked
    const bool control = op >= 8;
    if (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10) { r.kind = 1; r.closeCode = 1002; return r; }
    if (control && !fin) { r.kind = 1; r.closeCode = 1002; return r; }
    if (!control && (!fin || op == 0)) { r.kind = 1; r.closeCode = 1003; return r; }   // fragmented messages are not accepted
    uint64_t len = b1 & 0x7F;
    size_t p = 2;
    if (len == 126) { if (in.size() < 4) return r; len = ((uint64_t)(uint8_t)in[2] << 8) | (uint8_t)in[3]; p = 4; }
    else if (len == 127) {
        if (in.size() < 10) return r;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)in[2 + i];
        p = 10;
    }
    if (control && len > 125) { r.kind = 1; r.closeCode = 1002; return r; }
    if (len > maxPayload) { r.kind = 1; r.closeCode = 1009; return r; }
    if (in.size() < p + 4 + len) return r;
    const uint8_t* mk = (const uint8_t*)in.data() + p;
    p += 4;
    r.payload.resize((size_t)len);
    for (size_t i = 0; i < len; i++) r.payload[i] = (char)((uint8_t)in[p + i] ^ mk[i & 3]);
    r.kind = 2; r.op = op; r.used = p + (size_t)len;
    return r;
}

static std::string frameHead(int opcode, size_t n, bool masked) {
    std::string h;
    h += (char)(0x80 | (opcode & 0x0F));
    const uint8_t m = masked ? 0x80 : 0;
    if (n < 126) h += (char)(m | n);
    else if (n < 65536) { h += (char)(m | 126); h += (char)(n >> 8); h += (char)(n & 0xFF); }
    else { h += (char)(m | 127); for (int i = 7; i >= 0; i--) h += (char)((uint64_t)n >> (8 * i)); }
    return h;
}
std::string wsServerFrame(int opcode, const std::string& payload) { return frameHead(opcode, payload.size(), false) + payload; }
std::string wsCloseFrame(int code, const std::string& reason) {
    std::string p;
    p += (char)(code >> 8); p += (char)(code & 0xFF);
    p += reason.substr(0, 120);
    return wsServerFrame(8, p);
}
std::string wsMaskedFrame(int opcode, const std::string& payload, uint32_t key) {
    std::string f = frameHead(opcode, payload.size(), true);
    const uint8_t mk[4] = {(uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
    f.append((const char*)mk, 4);
    for (size_t i = 0; i < payload.size(); i++) f += (char)((uint8_t)payload[i] ^ mk[i & 3]);
    return f;
}

// SHA-1 (RFC 3174): only for the WebSocket handshake, where it is what the RFC asks for (not a security function here)
std::string sha1Raw(const std::string& data) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = data;
    const uint64_t bits = (uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (8 * i));
    auto rol = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (size_t blk = 0; blk < m.size(); blk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)(uint8_t)m[blk + 4 * i] << 24) | ((uint32_t)(uint8_t)m[blk + 4 * i + 1] << 16) | ((uint32_t)(uint8_t)m[blk + 4 * i + 2] << 8) | (uint8_t)m[blk + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t x : h) for (int i = 3; i >= 0; i--) out += (char)(x >> (8 * i));
    return out;
}

std::string base64Enc(const std::string& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        const uint32_t v = ((uint32_t)(uint8_t)d[i] << 16) | ((uint32_t)(uint8_t)d[i + 1] << 8) | (uint8_t)d[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == d.size()) { const uint32_t v = (uint32_t)(uint8_t)d[i] << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == d.size()) { const uint32_t v = ((uint32_t)(uint8_t)d[i] << 16) | ((uint32_t)(uint8_t)d[i + 1] << 8); o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '='; }
    return o;
}

std::string wsAccept(const std::string& key) { return base64Enc(sha1Raw(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")); }

bool ctEq(const std::string& a, const std::string& b) {
    // compare over the longer length so the time does not depend on where the strings differ
    const size_t n = std::max(a.size(), b.size());
    unsigned diff = (unsigned)(a.size() ^ b.size());
    for (size_t i = 0; i < n; i++) {
        const unsigned char x = i < a.size() ? (unsigned char)a[i] : 0, y = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(x ^ y);
    }
    return diff == 0 && !a.empty();
}

// ---------------------------------------------------------------- binary frames
static void putU16(std::string& s, uint16_t v) { s += (char)(v & 0xFF); s += (char)(v >> 8); }
static void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s += (char)(v >> (8 * i)); }
static void putF32(std::string& s, float f) { uint32_t v; std::memcpy(&v, &f, 4); putU32(s, v); }
static void putF64(std::string& s, double f) { uint64_t v; std::memcpy(&v, &f, 8); for (int i = 0; i < 8; i++) s += (char)(v >> (8 * i)); }
static uint32_t getU32(const std::string& s, size_t p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)s[p + i] << (8 * i); return v; }

std::string spectrumFrame(uint32_t seq, double startHz, double stopHz, float floorDb, float topDb, const std::vector<uint8_t>& bins) {
    std::string f;
    f.reserve(kSpectrumHeader + bins.size());
    f += (char)kFrameSpectrum; f += (char)1;
    putU16(f, (uint16_t)bins.size());
    putU32(f, seq);
    putF64(f, startHz); putF64(f, stopHz);
    putF32(f, floorDb); putF32(f, topDb);
    f.append((const char*)bins.data(), bins.size());
    return f;
}

static const int kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
                              157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
                              1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
                              10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int kIdx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int adpcmNibble(int x, AdpcmState& st) {
    const int step = kStep[st.index];
    int diff = x - st.pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int dq = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; dq += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; dq += step >> 1; }
    if (diff >= step >> 2) { code |= 1; dq += step >> 2; }
    st.pred += (code & 8) ? -dq : dq;
    st.pred = std::clamp(st.pred, -32768, 32767);
    st.index = std::clamp(st.index + kIdx[code], 0, 88);
    return code;
}
static int adpcmDec(int code, AdpcmState& st) {
    const int step = kStep[st.index];
    int dq = step >> 3;
    if (code & 4) dq += step;
    if (code & 2) dq += step >> 1;
    if (code & 1) dq += step >> 2;
    st.pred += (code & 8) ? -dq : dq;
    st.pred = std::clamp(st.pred, -32768, 32767);
    st.index = std::clamp(st.index + kIdx[code], 0, 88);
    return st.pred;
}
void adpcmEncode(const int16_t* x, size_t n, AdpcmState& st, std::string& out) {
    for (size_t i = 0; i < n; i += 2) {
        const int lo = adpcmNibble(x[i], st);
        const int hi = i + 1 < n ? adpcmNibble(x[i + 1], st) : 0;
        out += (char)(lo | (hi << 4));
    }
}
void adpcmDecode(const uint8_t* b, size_t nSamples, AdpcmState& st, std::vector<int16_t>& out) {
    for (size_t i = 0; i < nSamples; i++) {
        const int code = (i & 1) ? (b[i >> 1] >> 4) : (b[i >> 1] & 0x0F);
        out.push_back((int16_t)adpcmDec(code, st));
    }
}

std::string audioFrame(uint32_t seq, AudioCodec codec, int rateHz, float levelDb, bool squelched, const std::vector<int16_t>& pcm, AdpcmState& st) {
    std::string f;
    f.reserve(kAudioHeader + pcm.size() * 2);
    f += (char)kFrameAudio; f += (char)(int)codec;
    putU16(f, (uint16_t)std::min<size_t>(pcm.size(), 65535));
    putU32(f, seq);
    putU32(f, (uint32_t)rateHz);
    putF32(f, levelDb);
    f += (char)(squelched ? 1 : 0); f += (char)0;
    putU16(f, (uint16_t)(int16_t)st.pred);   // the decoder's state at the start of this frame: every frame decodes on its own
    f += (char)st.index; f += (char)0;
    putU16(f, 0);
    if (squelched) return f;
    const size_t n = std::min<size_t>(pcm.size(), 65535);
    if (codec == AudioCodec::Pcm16) { for (size_t i = 0; i < n; i++) putU16(f, (uint16_t)pcm[i]); }
    else adpcmEncode(pcm.data(), n, st, f);
    return f;
}

bool parseAudioFrame(const std::string& f, AudioCodec& codec, int& rateHz, bool& squelched, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (f.size() < kAudioHeader || (uint8_t)f[0] != kFrameAudio) return false;
    const int c = (uint8_t)f[1];
    if (c > 1) return false;
    codec = (AudioCodec)c;
    const size_t n = (uint8_t)f[2] | ((size_t)(uint8_t)f[3] << 8);
    rateHz = (int)getU32(f, 8);
    squelched = f[16] & 1;
    AdpcmState st;
    st.pred = (int16_t)((uint8_t)f[18] | ((uint16_t)(uint8_t)f[19] << 8));
    st.index = (uint8_t)f[20];
    if (st.index > 88) return false;
    if (squelched) return f.size() == kAudioHeader;
    if (codec == AudioCodec::Pcm16) {
        if (f.size() != kAudioHeader + 2 * n) return false;
        for (size_t i = 0; i < n; i++) pcm.push_back((int16_t)((uint8_t)f[kAudioHeader + 2 * i] | ((uint16_t)(uint8_t)f[kAudioHeader + 2 * i + 1] << 8)));
    } else {
        if (f.size() != kAudioHeader + (n + 1) / 2) return false;
        adpcmDecode((const uint8_t*)f.data() + kAudioHeader, n, st, pcm);
    }
    return true;
}

} // namespace onair_web
