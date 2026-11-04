// OnAir ESP32 firmware core: the protocol dispatcher (onair_wire.h), written against a small hardware layer so the very same code runs on the ESP32
// (onair_esp32.ino supplies the real layer) and inside OnAir's loopback test (a fake layer). Licence: GPL-3.0-or-later.
#pragma once
#include "onair_wire.h"
#include <string.h>

namespace onair {

// The hardware layer. Payloads are in the wire format of onair_wire.h (the radio byte is already removed from config / tx payloads).
class Hal {
public:
    virtual ~Hal() {}
    virtual uint32_t millis() = 0;
    virtual void serialWrite(const uint8_t* p, size_t n) = 0;
    virtual uint8_t chip() = 0;
    virtual uint8_t radios() = 0;                       // wire::RadioBit mask of what is present
    virtual const char* name() = 0;
    virtual uint8_t configure(uint8_t radio, const uint8_t* p, size_t n) = 0;   // wire::Status
    virtual uint8_t start(uint8_t radio) = 0;
    virtual void stopAll() = 0;
    virtual uint8_t transmit(uint8_t radio, const uint8_t* p, size_t n, uint8_t& result) = 0;   // result: 0 sent, 1 no acknowledge, 2 error
    // one received packet (kRx* payload) when there is one; radio says which kRx* type it is
    virtual bool pollRx(uint8_t& radio, uint8_t* payload, size_t& len, size_t cap) = 0;
};

class Core {
public:
    explicit Core(Hal& h) : h_(h) {}
    void onByte(uint8_t b) { if (parser_.feed(b)) handle(); }
    // call from loop(): forward received packets, send a statistics frame now and then
    void poll() {
        uint8_t radio, pl[wire::kMaxPayload];
        size_t n = 0;
        for (int i = 0; i < 8; i++) {   // a few per call so the serial input is not starved
            if (!h_.pollRx(radio, pl, n, sizeof pl)) break;
            uint8_t type = radio == wire::kRadioNrf24 ? wire::kRxNrf24 : radio == wire::kRadioBle ? wire::kRxBle
                         : radio == wire::kRadioIeee802154 ? wire::kRxIeee802154 : wire::kRxWifi;
            send(type, 0, pl, n);
            rx_++;
        }
        const uint32_t t = h_.millis();
        if (t - lastStats_ >= 5000) {
            lastStats_ = t;
            uint8_t s[12]; put32(s, t); put32(s + 4, rx_); put32(s + 8, parser_.badFrames());
            send(wire::kStats, 0, s, 12);
        }
    }
    bool txEnabled() const { return txOn_; }
private:
    static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
    void send(uint8_t type, uint8_t seq, const uint8_t* p, size_t n) {
        uint8_t f[wire::kMaxFrame];
        const size_t m = wire::encode(type, seq, p, n, f);
        if (m) h_.serialWrite(f, m);
    }
    void ack(uint8_t seq, uint8_t type, uint8_t status) { uint8_t a[2] = {type, status}; send(wire::kAck, seq, a, 2); }
    static bool nrfValid(const uint8_t* c, size_t n) {   // c: the 18 bytes after the radio byte
        if (n != wire::kNrf24ConfigLen - 1) return false;
        return c[0] <= 125 && c[1] <= 2 && c[2] >= 3 && c[2] <= 5 && c[8] <= 2 && c[9] <= 1 && c[10] <= 15 && c[11] <= 15 && c[12] <= 32 && c[13] <= 3 && c[14] <= 1;
    }
    void handle() {
        const uint8_t type = parser_.type(), seq = parser_.seq();
        const uint8_t* p = parser_.payload();
        const size_t n = parser_.length();
        switch (type) {
        case wire::kHello: {
            if (n < 1 || p[0] == 0) { ack(seq, type, wire::kBadVersion); break; }
            txOn_ = false;   // a new session starts with transmit off
            h_.stopAll();
            const char* nm = h_.name();
            const size_t nl = strlen(nm) > 32 ? 32 : strlen(nm);
            uint8_t r[6 + 32] = {wire::kProtoVersion, 1, 0, h_.chip(), h_.radios(), (uint8_t)nl};
            memcpy(r + 6, nm, nl);
            send(wire::kHelloAck, seq, r, 6 + nl);
            break;
        }
        case wire::kConfig: {
            if (n < 1) { ack(seq, type, wire::kBadParams); break; }
            const uint8_t radio = p[0];
            if (radio < 1 || radio > 4 || !(h_.radios() & (1 << (radio - 1)))) { ack(seq, type, wire::kUnsupported); break; }
            if (radio == wire::kRadioNrf24 && !nrfValid(p + 1, n - 1)) { ack(seq, type, wire::kBadParams); break; }
            ack(seq, type, h_.configure(radio, p + 1, n - 1));
            break;
        }
        case wire::kStart: {
            if (n < 1 || p[0] < 1 || p[0] > 4 || !(h_.radios() & (1 << (p[0] - 1)))) { ack(seq, type, wire::kUnsupported); break; }
            ack(seq, type, h_.start(p[0]));
            break;
        }
        case wire::kStop: h_.stopAll(); ack(seq, type, wire::kOk); break;
        case wire::kTxEnable: txOn_ = n >= 1 && p[0] != 0; ack(seq, type, wire::kOk); break;
        case wire::kTx: {
            if (n < 1) { ack(seq, type, wire::kBadParams); break; }
            const uint8_t radio = p[0];
            if (radio == wire::kRadioWifi) { ack(seq, type, wire::kUnsupported); break; }   // WiFi is observation only
            if (!txOn_) { ack(seq, type, wire::kTxDisabled); break; }
            if (radio < 1 || radio > 4 || !(h_.radios() & (1 << (radio - 1)))) { ack(seq, type, wire::kUnsupported); break; }
            if (radio == wire::kRadioNrf24 && (n < 3 || p[2] > 32 || n != 3u + p[2])) { ack(seq, type, wire::kBadParams); break; }
            uint8_t res = 2;
            const uint8_t st = h_.transmit(radio, p + 1, n - 1, res);
            ack(seq, type, st);
            if (st == wire::kOk) { uint8_t d[2] = {radio, res}; send(wire::kTxDone, seq, d, 2); }
            break;
        }
        case wire::kPing: send(wire::kPong, seq, p, n < 4 ? n : 4); break;
        default: break;   // unknown types are ignored (forward compatibility)
        }
    }
    Hal& h_;
    wire::Parser parser_;
    bool txOn_ = false;
    uint32_t rx_ = 0, lastStats_ = 0;
};

} // namespace onair
