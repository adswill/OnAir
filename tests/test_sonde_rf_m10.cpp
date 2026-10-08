// M10 through the whole receiver (see sonde_rftest.h)
#include "dect2/sonde_rftest.h"
int main() {
    const int r = dect2::sondetest::runTypeRf(1);
    if (!r) printf("ok\n");
    return r;
}
