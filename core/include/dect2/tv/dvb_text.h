// DVB SI text fields (ETSI EN 300 468 Annex A) to clean UTF-8: the character table prefix selects ISO/IEC 6937 (default), ISO 8859-x, UCS-2, UTF-8, GB 2312,
// Big5 or KS X 1001; the converted text loses its control characters. The character set tables that the system provides (iconv, or the Windows code pages) are
// reached through convertCharset(); everything else lives in text_charset (Rust).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dect2::dvbsi {

// Source bytes in the named character set ("ISO-8859-5", "UCS-2BE", "BIG5", "GB2312", "EUC-KR"...) to UTF-8 with iconv (code pages on Windows); the raw bytes
// when the character set is unknown to the system. Never empty-handed: partial output is returned for input that stops converting.
std::string convertCharset(const char* charset, const uint8_t* p, int n);
// DVB selectors take precedence over an unprefixed-text fallback. ARIB is a separate wire format; Isdb selects its profile from stream context.
enum class TextEncoding : uint32_t { Dvb = 0, Gb2312 = 1, EucKr = 2, Big5 = 3, Utf8 = 4, Latin15 = 5, Arib = 6, Isdb = 7 };
struct TextContext {
    TextEncoding encoding = TextEncoding::Dvb;
    std::string country;                    // one unambiguous broadcast country, not the interface locale
    TextContext withLanguage(std::string_view language) const;
};
std::string dvbText(const uint8_t* p, int n, const TextContext& context = {});
std::string aribText(const uint8_t* p, int n);
bool looksArib(const uint8_t* p, int n);

} // namespace dect2::dvbsi
