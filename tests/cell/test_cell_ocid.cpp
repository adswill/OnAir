// OpenCelliD lookups without the network: the request URL, the answers (found, not found, bad key, limit, error), the cache (write, read,
// the day "not found" is kept), the rate limiter, and the background lookup through a fake fetch.
#include "../common/tmp_path.h"
#include "dect2/cell/cell_ocid.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    OcidCellKey k; k.mcc = 246; k.mnc = 1; k.lac = 1234; k.cell = 25600 * 256 + 3;
    const std::string u = ocidUrl("pk.ab+c/d", k);
    CHECK(u == "https://opencellid.org/cell/get?key=pk.ab%2Bc%2Fd&mcc=246&mnc=1&lac=1234&cellid=6553603&radio=LTE&format=json", "url %s", u.c_str());

    OcidResult r = ocidParse("{\"lon\": 25.2797, \"lat\": 54.6872, \"mcc\": 246, \"mnc\": 1, \"lac\": 1234, \"cellid\": 6553603, \"averageSignalStrength\": 0, "
                             "\"range\": 1500, \"samples\": 42, \"changeable\": 1, \"radio\": \"LTE\"}");
    CHECK(r.status == OcidStatus::Found && std::fabs(r.lat - 54.6872) < 1e-6 && std::fabs(r.lon - 25.2797) < 1e-6 && r.range == 1500 && r.samples == 42, "found");
    CHECK(ocidParse("{\"error\":\"Cell not found\",\"code\":1}").status == OcidStatus::NotFound, "not found");
    CHECK(ocidParse("{\"error\":\"API key is invalid\",\"code\":2}").status == OcidStatus::BadKey, "bad key");
    CHECK(ocidParse("{\"error\":\"Daily limit exceeded\",\"code\":7}").status == OcidStatus::Limit, "limit");
    CHECK(ocidParse("{\"error\":\"Server is busy\",\"code\":5}").status == OcidStatus::Error, "error");
    CHECK(ocidParse("<html>").status == OcidStatus::Error, "garbage");

    OcidCache c;
    OcidResult nf; nf.status = OcidStatus::NotFound;
    OcidCellKey k2 = k; k2.cell++;
    OcidCellKey k3 = k; k3.cell += 2;
    c.put(k, r, 1000); c.put(k2, nf, 1000);
    OcidResult bad; bad.status = OcidStatus::BadKey; c.put(k3, bad, 1000);
    CHECK(c.size() == 2, "errors not stored");
    const TmpPath path = tmpPath("ocid_test_cache.bin");
    CHECK(c.save(path), "save");
    OcidCache d; CHECK(d.load(path) && d.size() == 2, "load");
    const OcidEntry* e = d.get(k, 1000 + 400 * 86400);
    CHECK(e && e->r.status == OcidStatus::Found && std::fabs(e->r.lat - 54.6872) < 1e-5 && e->r.samples == 42, "found kept");
    CHECK(d.get(k2, 1000 + 3600) && d.get(k2, 1000 + 3600)->r.status == OcidStatus::NotFound, "not found within a day");
    CHECK(!d.get(k2, 1000 + 86400), "not found expires");
    unlink(path);

    OcidRateLimiter rl(1.0);
    CHECK(rl.tryTake(10.0) && !rl.tryTake(10.5) && std::fabs(rl.waitFor(10.5) - 0.5) < 1e-9 && rl.tryTake(11.0), "rate limiter");

    // the background lookup: requests are spaced at least a second apart; a bad key stops the queue
    {
        std::vector<double> at; const auto t0 = std::chrono::steady_clock::now();
        OcidLookup L([&](const std::string& url, std::string& body) {
            at.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            body = url.find("cellid=6553603") != std::string::npos ? "{\"lat\":1.5,\"lon\":2.5,\"range\":100,\"samples\":3}" : "{\"error\":\"Cell not found\",\"code\":1}";
            return true;
        }, "");
        L.request(k);   // no key yet: ignored
        CHECK(L.pending() == 0, "no key");
        L.setKey("KEY"); L.request(k); L.request(k2); L.request(k);
        for (int i = 0; i < 40 && (L.pending() || at.size() < 2); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        OcidEntry x;
        CHECK(L.lookup(k, x) && x.r.status == OcidStatus::Found && x.r.lat == 1.5, "looked up");
        CHECK(L.lookup(k2, x) && x.r.status == OcidStatus::Found && x.r.lat == 1.5, "a sector not in the database takes its site's position");
        CHECK(at.size() == 2 && at[1] - at[0] >= 0.95, "spaced %zu", at.size());
        L.request(k); CHECK(L.pending() == 0, "cached, not asked again");
    }
    {
        // a cell missing from the database: the other sectors of its site are asked for, and the first one found places the site
        std::vector<std::string> asked;
        OcidLookup L([&](const std::string& url, std::string& body) {
            asked.push_back(url);
            body = url.find("cellid=6553857") != std::string::npos ? "{\"lat\":3.5,\"lon\":4.5,\"range\":200,\"samples\":9}" : "{\"error\":\"Cell not found\",\"code\":1}";
            return true;
        }, "");
        OcidCellKey m = k; m.cell = 25601 * 256 + 12;   // sector 12 of site 25601; only its sector 1 (6553857) is known
        L.setKey("KEY"); L.request(m);
        for (int i = 0; i < 60 && (L.pending() || asked.size() < 2); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        OcidEntry x;
        CHECK(L.lookup(m, x) && x.r.status == OcidStatus::Found && x.r.lat == 3.5, "placed from sector 1");
        CHECK(asked.size() == 2 && L.pending() == 0, "stops once the site is placed (%zu asked)", asked.size());
    }
    {
        int n = 0;
        OcidLookup L([&](const std::string&, std::string& body) { n++; body = "{\"error\":\"API key is invalid\",\"code\":2}"; return true; }, "");
        L.setKey("BAD"); L.request(k); L.request(k2);
        for (int i = 0; i < 20 && L.status().empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        CHECK(n == 1 && L.status().find("refused") != std::string::npos, "bad key halts (%d)", n);
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
