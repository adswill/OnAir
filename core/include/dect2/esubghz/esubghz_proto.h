// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero "Enhanced Sub-GHz Chat" (github.com/twisted-pear/esubghz_chat) and the firmware's own Sub-GHz CLI chat: the radio format and the
// message layer, written for this project from the app's and the firmware's sources (no code copied).
//
// Radio (the firmware's SubGhzTxRxWorker, CC1101 preset FuriHalSubGhzPresetGFSK9_99KbAsync over the chip's reset defaults):
//   GFSK, 9.99 kbaud (MDMCFG4/3 = 0xC8 / 0x93), deviation 19.04 kHz (DEVIATN 0x34), 433.92 MHz unless the user typed another frequency;
//   4 octets of preamble 0xAA (MDMCFG1 reset value), sync word 0x46 0x4C (16 of 16 bits), one length octet, the payload, CRC-16 (polynomial
//   0x8005, start 0xFFFF, over length and payload, sent high octet first); no whitening, no FEC, MSB first, a 1 is the upper frequency.
//   One packet carries at most 60 octets (SUBGHZ_TXRX_WORKER_MAX_TXRX_SIZE): a longer message goes out as several packets about 10 ms apart,
//   and the receiving app joins what arrives until 500 ms pass without a packet (MESSAGE_COMPLETION_TIMEOUT).
// Message (one per joined run of packets, at most 1024 octets):
//   plain:     "<name>: <text>\r\n" (the \r\n is for the CLI chat; the app shows "<name> joined chat." / "<name> left chat." too)
//   encrypted: run_id (8, opaque) | counter (4, big endian) | IV (12) | tag (16) | ciphertext. AES-256-GCM, the 12 header octets
//              (run_id and counter) are the additional data. Key: 32 octets typed as hex, or SHA-256 of a password.
//              A receiver drops a message whose counter is not above the last one it accepted from that run_id (replay guard).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace esubghz {

constexpr double kXtalHz = 26e6;
constexpr double kBaud = kXtalHz * (256 + 0x93) * 256.0 / 268435456.0;   // 9992.6 baud
constexpr double kDeviationHz = kXtalHz / 131072.0 * (8 + 4) * 8;        // 19042.97 Hz
constexpr double kGaussBt = 0.5;                                          // the CC1101's GFSK filter
constexpr double kDefaultHz = 433.92e6;
constexpr uint8_t kSync0 = 0x46, kSync1 = 0x4C;
constexpr int kPreambleOctets = 4;          // what a Flipper sends; OnAir sends kTxPreambleOctets (a CC1101 only needs the sync word)
constexpr int kTxPreambleOctets = 8;
constexpr size_t kMaxChunk = 60;            // payload octets in one packet
constexpr size_t kMaxMessage = 1024;        // the app's RX_TX_BUFFER_SIZE
constexpr double kChunkGapSec = 0.02;       // between the packets of one message (a Flipper: 10 ms or more)
constexpr double kMessageGapSec = 0.5;      // a message ends after this long without a packet
constexpr size_t kRunIdOctets = 8, kCounterOctets = 4, kIvOctets = 12, kTagOctets = 16;
constexpr size_t kHeaderOctets = kRunIdOctets + kCounterOctets;
constexpr size_t kOverhead = kHeaderOctets + kIvOctets + kTagOctets;   // 40
constexpr size_t kMaxText = 256;            // the app's text input (TEXT_INPUT_STORE_SIZE)

// CC1101 CRC-16 (polynomial 0x8005, start 0xFFFF, no reflection, no final XOR)
uint16_t crc16(const uint8_t* p, size_t n);

// The octets of one packet as sent: preamble, sync word, length, payload, CRC. n is 1 .. kMaxChunk.
std::vector<uint8_t> packetOctets(const uint8_t* payload, size_t n, int preambleOctets = kTxPreambleOctets);
// A message split the way the firmware's worker sends it: kMaxChunk octets per packet, the rest in the last one
std::vector<std::vector<uint8_t>> splitMessage(const std::vector<uint8_t>& msg);

// The shared key of an encrypted chat
struct Key {
    bool on = false;
    uint8_t k[32] = {};
};
Key keyFromPassword(const std::string& password);           // SHA-256 of the password's octets (the app's password entry)
bool keyFromHex(const std::string& hex, Key& out);          // 64 hex digits; spaces, ':' and '-' ignored
std::string keyHex(const Key& k);                           // "AB CD EF ..." as the Flipper shows it

// AES-256-GCM with a 96-bit IV (SP 800-38D). out may be in.
void gcmEncrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]);
bool gcmDecrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n, uint8_t* out, const uint8_t tag[16]);

// What goes on the air for one chat line
std::vector<uint8_t> encodePlain(const std::string& line);   // line + "\r\n"
std::vector<uint8_t> encodeEncrypted(const Key& k, uint64_t runId, uint32_t counter, const uint8_t iv[12], const std::string& line);

struct Message {
    bool ok = false;           // text holds the line
    bool encrypted = false;    // it came encrypted (and was decrypted with the key)
    bool looksEncrypted = false;   // not text: long enough for an encrypted message, but no key or the wrong key
    std::string text;          // the line, trailing CR / LF removed
    uint64_t runId = 0;
    uint32_t counter = 0;
};
// The key (nullptr or !on: none) is tried first; a message that does not decrypt is taken as plain text when it is text.
Message decodeMessage(const Key* key, const uint8_t* p, size_t n);
bool isText(const uint8_t* p, size_t n);    // UTF-8 text with no control characters other than tab, CR and LF

// The receiver's replay guard: true (and remembered) when counter is above the last one accepted from runId
class ReplayGuard {
public:
    bool accept(uint64_t runId, uint32_t counter);
    void clear() { last_.clear(); }
private:
    std::map<uint64_t, uint32_t> last_;
};

// The sender of one OnAir chat session: a random run_id, the counter from 1
class Sender {
public:
    Sender();                                   // a fresh run_id from the system's random source
    explicit Sender(uint64_t runId) : runId_(runId) {}
    std::vector<uint8_t> encode(const Key* key, const std::string& line);   // encrypted when the key is on, plain otherwise
    uint64_t runId() const { return runId_; }
    uint32_t counter() const { return counter_; }
private:
    uint64_t runId_ = 0;
    uint32_t counter_ = 1;
};

// The printable form of a received line: control characters become '?', invalid UTF-8 becomes '?'
std::string displayText(const std::string& s);

} // namespace esubghz
} // namespace dect2
