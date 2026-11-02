// MPT 1327 control channel codewords, written from the public signalling standard (MPT 1327, 1997 issue, sections 3 and 5):
// 64-bit codewords (48 information bits + 15 check bits of a (63,48) cyclic code + 1 parity bit), the codeword
// synchronisation sequences and the address codeword layouts used on a control channel. Receive and decode only.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::mpt1327 {

constexpr uint16_t kSync = 0xC4D7;   // control channel codeword synchronisation sequence SYNC, bit 1 first (section 3.2.1.1)
constexpr uint16_t kSynt = 0x3B28;   // traffic channel sequence SYNT (section 3.2.1.2); not used by the control channel receiver

// A codeword is held in a uint64_t with codeword bit 1 (the first one transmitted) as bit 63 and bit 64 as bit 0.
uint64_t encode(uint64_t info48);        // info48: codeword bits 1 .. 48 (bit 1 in bit 47); returns the 64-bit codeword with its check bits
bool valid(uint64_t cw);                 // the check bits and the parity agree with the information bits
bool correct(uint64_t& cw);              // a word with exactly one wrong bit (check bits or information) is repaired in place; true when it was
uint16_t checkBits(uint64_t info48);     // codeword bits 49 .. 64

struct Address {
    int pfix = -1;     // 7-bit prefix, -1 when the message carries no prefix
    int ident = -1;    // 13-bit ident, -1 when absent
};
const char* specialIdent(int ident);        // ALLI, TSCI, IPFIXI, SDMI, DIVERTI, INCI, REGI, DNI, PABXI, PSTNGI, DUMMYI; nullptr for an ordinary ident
std::string addressText(const Address& a);   // "prefix/ident", "ident N" without a prefix, "-" when absent

struct Message {
    std::string type;       // mnemonic: GTC, ALH, ACK, AHY, BCAST, ... (empty: not an address codeword)
    std::string detail;     // the function / parameters in words
    Address called;         // PFIX/IDENT1
    Address caller;         // IDENT2 (the standard gives it without a prefix of its own), pfix -1
    int channel = -1;       // channel number (GTC: the allocated channel; BCAST: the control channel announced), -1 when the message has none
    int sys = -1;           // system identity (BCAST), -1 when the message has none
    uint64_t cw = 0;
};

uint32_t field(uint64_t cw, int firstBit, int count);   // codeword bits firstBit .. firstBit+count-1 (1-based) as a number, first bit most significant
bool decodeAddress(uint64_t cw, Message& m);             // false when bit 1 is 0 (a data codeword); the check bits are not tested here
bool decodeCcsc(uint64_t cw, int& sys);                  // the control channel system codeword: bit 1 = 0, SYS in bits 2 .. 16, final 16 bits = SYNC

}  // namespace dect2::mpt1327
