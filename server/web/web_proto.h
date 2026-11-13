// OnAir Web protocol, version 1 (docs/ONAIR-WEB-PROTOCOL.md is the specification; keep the two in step). The pure part with no sockets:
// the client messages and their validation, the WebSocket framing (RFC 6455) both ways, the binary spectrum and audio frames, the IMA ADPCM
// audio codec and the SHA-1 / base64 of the handshake. Everything a browser sends goes through parseClientMsg() before anything acts on it.
#pragma once
#include "web_json.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace onair_web {

constexpr int kProtoVersion = 1;
constexpr const char* kSubprotocol = "onair-web.v1";
constexpr size_t kMaxClientMsg = 4096;       // bytes of one client text message (bigger: close 1009)
constexpr size_t kMaxHttpHead = 8192;        // request head (bigger: 431)
constexpr size_t kMaxHttpBody = 4096;        // admin POST body (bigger: 413)
constexpr int kMaxNameLen = 32;              // the display name a user may give

// Demodulators of the virtual receiver
enum class Demod { Am, Nfm, Wfm, Usb, Lsb, Cw, Count };
const char* demodName(Demod d);
bool demodFromName(const std::string& s, Demod& d);

enum class AudioCodec { Pcm16 = 0, Adpcm = 1 };
enum class AgcMode { Off, Slow, Fast };

// One validated client message. Only the fields of its type are meaningful; `has` bits tell which optional ones were given.
struct ClientMsg {
    enum Type { Hello, Tune, Rx, Audio, Spectrum, Subscribe, Unsubscribe, Ping, Bye, Tx, Control } type = Ping;
    // Tx: only parsed when the host allows web transmit (web_allow_tx=1); the raw message goes to the host's transmit handler
    std::string txType, txRaw;
    // hello
    int proto = 0;
    std::string password, invite, name;
    // tune
    double freqHz = 0; bool hasFreq = false;
    Demod demod = Demod::Nfm; bool hasDemod = false;
    double lowHz = 0, highHz = 0; bool hasPass = false;   // passband edges relative to the tuned frequency (lowHz < highHz)
    // rx
    double squelchDb = 0; bool hasSquelch = false;        // channel level (dBFS) under which the audio is muted; <= -150: open
    AgcMode agc = AgcMode::Slow; bool hasAgc = false;
    double gainDb = 0; bool hasGain = false;              // manual audio gain with AGC off (-20 .. 60)
    double deemphUs = 50; bool hasDeemph = false;         // WFM de-emphasis: 0 (off), 50, 75
    // audio
    bool on = false;
    AudioCodec codec = AudioCodec::Adpcm;
    int rateHz = 12000;
    // spectrum
    int bins = 1024, fps = 10;
    double startHz = 0, stopHz = 0; bool hasSpan = false; // absolute window inside the capture (0/0: the whole capture)
    // subscribe / unsubscribe
    std::string decoder;
    double decFreqHz = 0; bool hasDecFreq = false;
    // ping
    double t = 0;
    // control: take (true) or give back (false) the time-limited control slot
    bool take = false;
};

// Error codes sent to the client in {"type":"error","code":...}
namespace err {
constexpr const char* BadJson = "bad_json";
constexpr const char* BadMessage = "bad_message";
constexpr const char* UnknownType = "unknown_type";
constexpr const char* Forbidden = "forbidden";          // transmit, keys, recording ... never available to web sessions
constexpr const char* OutOfRange = "out_of_range";
constexpr const char* ForbiddenRange = "forbidden_range";   // the host limits the frequency ranges: this tuning / decoder is outside them (or blocked)
constexpr const char* NotAllowed = "not_allowed";       // the host has not enabled this
constexpr const char* RateLimited = "rate_limited";
constexpr const char* Busy = "busy";
constexpr const char* Auth = "auth";
constexpr const char* NotRunning = "not_running";     // the host's receiver delivers no samples (stopped, starting, or its start failed: the text says which)
}

// The message types a web session may not use unless the host allows them (transmit / arm: web_allow_tx=1; keys / decryption / recording never): answered with err::Forbidden.
bool isForbiddenType(const std::string& type);
// The message types a session can send at all, for the documentation test.
const std::vector<std::string>& clientTypes();

// Parses and validates. False: code (one of err::*) and text say why; the message must not be acted on.
// allowTx: the host set web_allow_tx=1, so the transmit message types (tx, transmit, ptt, arm, tx_arm, carrier) parse as ClientMsg::Tx instead of "forbidden".
bool parseClientMsg(const std::string& text, ClientMsg& m, std::string& code, std::string& why, bool allowTx = false);
bool isTxType(const std::string& type);

// ---------------------------------------------------------------- WebSocket framing
// The first client frame of `in`. kind 0: need more bytes; 1: protocol error, close with `closeCode` (1002 unmasked / reserved bits / bad
// control frame, 1003 a fragmented message, 1009 too big); 2: a whole frame (`used` bytes, opcode, unmasked payload).
struct WsIn { int kind = 0, closeCode = 0, op = 0; size_t used = 0; std::string payload; };
WsIn wsParse(const std::string& in, size_t maxPayload = kMaxClientMsg);
std::string wsServerFrame(int opcode, const std::string& payload);   // FIN set, never masked
std::string wsCloseFrame(int code, const std::string& reason);
std::string wsMaskedFrame(int opcode, const std::string& payload, uint32_t key);   // a client frame (tests and the load tool)

std::string sha1Raw(const std::string& data);   // 20 bytes
std::string base64Enc(const std::string& data);
std::string wsAccept(const std::string& clientKey);
bool ctEq(const std::string& a, const std::string& b);   // constant time for equal lengths; the length is not secret

// ---------------------------------------------------------------- binary server frames (little endian)
// Spectrum row: [0x10][ver=1][u16 bins][u32 seq][f64 startHz][f64 stopHz][f32 floorDb][f32 topDb][bins x u8, 0 = floor, 255 = top]
constexpr uint8_t kFrameSpectrum = 0x10;
// Audio: [0x20][codec][u16 samples][u32 seq][u32 rateHz][f32 levelDbfs][u8 flags (1 = squelched: no payload)][u8 0][i16 adpcmPred][u8 adpcmIndex][u8 0][u16 0][payload]
constexpr uint8_t kFrameAudio = 0x20;
constexpr size_t kSpectrumHeader = 32, kAudioHeader = 24;
std::string spectrumFrame(uint32_t seq, double startHz, double stopHz, float floorDb, float topDb, const std::vector<uint8_t>& bins);
struct AdpcmState { int pred = 0, index = 0; };
std::string audioFrame(uint32_t seq, AudioCodec codec, int rateHz, float levelDb, bool squelched, const std::vector<int16_t>& pcm, AdpcmState& st);
bool parseAudioFrame(const std::string& f, AudioCodec& codec, int& rateHz, bool& squelched, std::vector<int16_t>& pcm);   // tests and the load tool

// IMA ADPCM (4 bits a sample, two samples a byte, low nibble first)
void adpcmEncode(const int16_t* x, size_t n, AdpcmState& st, std::string& out);
void adpcmDecode(const uint8_t* b, size_t nSamples, AdpcmState& st, std::vector<int16_t>& out);

} // namespace onair_web
