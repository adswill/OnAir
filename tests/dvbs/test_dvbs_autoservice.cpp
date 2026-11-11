// #322: the single service of a DVB-S/S2 signal starts by itself and follows a retune; several services stay manual
#include "../../app/dvbs/dvbs_autoservice.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static TsService svc(int id, bool pmt = true) { TsService s; s.id = id; s.havePmt = pmt; if (pmt) s.streams.push_back(TsStream()); return s; }

int main() {
    AutoServiceState st;
    double t = 0;
    // no lock, nothing happens
    CHECK(autoServicePick(st, {svc(1)}, -1, false, t) == -1);
    // lock with the PMT not yet seen: wait
    CHECK(autoServicePick(st, {svc(1, false)}, -1, true, t += 0.25) == -1);
    // the PMT arrives: start the only service
    CHECK(autoServicePick(st, {svc(1)}, -1, true, t += 0.25) == 1);
    CHECK(autoServicePick(st, {svc(1)}, 1, true, t += 0.25) == -1);
    // the user stops it: it stays stopped
    CHECK(autoServicePick(st, {svc(1)}, -1, true, t += 0.25) == -1);
    CHECK(autoServicePick(st, {svc(1)}, -1, true, t += 0.25) == -1);
    // retune: the lock goes away for a while, the next signal has a service with another id: start it
    for (int i = 0; i < 10; i++) CHECK(autoServicePick(st, {}, -1, false, t += 0.25) == -1);
    CHECK(autoServicePick(st, {svc(5)}, -1, true, t += 0.25) == 5);
    // retune where the old service id is still selected but the new signal has another one: switch
    for (int i = 0; i < 10; i++) autoServicePick(st, {svc(5)}, 5, false, t += 0.25);
    CHECK(autoServicePick(st, {svc(7)}, 5, true, t += 0.25) == 7);
    // the same service id again after a retune, nothing playing: start
    autoServicePick(st, {svc(7)}, -1, true, t += 0.25);   // stopped by hand
    for (int i = 0; i < 10; i++) autoServicePick(st, {}, -1, false, t += 0.25);
    CHECK(autoServicePick(st, {svc(7)}, -1, true, t += 0.25) == 7);
    // two services: manual, a playing service is left alone
    AutoServiceState m;
    CHECK(autoServicePick(m, {svc(1), svc(2)}, -1, true, 0) == -1);
    CHECK(autoServicePick(m, {svc(1), svc(2)}, 2, true, 1) == -1);
    // a stopped service stays stopped through a short lock dropout
    AutoServiceState d;
    CHECK(autoServicePick(d, {svc(3)}, -1, true, 0) == 3);
    autoServicePick(d, {svc(3)}, -1, true, 1);   // user stop
    autoServicePick(d, {svc(3)}, -1, false, 2); autoServicePick(d, {svc(3)}, -1, false, 2.5);
    CHECK(autoServicePick(d, {svc(3)}, -1, true, 3) == -1);
    // #410: a retune to another carrier whose only service has the same id while it is still selected: restart the player session
    {
        AutoServiceState r; bool rs = false; double u = 0;
        CHECK(autoServicePick(r, {svc(1)}, -1, true, u, &rs) == 1 && !rs);
        CHECK(autoServicePick(r, {svc(1)}, 1, true, u += 0.25, &rs) == -1 && !rs);
        // the retune clears the service list while the lock stays (fast relock): the same id comes back on the new carrier
        CHECK(autoServicePick(r, {}, 1, true, u += 0.25, &rs) == -1 && !rs);
        CHECK(autoServicePick(r, {svc(1)}, 1, true, u += 0.25, &rs) == 1 && rs);
        CHECK(autoServicePick(r, {svc(1)}, 1, true, u += 0.25, &rs) == -1 && !rs);   // once only
        // the lock is gone for longer than the relock time, the same id comes back: restart again
        for (int i = 0; i < 10; i++) autoServicePick(r, {svc(1)}, 1, false, u += 0.25, &rs);
        CHECK(autoServicePick(r, {svc(1)}, 1, true, u += 0.25, &rs) == 1 && rs);
        // a short dropout on the same carrier (services kept) is not a new signal
        autoServicePick(r, {svc(1)}, 1, false, u += 0.25, &rs); autoServicePick(r, {svc(1)}, 1, false, u += 0.25, &rs);
        CHECK(autoServicePick(r, {svc(1)}, 1, true, u += 0.25, &rs) == -1 && !rs);
        // stopped by hand, then a retune with the same id: an ordinary start, no restart needed
        autoServicePick(r, {svc(1)}, -1, true, u += 0.25, &rs);
        for (int i = 0; i < 10; i++) autoServicePick(r, {}, -1, false, u += 0.25, &rs);
        CHECK(autoServicePick(r, {svc(1)}, -1, true, u += 0.25, &rs) == 1 && !rs);
        // a caller without the restart flag keeps the old behaviour
        AutoServiceState o;
        autoServicePick(o, {svc(2)}, -1, true, 0); autoServicePick(o, {}, 2, true, 1);
        CHECK(autoServicePick(o, {svc(2)}, 2, true, 2) == -1);
    }
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
