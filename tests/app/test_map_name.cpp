// The optional public name on the map: validation (app/map/map_name.h), same rules as the server's by-name.js.
#include "map_name.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::string n;
    CHECK(mapname::check("  Kris_9  ", n) == 0 && n == "Kris_9");
    CHECK(mapname::check("Anna  Maria-K.", n) == 0 && n == "Anna Maria-K.");
    CHECK(mapname::check("", n) == 0 && n.empty());
    CHECK(mapname::check("   ", n) == 0 && n.empty());
    CHECK(mapname::check("a", n) == 1);
    CHECK(mapname::check(std::string(24, 'x'), n) == 0);
    CHECK(mapname::check(std::string(25, 'x'), n) == 1);
    CHECK(mapname::check("a<b>c", n) == 2);
    CHECK(mapname::check("caf\xc3\xa9 x", n) == 2);
    CHECK(mapname::check("a\tb", n) == 2);
    CHECK(mapname::check("name@mail", n) == 2);
    CHECK(mapname::check("www.site", n) == 3);
    CHECK(mapname::check("me.com", n) == 3);
    CHECK(mapname::check("v.2 radio", n) == 0);
    CHECK(mapname::check("Admin", n) == 4);
    CHECK(mapname::check("0n-Air", n) == 4);
    CHECK(mapname::check("adm1n_01", n) == 4);
    CHECK(mapname::check("Adminton", n) == 0);
    CHECK(mapname::check("nazi_1", n) == 4);
    CHECK(n.empty());
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
