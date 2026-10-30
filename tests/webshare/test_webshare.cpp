#include "../../app/webshare/webshare.h"
#include <cstdio>
using namespace webshare;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main() {
    const std::string tl = "INF Requesting new quick Tunnel on trycloudflare.com...\nINF |  https://api.trycloudflare.com  |\nINF |  https://quiet-river-lamp-7.trycloudflare.com  |\n";
    CHECK(parseTunnelUrl(tl) == "https://quiet-river-lamp-7.trycloudflare.com");
    CHECK(parseTunnelUrl("nothing here").empty());
    CHECK(parseTunnelUrl("https://evil.example.com/x.trycloudflare.com.evil.org").empty());
    const std::string sl = "onair-server: Directory listing preview\n  name=Test\n  url=https://a.trycloudflare.com\nweb_directory is on, but nothing is sent yet: to confirm exactly this, add  web_directory_ack=AB12-cd34  to the settings file\n";
    CHECK(parseAckCode(sl) == "AB12-cd34");
    CHECK(parseAckCode("no code").empty());
    const std::string pv = parsePreview(sl);
    CHECK(pv.find("name=Test") != std::string::npos && pv.find("web_directory_ack") == std::string::npos);
    CHECK(cleanLine("Roof HackRF", 48));
    CHECK(!cleanLine("two\nlines", 48));
    CHECK(!cleanLine(std::string(49, 'a'), 48));
    printf(bad ? "FAILED\n" : "ok\n");
    return bad ? 1 : 0;
}
