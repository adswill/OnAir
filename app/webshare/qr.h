// A small QR code encoder for the share link (byte mode, error correction M, versions 1 to 10: links up to 213 bytes). No dependencies; tested in
// tests/webshare/test_webshare_core.cpp (format bits, Reed-Solomon) and checked against a real decoder when the OnAir release is made.
#pragma once
#include <string>
#include <vector>

namespace webshare {

struct QrCode {
    int size = 0;                  // modules per side (21 for version 1)
    std::vector<unsigned char> m;  // size*size, 1 = dark
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && m[(size_t)y * (size_t)size + (size_t)x]; }
};
// False when the text is longer than 213 bytes.
bool qrEncode(const std::string& text, QrCode& out);
// The 15 format bits (error correction level M, mask 0..7) as the standard defines them; exposed for the test.
unsigned qrFormatBitsM(int mask);
// Reed-Solomon check bytes of `data` over GF(256) (polynomial 0x11D) for `degree` check bytes.
std::vector<unsigned char> qrReedSolomon(const std::vector<unsigned char>& data, int degree);

} // namespace webshare
