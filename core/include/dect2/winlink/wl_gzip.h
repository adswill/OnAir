// gzip (RFC 1952) compression for Winlink's gzip proposals ("FD", the GZIP_EXPERIMENT of Pat / wl2k-go): one deflate block (RFC 1951)
// with the fixed Huffman codes and LZ77 matches found through hash chains (32 KB window). Decompression is the shared gunzip()
// (dect2/common/inflate.h). OnAir's own code.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace winlink {

std::vector<uint8_t> gzipCompress(const uint8_t* d, size_t n);
inline std::vector<uint8_t> gzipCompress(const std::vector<uint8_t>& v) { return gzipCompress(v.data(), v.size()); }
// The raw deflate stream alone (what gzipCompress wraps)
std::vector<uint8_t> deflateFixed(const uint8_t* d, size_t n);

} // namespace winlink
} // namespace dect2
