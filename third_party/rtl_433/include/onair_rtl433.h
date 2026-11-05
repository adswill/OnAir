/* OnAir glue API over the vendored rtl_433 sources, see onair_glue.c */
#ifndef ONAIR_RTL433_H_
#define ONAIR_RTL433_H_
#include "r_device.h"
#ifdef __cplusplus
extern "C" {
#endif
unsigned onair_rtl433_device_count(void);
r_device const *onair_rtl433_device(unsigned i);   /* i = protocol number - 1 */
r_device const *onair_rtl433_flex(void);
r_device *onair_rtl433_flex_create(char const *spec);   /* NULL when the spec is not valid */
#ifdef __cplusplus
}
#endif
#endif
