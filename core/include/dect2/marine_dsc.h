// Digital Selective Calling (ITU-R M.493, operation M.541): 10-bit symbols, DX/RX time diversity, phasing, call parsing and builders.
// The decoder works on bits (100 bd FSK on MF/HF, 1200 bd AFSK on VHF channel 70), so it is tested without the radio layer.
#pragma once
#include "marine_tel.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

// ---- symbols (M.493 Table 1): bits 1..7 = the symbol number, least significant bit first (Y = 1, B = 0); bits 8..10 = the number of
// B elements in the seven, most significant bit first. Returned with bit 1 in bit 9 of the value (bit 1 is sent first).
unsigned dscSymbolBits(int sym);
bool dscSymbolDecode(unsigned bits10, int& sym);           // false when the check bits do not match

// ---- text for the codes (M.493 Tables 3, 9, 10, 11, 12)
const char* dscFormatName(int sym);
const char* dscCategoryName(int sym);
const char* dscNatureName(int sym);
const char* dscTelecmd1Name(int sym);
const char* dscTelecmd2Name(int sym);

// ---- number fields (Table 2): two decimal digits per symbol
std::string dscMmsiFromSymbols(const int* s5);             // 9 digits, "" when a symbol is not 00..99
bool dscMmsiToSymbols(const std::string& mmsi9, int out[5]);
bool dscPositionFromSymbols(const int* s5, double& lat, double& lon, std::string* text = nullptr);   // false: all 9s or unreadable
void dscPositionToSymbols(double lat, double lon, int out[5]);
bool dscTimeFromSymbols(int a, int b, int& hour, int& minute);       // false for 8888 or out of range
std::string dscFrequencyText(const int* s3);               // three symbols in the order sent (HM TM first): "8291.0 kHz", "MF/HF channel 1234", "VHF ch 16", "" for 126 126 126
void dscFrequencyToSymbols(long hundredHz, int out[3]);

// ---- calls
// Body = the information symbols from the format specifier to the last message symbol, without the repeated format specifier, EOS and ECC.
std::vector<int> dscBuildDistress(const std::string& mmsi, int nature, double lat, double lon, int hour, int minute, int subsequent);
std::vector<int> dscBuildAllShips(const std::string& mmsi, int category, int tc1, int tc2, long rxHundredHz, long txHundredHz);   // frequency < 0: 126 x 3
std::vector<int> dscBuildIndividual(const std::string& to, int category, const std::string& from, int tc1, int tc2, long rxHundredHz, long txHundredHz);
std::vector<int> dscBuildDistressAck(const std::string& from, const std::string& distressMmsi, int nature, double lat, double lon, int hour, int minute, int subsequent);
int dscEcc(const std::vector<int>& body, int eos);          // even parity over the information symbols
// The bits of a whole call: dot pattern (200 or 20 bits), phasing, format specifier twice, the body, EOS ECC EOS EOS, each symbol again
// five places later (RX). One entry per bit, 1 = Y.
std::vector<uint8_t> dscFrameBits(const std::vector<int>& body, int eos, int dotBits);
// The same list of symbols in the order of the slots (DX at even places), for tests
std::vector<int> dscFrameSymbols(const std::vector<int>& body, int eos);

// Parse the merged information symbols of a call (-1 = unreadable). body as above, eos = 117/122/127.
DscCall dscParseCall(const std::vector<int>& body, int eos, bool eccOk, int erasures);

class DscDecoder {
public:
    using CallCb = std::function<void(const DscCall&)>;
    explicit DscDecoder(bool invert = false, bool vhf = false);
    void setCallback(CallCb cb) { cb_ = std::move(cb); }
    void reset();
    // One received bit; soft > 0 = Y (1) before the inversion, |soft| 0..1 = how sure
    void pushBit(float soft);
    bool locked() const { return locked_; }
    uint64_t callsOk() const { return ok_; }
    uint64_t callsBad() const { return bad_; }
private:
    struct Sym { int s = -1; float conf = 0; };
    void onSymbol(int s, float conf);
    void tryFinish();
    void abortLock();
    bool invert_, vhf_;
    CallCb cb_;
    unsigned reg_ = 0; float regConf_[10] = {};
    uint64_t bitCount_ = 0;
    std::vector<Sym> hist_[10];          // per bit phase: the last symbols
    bool locked_ = false;
    int lockPhase_ = 0;
    std::vector<Sym> slots_;             // from the first phasing slot (index 0)
    uint64_t ok_ = 0, bad_ = 0;
};

} // namespace marine
} // namespace dect2
