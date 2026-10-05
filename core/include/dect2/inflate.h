// DEFLATE (RFC 1951) and gzip (RFC 1952) decompression, without any library: the low level signaling tables of ATSC 3.0 are gzip compressed.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

// Decompresses a raw deflate stream. Returns false on corrupt data or when the output would exceed maxOut bytes.
bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut = 64u << 20, size_t* consumed = nullptr);
// Decompresses a gzip file (one member); checks the CRC-32 and the length.
bool gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut = 64u << 20);
uint32_t crc32Ieee(const uint8_t* data, size_t size, uint32_t crc = 0);

} // namespace dect2
