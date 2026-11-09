// FIS-B text and graphic overlay products (TWGO, products 8 to 16): DLAC text records and overlay geometry. Field layout read from Rand's fisb-decode (a DO-358B implementation) and the FIS-B Product
// Definition draft; tested on simulated records only. Segmented APDUs are not reassembled.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct UatOverlayPoint { double lon = 0, lat = 0; int altFt = 0; };
struct UatOverlay { int reportNumber = 0, reportYear = 0, recordId = 0, geometry = 0, objectType = 0, objectStatus = 0; std::string label; std::vector<UatOverlayPoint> pts; double radiusNm = 0; };

// Decode one TWGO APDU payload. Returns false when the payload format is not recognised (the text is then left unchanged).
bool uatTwgoDecode(uint16_t productId, const uint8_t* d, size_t n, std::string& text, std::vector<UatOverlay>& overlays);
std::string uatDlacToText(const uint8_t* d, size_t n);

} // namespace dect2
