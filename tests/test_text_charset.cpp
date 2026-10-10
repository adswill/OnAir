// Broadcast character sets to UTF-8, and the clean-up of names before they go into a channel-list submission (a submission with control or
// invisible characters is refused): examples from real scans (Polish and Austrian DAB labels shown as "Tr?jka", "xsterrei").
#include "dect2/text_charset.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace dect2;
static int fails = 0;
static void check(const std::string& got, const char* want, const char* what) {
    if (got != want) { printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want); fails++; }
}
static std::string b(const char* s) { return std::string(s); }

int main() {
    auto ebu = [](const char* s) { return text::clean(text::dabCharset(0, (const uint8_t*)s, strlen(s))); };
    check(ebu("PR Tr\x86jka      "), "PR Trójka", "EBU o acute");
    check(ebu("Radio Kierowc\x86w"), "Radio Kierowców", "EBU o acute 2");
    check(ebu("Antenne \xd7sterrei"), "Antenne Österrei", "EBU O diaeresis");
    check(ebu("j\x97.live"), "jö.live", "EBU o diaeresis");
    check(ebu("Radio Z\x9brich"), "Radio Zçrich", "EBU c cedilla (code 0x9B)");
    check(ebu("\xa9 Euro"), "€ Euro", "EBU euro");
    const uint8_t ucs[] = {0x00, 'O', 0x00, 0xF6, 0x01, 0x7E};
    check(text::clean(text::dabCharset(6, ucs, sizeof ucs)), "Oöž", "DAB UCS-2");
    check(text::clean(text::dabCharset(15, (const uint8_t*)"Caf\xc3\xa9", 5)), "Café", "DAB UTF-8");
    auto i6937 = [](const char* s) { return text::clean(text::iso6937((const uint8_t*)s, strlen(s))); };
    check(i6937("Caf\xc2" "e"), "Café", "ISO 6937 acute");
    check(i6937("M\xc8" "agyar \xcf" "Zivot"), "Mägyar Život", "ISO 6937 diaeresis, caron");
    check(i6937("\x86" "News\x87\x8aToday"), "News Today", "DVB emphasis and line break codes");
    check(text::clean(b("A\xc2\x86" "B")), "AB", "C1 control dropped");
    check(text::clean(b("A\xe2\x80\x8b" "B\xef\xbb\xbf")), "AB", "zero width space and BOM dropped");
    check(text::clean(b("bad\xff\xfe" "x\xc3")), "badx", "invalid UTF-8 dropped");
    check(text::clean(b("Tr\xef\xbf\xbdjka")), "Trjka", "replacement character dropped");
    check(text::clean(b("  a \t b\n")), "a b", "spaces folded and trimmed");
    check(text::clean(b("\xc0\xaf")), "", "over-long encoding dropped");
    check(text::clean(b("Zürich 日本 🙂")), "Zürich 日本 🙂", "real characters kept");
    printf(fails ? "text charset: %d FAILED\n" : "text charset: all passed\n", fails);
    return fails ? 1 : 0;
}
