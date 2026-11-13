// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* Reference harness for the differential tests of rust/onair_usb.
 *
 * Links the ORIGINAL librtlsdr (RTL-SDR Blog fork 1.3.6: librtlsdr.c + tuner_*.c) against a FAKE libusb that models the RTL2832U register file,
 * the I2C repeater, an R820T / R828D / E4000 / none tuner and the EEPROM, and writes every USB transfer to stdout. The Rust tests replay the same
 * command line against rust/onair_usb and require the same transfers. No hardware, no libusb library needed (only its header).
 *
 *   ref_harness <model> <command>...        see tests/golden/scenarios.txt
 *
 * model = comma separated: chip: r820t | r828d | e4000 | fc0013 | fc2580 | fc0012 | none
 *                          dongle strings: generic | blogv3 | blogv4 | blogv4l
 *                          lock | nolock (PLL lock bit), vft0..vft3 (R82xx VCO fine tune), cal0..cal15 (filter calibration code), eepbt (EEPROM forces the bias-T),
 *                          failN (the Nth control OUT transfer fails with -1)
 * Not part of the build; tools: see tests/ref/gen_golden.sh. Compile: cc -I<rtl-sdr-blog>/include -I<libusb-1.0> ... (see the script).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libusb.h>
#include "rtl-sdr.h"

/* ---------------------------------------------------------------- the fake device */
static struct {
    uint8_t blk[8][0x10000];     /* USB / SYS blocks: byte per address */
    uint8_t demod[32][256];      /* demodulator pages */
    uint8_t chip_regs[256][256]; /* I2C devices by 8-bit address */
    uint8_t present[256];
    uint8_t ptr[256];
    uint8_t r82_status[5];       /* raw (bit reversed) status bytes of the R82xx */
    uint8_t eeprom[256];
    int e4k_lock;
    int fail_out; /* 1-based number of the OUT transfer that fails, 0 none */
    int n_out;
    const char *vendor, *product;
} dev;

static uint8_t bitrev(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= 1 << (7 - i);
    return r;
}

static void hex(const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) printf("%02x", d[i]);
}

static void model_init(const char *spec) {
    memset(&dev, 0, sizeof dev);
    dev.vendor = "Realtek";
    dev.product = "RTL2838UHIDIR";
    int lock = 1, vft = 2, cal = 2;
    memset(dev.eeprom, 0xff, sizeof dev.eeprom);
    dev.eeprom[7] = 0x02; /* IR endpoint bit set: no forced bias-T (default) */
    for (int i = 0; i < 256; i++) dev.eeprom[i] = (i == 7) ? 0x02 : (uint8_t)(i * 5 + 1);
    char buf[256];
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, "r820t")) dev.present[0x34] = 1, dev.r82_status[0] = 0x69;
        else if (!strcmp(t, "r828d")) dev.present[0x74] = 1, dev.r82_status[0] = 0x69;
        else if (!strcmp(t, "e4000")) dev.present[0xc8] = 1, dev.chip_regs[0xc8][2] = 0x40;
        else if (!strcmp(t, "fc0013")) dev.present[0xc6] = 1, dev.chip_regs[0xc6][0] = 0xa3;
        else if (!strcmp(t, "fc0012")) dev.present[0xc6] = 1, dev.chip_regs[0xc6][0] = 0xa1;
        else if (!strcmp(t, "fc2580")) dev.present[0xac] = 1, dev.chip_regs[0xac][1] = 0x56;
        else if (!strcmp(t, "none")) {}
        else if (!strcmp(t, "generic")) {}
        else if (!strcmp(t, "blogv3")) dev.vendor = "RTLSDRBlog", dev.product = "Blog V3";
        else if (!strcmp(t, "blogv4")) dev.vendor = "RTLSDRBlog", dev.product = "Blog V4";
        else if (!strcmp(t, "blogv4l")) dev.vendor = "RTLSDRBlog", dev.product = "Blog V4L";
        else if (!strcmp(t, "lock")) lock = 1;
        else if (!strcmp(t, "nolock")) lock = 0;
        else if (!strncmp(t, "vft", 3)) vft = atoi(t + 3);
        else if (!strncmp(t, "cal", 3)) cal = atoi(t + 3);
        else if (!strcmp(t, "eepbt")) dev.eeprom[7] = 0x00;
        else if (!strncmp(t, "fail", 4)) dev.fail_out = atoi(t + 4);
        else { fprintf(stderr, "unknown model token %s\n", t); exit(2); }
    }
    dev.e4k_lock = lock;
    dev.r82_status[1] = 0x00;
    dev.r82_status[2] = bitrev((uint8_t)(lock ? 0x40 | 0x15 : 0x15));
    dev.r82_status[3] = bitrev(0x5a);
    dev.r82_status[4] = bitrev((uint8_t)((vft << 4) | cal));
}

static int i2c_in(uint8_t addr, uint8_t *data, int len) {
    if (!dev.present[addr] && addr != 0xa0) return LIBUSB_ERROR_PIPE;
    for (int i = 0; i < len; i++) {
        uint8_t p = dev.ptr[addr];
        uint8_t v;
        if (addr == 0xa0) v = dev.eeprom[p];
        else if (addr == 0x34 || addr == 0x74) v = p < 5 ? dev.r82_status[p] : bitrev(dev.chip_regs[addr][p]);
        else if (addr == 0xc8 && p == 0x07) v = (dev.chip_regs[addr][p] & ~1) | (dev.e4k_lock ? 1 : 0);
        else v = dev.chip_regs[addr][p];
        data[i] = v;
        dev.ptr[addr] = p + 1;
    }
    return len;
}

static int i2c_out(uint8_t addr, const uint8_t *data, int len) {
    if (addr == 0xa0) { /* the EEPROM is always there */
        if (len >= 1) dev.ptr[addr] = data[0];
        if (len == 2) dev.eeprom[data[0]] = data[1];
        return len;
    }
    if (!dev.present[addr]) return LIBUSB_ERROR_PIPE;
    if (len >= 1) {
        dev.ptr[addr] = data[0];
        for (int i = 1; i < len; i++) dev.chip_regs[addr][(uint8_t)(data[0] + i - 1)] = data[i];
    }
    return len;
}

/* ---------------------------------------------------------------- fake libusb */
struct libusb_device { int dummy; };
struct libusb_device_handle { int dummy; };
struct libusb_context { int dummy; };
static struct libusb_device g_dev;
static struct libusb_device_handle g_handle;
static struct libusb_context g_ctx;
static struct libusb_device *g_list[2] = { &g_dev, NULL };

int libusb_init(libusb_context **ctx) { *ctx = &g_ctx; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) { (void)ctx; *list = g_list; return 1; }
void libusb_free_device_list(libusb_device **list, int unref) { (void)list; (void)unref; }
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *dd) {
    (void)d;
    memset(dd, 0, sizeof *dd);
    dd->idVendor = 0x0bda; dd->idProduct = 0x2838; dd->iManufacturer = 1; dd->iProduct = 2; dd->iSerialNumber = 3;
    return 0;
}
int libusb_open(libusb_device *d, libusb_device_handle **h) { (void)d; *h = &g_handle; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
libusb_device *libusb_get_device(libusb_device_handle *h) { (void)h; return &g_dev; }
int libusb_kernel_driver_active(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_attach_kernel_driver(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_reset_device(libusb_device_handle *h) { (void)h; printf("T X reset\n"); return 0; }
int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length) {
    (void)h;
    const char *s = idx == 1 ? dev.vendor : idx == 2 ? dev.product : "00000001";
    int n = (int)strlen(s);
    if (n > length - 1) n = length - 1;
    memcpy(data, s, (size_t)n);
    data[n] = 0;
    printf("T S %d -> %d %s\n", idx, n, s);
    return n;
}
struct libusb_transfer *libusb_alloc_transfer(int n) { (void)n; return NULL; }
void libusb_free_transfer(struct libusb_transfer *t) { (void)t; }
int libusb_submit_transfer(struct libusb_transfer *t) { (void)t; return -1; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; return -1; }
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *x) { (void)c; (void)tv; (void)x; return -1; }
unsigned char *libusb_dev_mem_alloc(libusb_device_handle *h, size_t n) { (void)h; (void)n; return NULL; }
int libusb_dev_mem_free(libusb_device_handle *h, unsigned char *b, size_t n) { (void)h; (void)b; (void)n; return -1; }

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int length, int *actual, unsigned int timeout) {
    (void)h;
    for (int i = 0; i < length; i++) data[i] = (uint8_t)(i * 7 + 3);
    *actual = length;
    printf("T B %02x %d %u -> 0 %d\n", ep, length, timeout, length);
    return 0;
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, unsigned char *data, uint16_t length,
                            unsigned int timeout) {
    (void)h;
    int ret = length;
    if (rt & 0x80) { /* IN */
        if (index < 0x100) { /* demodulator: wValue = (reg << 8) | 0x20, wIndex = page */
            for (int i = 0; i < length; i++) data[i] = dev.demod[index & 31][(uint8_t)((value >> 8) + i)];
        } else if ((index >> 8) == 6) {
            ret = i2c_in((uint8_t)value, data, length);
        } else {
            for (int i = 0; i < length; i++) data[i] = dev.blk[(index >> 8) & 7][(uint16_t)(value + i)];
        }
        printf("T I %02x %02x %04x %04x %u %u -> %d ", rt, req, value, index, length, timeout, ret);
        if (ret > 0) hex(data, ret);
        printf("\n");
    } else {
        dev.n_out++;
        printf("T O %02x %02x %04x %04x %u %u ", rt, req, value, index, length, timeout);
        hex(data, length);
        if (dev.fail_out && dev.n_out == dev.fail_out) {
            printf(" -> -1\n");
            return -1;
        }
        if (index < 0x100) {
            for (int i = 0; i < length; i++) dev.demod[index & 31][(uint8_t)((value >> 8) + i)] = data[i];
        } else if ((index >> 8) == 6) {
            ret = i2c_out((uint8_t)value, data, length);
        } else {
            for (int i = 0; i < length; i++) dev.blk[(index >> 8) & 7][(uint16_t)(value + i)] = data[i];
        }
        printf(" -> %d\n", ret);
    }
    return ret;
}

/* ---------------------------------------------------------------- command interpreter */
static rtlsdr_dev_t *d;

static void R(const char *name, long long v) { printf("R %s %lld\n", name, v); }

static int run(const char *cmd) {
    char c[128];
    strncpy(c, cmd, sizeof c - 1);
    c[sizeof c - 1] = 0;
    char *eq = strchr(c, '=');
    const char *arg = "";
    if (eq) { *eq = 0; arg = eq + 1; }
    long long n = strtoll(arg, NULL, 0);
    if (!strcmp(c, "open")) { int r = rtlsdr_open(&d, 0); R("open", r); if (r) d = NULL; }
    else if (!d) { R(c, 0x7fffffff); }
    else if (!strcmp(c, "close")) { R("close", rtlsdr_close(d)); d = NULL; }
    else if (!strcmp(c, "cf")) R("cf", rtlsdr_set_center_freq(d, (uint32_t)n));
    else if (!strcmp(c, "sr")) R("sr", rtlsdr_set_sample_rate(d, (uint32_t)n));
    else if (!strcmp(c, "ppm")) R("ppm", rtlsdr_set_freq_correction(d, (int)n));
    else if (!strcmp(c, "gm")) R("gm", rtlsdr_set_tuner_gain_mode(d, (int)n));
    else if (!strcmp(c, "g")) R("g", rtlsdr_set_tuner_gain(d, (int)n));
    else if (!strcmp(c, "ig")) { int st = (int)strtol(arg, NULL, 0); const char *col = strchr(arg, ':'); R("ig", rtlsdr_set_tuner_if_gain(d, st, col ? atoi(col + 1) : 0)); }
    else if (!strcmp(c, "bw")) R("bw", rtlsdr_set_tuner_bandwidth(d, (uint32_t)n));
    else if (!strcmp(c, "agc")) R("agc", rtlsdr_set_agc_mode(d, (int)n));
    else if (!strcmp(c, "ds")) R("ds", rtlsdr_set_direct_sampling(d, (int)n));
    else if (!strcmp(c, "off")) R("off", rtlsdr_set_offset_tuning(d, (int)n));
    else if (!strcmp(c, "bias")) R("bias", rtlsdr_set_bias_tee(d, (int)n));
    else if (!strcmp(c, "biasgpio")) { int g = (int)strtol(arg, NULL, 0); const char *col = strchr(arg, ':'); R("biasgpio", rtlsdr_set_bias_tee_gpio(d, g, col ? atoi(col + 1) : 0)); }
    else if (!strcmp(c, "reset")) R("reset", rtlsdr_reset_buffer(d));
    else if (!strcmp(c, "test")) R("test", rtlsdr_set_testmode(d, (int)n));
    else if (!strcmp(c, "read")) { static uint8_t b[65536]; int got = 0; int r = rtlsdr_read_sync(d, b, (int)n, &got); R("read", r); R("read_n", got); }
    else if (!strcmp(c, "xtal")) { uint32_t a = (uint32_t)strtoul(arg, NULL, 0); const char *col = strchr(arg, ':'); R("xtal", rtlsdr_set_xtal_freq(d, a, col ? (uint32_t)strtoul(col + 1, NULL, 0) : 0)); }
    else if (!strcmp(c, "eeprom_r")) { uint8_t b[256]; int off = (int)strtol(arg, NULL, 0); const char *col = strchr(arg, ':'); int len = col ? atoi(col + 1) : 1; int r = rtlsdr_read_eeprom(d, b, (uint8_t)off, (uint16_t)len); R("eeprom_r", r); if (r >= 0) { printf("R eeprom "); hex(b, len); printf("\n"); } }
    else if (!strcmp(c, "eeprom_w")) { uint8_t b[16]; int off = (int)strtol(arg, NULL, 0); const char *col = strchr(arg, ':'); int len = 0; if (col) for (const char *p = col + 1; p[0] && p[1] && len < 16; p += 2) { char t[3] = { p[0], p[1], 0 }; b[len++] = (uint8_t)strtol(t, NULL, 16); } R("eeprom_w", rtlsdr_write_eeprom(d, b, (uint8_t)off, (uint16_t)len)); }
    else if (!strcmp(c, "info")) {
        R("tuner", rtlsdr_get_tuner_type(d)); R("freq", rtlsdr_get_center_freq(d)); R("rate", rtlsdr_get_sample_rate(d));
        R("corr", rtlsdr_get_freq_correction(d)); R("dsm", rtlsdr_get_direct_sampling(d)); R("offs", rtlsdr_get_offset_tuning(d)); R("gain", rtlsdr_get_tuner_gain(d));
        int gains[40]; int cnt = rtlsdr_get_tuner_gains(d, gains); R("ngains", cnt);
        printf("R gains"); for (int i = 0; i < cnt; i++) printf(" %d", gains[i]); printf("\n");
    }
    else { fprintf(stderr, "unknown command %s\n", cmd); return 2; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    model_init(argv[1]);
    for (int i = 2; i < argc; i++) if (run(argv[i])) return 2;
    if (d) rtlsdr_close(d);
    return 0;
}
