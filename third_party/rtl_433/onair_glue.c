/* OnAir glue for the vendored rtl_433 sources (GPL-2.0-or-later, see COPYING and THIRD_PARTY.md).
 * - the device table (rtl_433's DEVICES list)
 * - a way to leave flex_decoder / optparse error paths without exit() (a bad user spec must not end the app) */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_433_devices.h"
#include "onair_rtl433.h"

static r_device const *const kDevices[] = {
#define DECL(name) &name,
    DEVICES
#undef DECL
};

unsigned onair_rtl433_device_count(void) { return (unsigned)(sizeof kDevices / sizeof *kDevices); }
r_device const *onair_rtl433_device(unsigned i) { return i < onair_rtl433_device_count() ? kDevices[i] : NULL; }
r_device const *onair_rtl433_flex(void) { return &flex_decoder; }

static _Thread_local jmp_buf *g_jb;

void onair_rtl433_exit(int code)
{
    if (g_jb) longjmp(*g_jb, code ? code : -1);
    abort();
}

r_device *onair_rtl433_flex_create(char const *spec)
{
    jmp_buf jb;
    jmp_buf *prev = g_jb;
    g_jb = &jb;
    r_device *d = NULL;
    if (setjmp(jb) == 0)
        d = flex_decoder.create_fn(spec);
    g_jb = prev;
    return d;
}
