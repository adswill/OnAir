// C ABI of the Rust crate rust/onair_tetra (TETRA network information: the broadcasts every cell sends to all terminals). Written by hand; the Rust
// test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte (0 / 1). No voice, no call or short data content and no subscriber address is read by any function; the one address kept is
// the group (GSSI) a clear group call is set up for.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {   // SYNC PDU + D-MLE-SYNC (EN 300 392-2 21.4.4.2, 18.4.2.1)
    int32_t system_code, colour_code, tn, fn_, mn, sharing_mode, ts_reserved_frames, uplane_dtx, frame18_ext, mcc, mnc, nbr_broadcast, cell_load, late_entry;
} OnairTetraSync;

typedef struct {   // SYSINFO + D-MLE-SYSINFO (21.4.4.1, 18.4.2.2)
    int32_t main_carrier, band, offset, duplex, reverse, num_scch, ms_tx_pwr_max, rx_lev_access_min, access_param, dl_timeout, cck_flag, hyperframe_or_cck, opt_flag;
    uint32_t opt_value;
    int32_t la, subscriber_class, bs_service;
} OnairTetraSysinfo;

typedef struct {   // a neighbour cell of a D-NWRK-BROADCAST; -1 = element not present
    int32_t cell_id, resel_types, cell_load, carrier, synced, has_ext, band, offset, duplex, reverse, mcc, mnc, la, max_tx, min_rx, subscriber_class,
        bs_service, timeshare, frame_offset;
} OnairTetraNeighbour;

typedef struct {   // a D-NWRK-BROADCAST; neighbours are nbr[nbr_first .. nbr_first + nbr_count)
    int32_t reselect, cell_load, has_time;
    uint32_t nbr_first, nbr_count, pad;
    uint64_t time48;
} OnairTetraNwrk;

typedef struct {   // call set-up facts of one clear CMCE PDU: kind 1 D-SETUP, 2 D-CONNECT, 3 D-DISCONNECT / D-RELEASE; enc 0 clear, 1 encrypted, -1 not stated;
                   // slots = "timeslot assigned" bit map (MSB = timeslot 1) and carrier = carrier number of the channel allocation of the same PDU, -1 when none;
                   // gssi = the group a D-SETUP of a group call is sent to, -1 otherwise (a call to one terminal keeps no address)
    int32_t kind, call_id, enc, slots, carrier, gssi;
} OnairTetraCallInfo;

typedef struct {   // what one signalling block held
    int32_t sysinfo, access_define, resources, encrypted, fragments, by_pd[8];
    uint32_t n_nwrk, n_nbr;
    int32_t n_call;
    OnairTetraCallInfo call[4];
    OnairTetraSysinfo si;
} OnairTetraMacBlock;

// ABI version of this module (4).
uint32_t onair_tetra_version(void);
// Sizes of the records above in this order: Sync, Sysinfo, Neighbour, Nwrk, MacBlock (out: 5 size_t).
void onair_tetra_struct_sizes(size_t* out);

// 60-bit SYNC PDU. *out is always written. Returns 2 valid, 1 too short or frame / multiframe out of range, 0 null output.
int32_t onair_tetra_parse_sync(const uint8_t* bits, size_t len, OnairTetraSync* out);
// 124-bit SYSINFO starting at the MAC PDU type. Returns 2 valid (written), 1 too short (written, zeros past the end), 0 not a SYSINFO (not written).
int32_t onair_tetra_parse_sysinfo(const uint8_t* bits, size_t len, OnairTetraSysinfo* out);
// Comma separated BS service names of a 12-bit details word, or "none"; writes at most cap bytes (no NUL), returns the count (needs about 200).
size_t onair_tetra_service_text(int32_t v, uint8_t* out, size_t cap);
// Extended services security: 0 not present, else bit 0 valid, 1 authentication required, 2 class 1, 3 class 2, 4 class 3.
int32_t onair_tetra_security(const OnairTetraSysinfo* si);
// Carrier frequencies in Hz; the duplex spacing is negative when reserved, the uplink 0 when unknown.
double onair_tetra_dl_carrier_hz(int32_t band, int32_t carrier, int32_t offset);
double onair_tetra_duplex_spacing_hz(int32_t band, int32_t duplex);
double onair_tetra_ul_carrier_hz(int32_t band, int32_t carrier, int32_t offset, int32_t duplex, int32_t reverse);
// Network time text of the 48-bit time (at most 96 bytes, no NUL); returns the length, 0 when not valid.
size_t onair_tetra_network_time_text(uint64_t t, uint8_t* out, size_t cap);
// Walk one signalling block of len bits. blk gets the counters, the SYSINFO and the number of broadcasts found; nwrk / nbr (capacities in elements) get the
// D-NWRK-BROADCASTs and their neighbours (beyond a capacity: counted, not stored). A block of len bits holds at most len / 8 + 1 broadcasts of 7 neighbours.
int32_t onair_tetra_parse_block(const uint8_t* bits, size_t len, OnairTetraMacBlock* blk, OnairTetraNwrk* nwrk, size_t nwrk_cap, OnairTetraNeighbour* nbr, size_t nbr_cap);
// ACCESS-ASSIGN downlink use of the slot: -1 not stated, 0 unallocated, 1 assigned control, 2 common control, 3 reserved, 4 and up traffic; and its name.
int32_t onair_tetra_aach_dl_usage(uint32_t aa, int32_t frame18);
const uint8_t* onair_tetra_dl_usage_text(int32_t u);

#ifdef __cplusplus
}
#endif
