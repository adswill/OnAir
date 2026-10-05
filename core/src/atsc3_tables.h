// Constant tables taken from A/322 (generated from the standard's text; see atsc3_tables.cpp).
#pragma once
namespace dect2 {
namespace atsc3 {
extern const int kCp32[192];   // Table D.1.1: common continual pilots, absolute carrier numbers, 32K FFT
extern const int kCp16[96];    // Table D.1.2: 16K FFT
extern const int kCp8[48];     // Table D.1.3: 8K FFT
} // namespace atsc3
} // namespace dect2
