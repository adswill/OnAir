// Inmarsat-C TDM frame coding: unique word, scrambler, rate 1/2 K=7 convolutional code, interleaver, packet check bytes.
// The frame is 640 bytes (639 of data and a flush byte), 8.64 s at 1200 symbols/s: 10240 coded symbols plus 128 unique word symbols.
//
// Sources (facts only, the code is written here): the layout, the unique word, the row permutation, the scrambler and the check
// bytes follow the open decoder "inmarsatc" (Scytale-C library: inmarsatc_decoder.cpp, inmarsatc_parser.cpp), which in turn cites
// Calcutt and Tetley, Satellite communications, and the Nera Inmarsat-C service manual.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace inmc {

constexpr int kFrameBytes = 640;        // data bytes of a frame, the last one is the flush byte
constexpr int kRows = 64, kCols = 162;  // interleaver block as sent: 64 rows of 2 unique word symbols and 160 data symbols
constexpr int kDataCols = 160;
constexpr int kFrameSyms = kRows * kCols;           // 10368
constexpr int kCodedSyms = kRows * kDataCols;       // 10240
constexpr double kSymbolRate = 1200.0;
constexpr double kFrameSeconds = 8.64;

// Unique word: one bit per row, sent twice at the start of each row, rows in transmission order.
extern const uint8_t kUw[kRows];

// Row j of the transmitted block carries row (39 * j) mod 64 of the interleaver matrix; the coded stream is written down the columns.
inline int matrixRowOfTxRow(int j) { return (j * 39) & 63; }

// Scrambler: the 640 bytes are 160 groups of 4; a group is complemented when the generator (x^7 + x^5 + x^4 + x^3, start state 0x80) gives 1.
uint8_t scramblerFlag(int group);
void scrambleBytes(uint8_t bytes[kFrameBytes]);     // complements the flagged groups; the same call descrambles

// Convolutional code: constraint length 7, polynomials 0x6d and 0x4f with the newest bit in the lowest position
// (the K = 7 NASA code, 0133 and 0171 octal with the bit order reversed). Two symbols per bit, the 0x6d one first.
uint8_t convOutput(unsigned reg7);                  // bit 1 = first symbol, bit 0 = second symbol

// Frame bytes (info[639] is ignored, the flush byte is sent as zero) to the 10368 symbols (0 or 1) in transmission order.
void encodeFrame(const uint8_t info[kFrameBytes], uint8_t sym[kFrameSyms]);

struct FrameDecode {
    uint8_t bytes[kFrameBytes];
    int uwErrors = 0;            // unique word symbols that disagree with their hard decision (polarity as given)
    int symbolErrors = 0;        // coded symbols that differ from the re-encoded result
    double pathMetric = 0;
};
// soft: 10368 values in transmission order, positive for symbol 1. The polarity must already be right (see uwCorrelate).
void decodeFrame(const float soft[kFrameSyms], FrameDecode& out);

// Unique word errors of the frame whose first symbol is hard[0]: 128 compares. Returns the number of errors for the normal polarity.
int uwErrors(const uint8_t* hard);

// Unique word fit that allows one polarity flip inside the frame (a cycle slip of the carrier loop): d[r] is the number of the two unique word symbols
// of row r that disagree with the normal polarity. Returns the fewest wrong symbols over: normal or reversed all through, or a flip before row slipRow.
// slipRow is kRows when there is no flip. A flip must save at least 8 symbols over the plain fit, so that noise does not invent one.
int uwFit(const uint8_t d[kRows], bool& startRev, int& slipRow);

// Check bytes of a packet of len bytes (the last two are the check bytes): the two sums of the reference decoder.
void packetCheckSet(uint8_t* pkt, int len);
bool packetCheckOk(const uint8_t* pkt, int len);

} // namespace inmc
} // namespace dect2
