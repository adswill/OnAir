// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL layers: burst structure, scrambler, interleaver, convolutional code, PSK mapping, the PDUs (SPDU squitters, MPDU headers, LPDUs,
// HFNPDUs, ACARS) and the same in reverse for the test signal. Nothing here touches a radio signal.
//
// Facts from ARINC 635 / ICAO Doc 9741 as dumphfdl (szpajder, GPL) implements them, read for reference, not copied:
//   1800 symbols/s, carrier 1440 Hz above the assigned (dial) frequency, upper sideband.
//   TDMA: 32 s frames of 13 slots. Bursts of one slot (1.8 s interleaver, 72 data blocks) or two (4.2 s, 168 blocks).
//   Burst: prekey (448 symbols), A1 and A2 (the 127-symbol A sequence twice), M1 (127 symbols: one of 8 cyclic shifts of an
//     m-sequence, which says the rate and the interleaver), M2 (the first 15 symbols of the same shift), 9 training blocks T (15 symbols),
//     then per data block 30 data symbols and 15 probe symbols (T). Known symbols are BPSK, bit 0 = +1.
//   M1 shift index: 0..3 single slot at 300 / 600 / 1200 / 1800 bit/s, 4..7 the same on two slots.
//     300: BPSK, rate 1/2 code with every coded bit sent twice (1/4); 600: BPSK; 1200: QPSK; 1800: 8PSK; all rate 1/2, K = 7.
//   Data symbols are multiplied by +-1 from a 15-bit m-sequence (x^15 + x^14 + 1 form, state 0x4D4B, one bit per data symbol,
//     restarted every 120 data symbols).
//   Block interleaver of 40 rows, column shift 17 (single slot) or 23 (double slot), read with a row step of 9.
//   PDU octets sent least significant bit first. FCS: CRC-16/X.25 (as the HDLC FCS).
//   SPDU (66 octets): byte 0 rls / version / iso8208 / change note, byte 1 ground station id + UTC sync, bytes 2-3 frame index (12 bits)
//     + offset, 52 minimum priority, 53-54 system table version, then frequency masks of the sender and two other stations, FCS 64-65.
//   MPDU: byte 0 bit 0 set, bit 1 downlink; downlink: LPDU count in bits 2-5, gs id, aircraft id, 3 octets, one size octet per LPDU,
//     FCS; uplink: aircraft count - 1 in bits 4-6, gs id, per aircraft (id, LPDU count << 4, sizes), FCS. Size octet = length - 1.
//   LPDU: type octet, body, FCS. 0x0D unnumbered data, 0x1D unnumbered acked data, 0x8F logon request, 0x9F logon confirm,
//     0x3F logoff, 0x2F logon denied, 0x4F / 0x5F logon resume (confirm).
//   HFNPDU: 0xFF, type: 0xD1 performance data, 0xD5 frequency data, 0xD0 system table, 0xD2 system table request, 0xFF ACARS.
//   Positions: 20-bit two's complement latitude and longitude, 180 / 2^19 degrees per step; UTC in 2 s steps.
// The frequency masks of a squitter are read against the built-in station table (bit i = the i-th frequency of the station), as the
// system table the stations broadcast (HFNPDU 0xD0) is not reassembled.
#pragma once
#include "hfdl_tel.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr double kHfdlBaud = 1800.0;
constexpr double kHfdlCarrierHz = 1440.0;         // above the dial frequency
constexpr int kHfdlPrekey = 448, kHfdlALen = 127, kHfdlM1Len = 127, kHfdlM2Len = 15, kHfdlTLen = 15, kHfdlTrain = 9;
constexpr int kHfdlDataLen = 30, kHfdlBlockLen = 45;
constexpr int kHfdlPreLen = 2 * kHfdlALen + kHfdlM1Len + kHfdlM2Len + kHfdlTrain * kHfdlTLen;   // 531: A1 .. the last T
constexpr double kHfdlFrameSec = 32.0, kHfdlSlotSec = 32.0 / 13.0;
constexpr double kHfdlRrcAlpha = 0.35;

struct HfdlRate { int m1; int bps; int bitsPerSym; bool quarter; bool twoSlots; int blocks; int colShift; };
const HfdlRate& hfdlRate(int m1);                  // m1 0..7
int hfdlBurstSyms(int m1);                         // from A1 to the last probe (the prekey not counted)
int hfdlCodedBits(int m1);                         // bits on the channel
int hfdlPayloadOctets(int m1);                     // octets one burst carries

int hfdlABit(int k);                               // 0..126
int hfdlMBit(int shift, int k);                    // shift 0..7, k 0..126
extern const int8_t kHfdlT[kHfdlTLen];             // +-1
// the known symbols from A1 to the last training block (531), +-1
const std::vector<float>& hfdlPreambleRef(int m1);
void hfdlScramblerSigns(int n, std::vector<int8_t>& out);   // n data symbols

// interleaver: chanPos[j] = where the j-th decoder-order bit is sent (a permutation of 0 .. coded bits - 1)
const std::vector<int>& hfdlInterleave(int m1);

// rate 1/2, K = 7, generators 0x4F and 0x6D (the first output first)
std::vector<uint8_t> hfdlConvEncode(const std::vector<uint8_t>& bits);
// soft: 2 values per input bit, positive = 0. Returns the decoded bits (n of them).
std::vector<uint8_t> hfdlViterbi(const std::vector<float>& soft, int n);

// PSK: v holds bitsPerSym bits, the first bit sent in bit 0 (the most significant bit of the Gray label). BPSK +-1; QPSK and 8PSK
// at k * 360 / M degrees, k the Gray-decoded label (QPSK on the axes).
std::complex<float> hfdlPsk(int bitsPerSym, int v);
void hfdlSoftBits(int bitsPerSym, std::complex<float> y, float scale, float* out);   // positive = 0
int hfdlDecide(int bitsPerSym, std::complex<float> y);                               // nearest v

// the complex symbols of one burst (prekey included): unit amplitude
std::vector<std::complex<float>> hfdlBurst(int m1, const std::vector<uint8_t>& pdu);
// the reverse of the bit layers: data-symbol soft bits (channel order) -> octets
std::vector<uint8_t> hfdlDecodeSoft(int m1, const std::vector<float>& chanSoft);

uint16_t hfdlFcs(const uint8_t* p, size_t n);      // the FCS of n octets (sent low octet first)
bool hfdlFcsOk(const uint8_t* p, size_t n);        // n octets followed by their FCS

// ---- ground stations (built-in table, frequencies in kHz as published in recent HFDL system tables)
struct HfdlStationInfo { int id; const char* name; double lat, lon; std::vector<int> khz; };
const std::vector<HfdlStationInfo>& hfdlStations();
const HfdlStationInfo* hfdlStation(int id);
std::vector<int> hfdlMaskKhz(int gsId, uint32_t mask);
int hfdlStationOnKhz(int khz);                     // 0 none

// ---- builders (test signal, tests)
struct HfdlSquitterGs { int id = 0; bool utc = true; uint32_t mask = 0; };
struct HfdlSquitterSpec { int gsId = 1; bool utc = true; int frameIndex = 0; int changeNote = 0; int version = 0; uint32_t mask = 0; int systable = 50; HfdlSquitterGs other[2]; };
std::vector<uint8_t> hfdlSpdu(const HfdlSquitterSpec& s);
std::vector<uint8_t> hfdlLpdu(uint8_t type, const std::vector<uint8_t>& body);       // FCS appended
std::vector<uint8_t> hfdlLogonRequest(uint32_t icao);
std::vector<uint8_t> hfdlLogonConfirm(uint32_t icao, int acId);
std::vector<uint8_t> hfdlUnnumbered(const std::vector<uint8_t>& hfnpdu);
std::vector<uint8_t> hfdlPerfData(const std::string& flight, double lat, double lon, int utcSec, int gsId, int freqId);
std::vector<uint8_t> hfdlFreqData(const std::string& flight, double lat, double lon, int utcSec, const std::vector<HfdlSquitterGs>& heard);
std::vector<uint8_t> hfdlAcarsHfnpdu(const std::string& reg, const std::string& label, char blockId, const std::string& text, bool downlink);
std::vector<uint8_t> hfdlMpduDown(int gsId, int acId, const std::vector<std::vector<uint8_t>>& lpdus);
std::vector<uint8_t> hfdlMpduUp(int gsId, int acId, const std::vector<std::vector<uint8_t>>& lpdus);

// ---- parser: the PDU of one burst (padding after it allowed). Every SPDU / LPDU becomes a message (receiver fields left out).
// Returns false when no check sequence comes out right.
bool hfdlParsePdu(const uint8_t* p, size_t n, std::vector<HfdlMessage>& out);

} // namespace dect2
