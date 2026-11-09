// Auto gain state per receiver (#450): two receivers never see each other's object.
#include "../../app/state/per_receiver.h"
#include <cstdio>
#include "../common/test_util.h"

struct Thing { int v = 0; };

int main() {
    perReceiver<Thing, 4>(1).v = 11;
    perReceiver<Thing, 4>(2).v = 22;
    CHECK(perReceiver<Thing, 4>(1).v == 11, "RX 1 lost its state");
    CHECK(perReceiver<Thing, 4>(2).v == 22, "RX 2 lost its state");
    CHECK(&perReceiver<Thing, 4>(1) != &perReceiver<Thing, 4>(2), "RX 1 and RX 2 share one object");
    CHECK(&perReceiver<Thing, 4>(4) != &perReceiver<Thing, 4>(1), "RX 4 shares RX 1's object");
    CHECK(&perReceiver<Thing, 4>(0) == &perReceiver<Thing, 4>(5), "out of range ids share the spare slot");
    CHECK(&perReceiver<Thing, 4>(0) != &perReceiver<Thing, 4>(1), "the spare slot is a real receiver's");
    printf("ok\n");
    return fails ? 1 : 0;
}
