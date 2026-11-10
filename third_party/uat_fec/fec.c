//
// Copyright 2015, Oliver Jowett <oliver@mutability.co.uk>
//

// This file is free software: you may copy, redistribute and/or modify it  
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your  
// option) any later version.  
//
// This file is distributed in the hope that it will be useful, but  
// WITHOUT ANY WARRANTY; without even the implied warranty of  
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU  
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License  
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "dect2/uat/uat_frame.h"
#include "rs.h"

static void *rs_uplink;
static void *rs_adsb_short;
static void *rs_adsb_long;

#define UPLINK_POLY 0x187
#define ADSB_POLY 0x187

void init_fec(void)
{
    rs_adsb_short = init_rs_char(8, /* gfpoly */ ADSB_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 12, /* pad */ 225);
    rs_adsb_long  = init_rs_char(8, /* gfpoly */ ADSB_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 14, /* pad */ 207);
    rs_uplink     = init_rs_char(8, /* gfpoly */ UPLINK_POLY, /* fcr */ 120, /* prim */ 1, /* nroots */ 20, /* pad */ 163);
}

int correct_adsb_frame(uint8_t *to, int *rs_errors)
{
    // Try decoding as a Long UAT.
    // We rely on decode_rs_char not modifying the data if there were
    // uncorrectable errors.
    int n_corrected = decode_rs_char(rs_adsb_long, to, NULL, 0);
    if (n_corrected >= 0 && n_corrected <= 7 && (to[0]>>3) != 0) {
        // Valid long frame.
        *rs_errors = n_corrected;
        return 2;
    }

    // Retry as Basic UAT
    n_corrected = decode_rs_char(rs_adsb_short, to, NULL, 0);
    if (n_corrected >= 0 && n_corrected <= 6 && (to[0]>>3) == 0) {
        // Valid short frame
        *rs_errors = n_corrected;
        return 1;
    }

    // Failed.
    *rs_errors = 9999;
    return -1;
}

// OnAir addition (3.9, docs/SENSITIVITY-3.9-C.md): the same as correct_adsb_frame, with erasures. eras_long / eras_short are byte indices into the
// frame (0 .. 47 / 0 .. 29) that the demodulator found least reliable; the decoder then corrects up to 2 errors per erasure fewer than it would
// otherwise need. Accepted only within the roots' capacity (2 errors + erasures <= roots) and with the frame type matching the length.
int correct_adsb_frame_eras(uint8_t *to, const int *eras_long, int n_long, const int *eras_short, int n_short, int *rs_errors)
{
    int pos[20];
    int i, n;
    if (n_long > 0 && n_long <= 14) {
        for (i = 0; i < n_long; ++i) pos[i] = 207 + eras_long[i];
        n = decode_rs_char(rs_adsb_long, to, pos, n_long);
        if (n >= 0 && n <= 14 && (to[0]>>3) != 0) { *rs_errors = n; return 2; }
    }
    if (n_short > 0 && n_short <= 12) {
        for (i = 0; i < n_short; ++i) pos[i] = 225 + eras_short[i];
        n = decode_rs_char(rs_adsb_short, to, pos, n_short);
        if (n >= 0 && n <= 12 && (to[0]>>3) == 0) { *rs_errors = n; return 1; }
    }
    *rs_errors = 9999;
    return -1;
}

// OnAir addition: one uplink block (92 bytes, deinterleaved) with erasures at byte indices 0 .. 91; returns the corrections or -1
int correct_uplink_block_eras(uint8_t *block, const int *eras, int n_eras)
{
    int pos[20];
    int i, n;
    if (n_eras < 0 || n_eras > 20) return -1;
    for (i = 0; i < n_eras; ++i) pos[i] = 163 + eras[i];
    n = decode_rs_char(rs_uplink, block, n_eras ? pos : NULL, n_eras);
    return (n >= 0 && n <= 20) ? n : -1;
}

int correct_uplink_frame(uint8_t *from, uint8_t *to, int *rs_errors)
{
    int block;
    int total_corrected = 0;

    for (block = 0; block < UPLINK_FRAME_BLOCKS; ++block) {
        int i, n_corrected;
        uint8_t *blockdata = &to[block * UPLINK_BLOCK_DATA_BYTES];

        for (i = 0; i < UPLINK_BLOCK_BYTES; ++i)
            blockdata[i] = from[i * UPLINK_FRAME_BLOCKS + block];

        // error-correct in place
        n_corrected = decode_rs_char(rs_uplink, blockdata, NULL, 0);
        if (n_corrected < 0 || n_corrected > 10) {
            // Failed
            *rs_errors = 9999;
            return -1;
        }

        total_corrected += n_corrected;
        // next block (if there is one) will overwrite the ECC bytes.
    }

    *rs_errors = total_corrected;
    return 1;
}
