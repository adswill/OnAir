// OnAir <-> ESP32 serial wire protocol, version 1. One header, no dependencies, shared by the firmware and by OnAir (core/include/dect2/esp32/esp32_wire.h
// includes this very file, so the two sides cannot drift). Licence: GPL-3.0-or-later, like OnAir. Description: docs/ESP32-NRF24-DESIGN.md.
//
// Frame:  0xA5 | len (u16 LE, payload bytes only) | type (u8) | seq (u8) | payload[len] | crc (u16 LE)
// crc = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) over len, type, seq and payload.
// A receiver that sees a bad CRC or an impossible length drops one byte and looks for the next 0xA5, so it resynchronises after any damage.
// Integers in payloads are little endian. A reply to a request carries the request's seq; unsolicited frames from the device use seq 0.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace onair {
namespace wire {

constexpr uint8_t kSync = 0xA5;
constexpr uint8_t kProtoVersion = 1;
constexpr size_t kMaxPayload = 512;
constexpr size_t kOverhead = 1 + 2 + 1 + 1 + 2;
constexpr size_t kMaxFrame = kOverhead + kMaxPayload;

// host -> device
enum : uint8_t {
    kHello = 0x01,      // u8 host protocol version
    kConfig = 0x02,     // u8 radio, then the radio's parameters (below)
    kStart = 0x03,      // u8 radio: receive on
    kStop = 0x04,       // nothing: everything off
    kTx = 0x05,         // u8 radio, then the radio's transmit parameters (below)
    kPing = 0x06,       // u32 token
    kTxEnable = 0x07,   // u8 0/1: the transmit gate of this session (off after every connect); the user's Arm button
};
// device -> host
enum : uint8_t {
    kHelloAck = 0x81,   // u8 protocol version, u8 fw major, u8 fw minor, u8 chip, u8 radios mask, u8 name length, name
    kAck = 0x82,        // u8 type of the request, u8 status
    kPong = 0x86,       // u32 token
    kRxNrf24 = 0x90,    // below
    kRxBle = 0x91,
    kRxIeee802154 = 0x92,
    kRxWifi = 0x93,
    kTxDone = 0x94,     // u8 radio, u8 result (0 sent / acknowledged, 1 no acknowledge, 2 error)
    kLog = 0xA0,        // text
    kStats = 0xA1,      // u32 uptime ms, u32 frames received, u32 dropped
};

enum Radio : uint8_t { kRadioNrf24 = 1, kRadioBle = 2, kRadioIeee802154 = 3, kRadioWifi = 4 };
enum RadioBit : uint8_t { kBitNrf24 = 1, kBitBle = 2, kBitIeee802154 = 4, kBitWifi = 8 };
enum Chip : uint8_t { kChipUnknown = 0, kChipEsp32 = 1, kChipEsp32S3 = 2, kChipEsp32C3 = 3, kChipEsp32C6 = 4, kChipEsp32H2 = 5, kChipEsp32S2 = 6, kChipSimulator = 255 };
enum Status : uint8_t { kOk = 0, kUnsupported = 1, kBadParams = 2, kHwFail = 3, kTxDisabled = 4, kBusy = 5, kBadVersion = 6 };

// ---- nRF24 (a module on the ESP32's SPI bus), kConfig payload, 15 bytes after the radio byte:
//   channel u8 (0..125, 2400 + N MHz) | rate u8 (0 = 250 kbit/s, 1 = 1 Mbit/s, 2 = 2 Mbit/s) | addrWidth u8 (3..5) | address[5] (first byte first, the
//   first addrWidth used: receive pipe 0 and the transmit address) | crc u8 (0 off, 1 CRC-8, 2 CRC-16) | autoAck u8 (0/1) | retryDelay u8 (0..15, 250 us steps)
//   | retryCount u8 (0..15) | payloadLen u8 (1..32 static, 0 = dynamic payload length) | power u8 (0..3 = -18, -12, -6, 0 dBm) | promisc u8 (0 normal,
//   1 promiscuous: the chip is set to the illegal 2-byte address 0xAA55 with no CRC and the raw bytes after it are reported, the host looks for frames)
// ---- nRF24 kTx payload: radio u8 | flags u8 (bit 0 = send without acknowledge) | len u8 (0..32) | data[len]
constexpr size_t kNrf24ConfigLen = 1 + 15;
// ---- kRxNrf24: t_ms u32 | flags u8 (bit 0 = raw, the module's bytes after 0xAA55, not a decoded frame) | rate u8 | channel u8 | addrLen u8 | pid u8 | noAck u8
//   | crcBits u8 | len u8 | address[addrLen] | data[len]
constexpr size_t kRxNrf24Head = 4 + 8;

// ---- BLE (the ESP32's own radio): kConfig: radio | mode u8 (0 = scan advertising, 1 = advertise) | channelMask u8 (bit 0..2 = 37, 38, 39) | active u8 (scan requests)
//   | power u8 (0..7, chip steps) ; kTx: radio | advType u8 (0 ADV_IND, 2 ADV_SCAN_IND, 3 ADV_NONCONN_IND) | addr[6] | len u8 (0..31) | data[len]
// ---- kRxBle: t_ms u32 | rssi i8 | channel u8 (37..39) | pduType u8 | addr[6] | len u8 | data[len]
// ---- 802.15.4 (ESP32-C6 / H2 only): kConfig: radio | channel u8 (11..26) | promisc u8 ; kTx: radio | len u8 (1..125) | frame[len]
// ---- kRxIeee802154: t_ms u32 | rssi i8 | channel u8 | len u8 | frame[len] (without the CRC)
// ---- WiFi (observation only: it never transmits, no deauthentication, no capture of keys or passwords). kConfig: radio | mode u8 (0 = listen: promiscuous
//   sniffer for management and data headers, 1 = scan: active access point scan like a phone does) | channel u8 (1..14, 0 = hop over all) | hopMs u16 (dwell,
//   listen mode) ; kTx for WiFi is refused (kUnsupported).
// ---- kRxWifi: t_ms u32 | rssi i8 | channel u8 | kind u8 (0 beacon / scan result, 1 probe request, 2 probe response, 3 deauthentication, 4 disassociation,
//   5 data frame: a client talking to an access point) | mac[6] (transmitter) | bssid[6] | reason u16 (kind 3, 4) | auth u8 (0 open, 1 WEP, 2 WPA, 3 WPA2,
//   4 WPA/WPA2, 5 WPA3, 6 WPA2/WPA3, 7 enterprise, 255 unknown / not a beacon) | ssidLen u8 | ssid[ssidLen]

inline uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// one frame into out (at least kMaxFrame bytes); returns its length, 0 when the payload is too long
inline size_t encode(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len, uint8_t* out) {
    if (len > kMaxPayload) return 0;
    out[0] = kSync; out[1] = (uint8_t)(len & 0xFF); out[2] = (uint8_t)(len >> 8); out[3] = type; out[4] = seq;
    for (size_t i = 0; i < len; i++) out[5 + i] = payload[i];
    const uint16_t c = crc16(out + 1, 4 + len);
    out[5 + len] = (uint8_t)(c & 0xFF); out[6 + len] = (uint8_t)(c >> 8);
    return kOverhead + len;
}

// Byte-at-a-time parser. feed() returns true when a whole good frame has just been completed; type/seq/payload/len then describe it until the next feed().
class Parser {
public:
    bool feed(uint8_t b) {
        if (done_) { done_ = false; n_ = 0; }
        if (n_ == 0) { if (b != kSync) return false; buf_[n_++] = b; return false; }
        buf_[n_++] = b;
        if (n_ == 3) {
            const size_t len = buf_[1] | ((size_t)buf_[2] << 8);
            if (len > kMaxPayload) return resync();
        }
        if (n_ >= 3) {
            const size_t len = buf_[1] | ((size_t)buf_[2] << 8);
            if (n_ == kOverhead + len) {
                const uint16_t want = (uint16_t)(buf_[n_ - 2] | (buf_[n_ - 1] << 8));
                if (crc16(buf_ + 1, n_ - 3) == want) { done_ = true; return true; }
                bad_++; return resync();
            }
        }
        return false;
    }
    uint8_t type() const { return buf_[3]; }
    uint8_t seq() const { return buf_[4]; }
    const uint8_t* payload() const { return buf_ + 5; }
    size_t length() const { return buf_[1] | ((size_t)buf_[2] << 8); }
    uint32_t badFrames() const { return bad_; }
    void reset() { n_ = 0; done_ = false; }
private:
    // drop the first byte and replay the rest, looking for another 0xA5; true when the replay completed a good frame
    bool resync() {
        uint8_t tmp[kMaxFrame];
        const size_t m = n_;
        for (size_t i = 1; i < m; i++) tmp[i - 1] = buf_[i];
        n_ = 0;
        for (size_t i = 0; i + 1 < m; i++) { if (feed(tmp[i])) return true; }
        return false;
    }
    uint8_t buf_[kMaxFrame];
    size_t n_ = 0;
    bool done_ = false;
    uint32_t bad_ = 0;
};

} // namespace wire
} // namespace onair
