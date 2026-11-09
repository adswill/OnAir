// DVB SI text fields (ETSI EN 300 468 Annex A) to clean UTF-8: the character table prefix selects ISO/IEC 6937 (default), ISO 8859-x, UCS-2, UTF-8, GB 2312,
// Big5 or KS X 1001; the converted text loses its control characters. The character set tables that the system provides (iconv, or the Windows code pages) are
// reached through convertCharset(); everything else lives in text_charset (Rust).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2::dvbsi {

// Source bytes in the named character set ("ISO-8859-5", "UCS-2BE", "BIG5", "GB2312", "EUC-KR"...) to UTF-8 with iconv (code pages on Windows); the raw bytes
// when the character set is unknown to the system. Never empty-handed: partial output is returned for input that stops converting.
std::string convertCharset(const char* charset, const uint8_t* p, int n);
// gbDefault: unprefixed GB 2312 pairs are decoded as GB 2312 (DTMB).
std::string dvbText(const uint8_t* p, int n, bool gbDefault = false);
// DTMB: every dvbText() call (service names, EPG, network name) also tries unprefixed GBK pairs, not only the callers that pass gbDefault.
void setGbDefaultGlobal(bool on);
// ISDB (ARIB STD-B24): unprefixed text is the 8-unit code. Automatic: text that looks like plain ASCII stays with the DVB rules (the Latin variant used in South America).
void setAribDefaultGlobal(bool on);
std::string aribText(const uint8_t* p, int n);
bool looksArib(const uint8_t* p, int n);

} // namespace dect2::dvbsi
