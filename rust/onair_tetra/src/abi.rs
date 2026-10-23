//! `repr(C)` records shared with C++ (core/include/dect2/rs/tetra.h). All integers are `i32` so the layout is the same everywhere.

/// SYNC PDU (BSCH, 60 bits), EN 300 392-2 clause 21.4.4.1 and D-MLE-SYNC 18.4.2.1.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Sync {
    pub system_code: i32,
    pub colour_code: i32,
    pub tn: i32,
    pub fn_: i32,
    pub mn: i32,
    pub sharing_mode: i32,
    pub ts_reserved_frames: i32,
    pub uplane_dtx: i32,
    pub frame18_ext: i32,
    pub mcc: i32,
    pub mnc: i32,
    pub nbr_broadcast: i32,
    pub cell_load: i32,
    pub late_entry: i32,
}

/// SYSINFO PDU with D-MLE-SYSINFO (BNCH, 124 bits), 21.4.4.1 and 18.4.2.2.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Sysinfo {
    pub main_carrier: i32,
    pub band: i32,
    pub offset: i32,
    pub duplex: i32,
    pub reverse: i32,
    pub num_scch: i32,
    pub ms_tx_pwr_max: i32,
    pub rx_lev_access_min: i32,
    pub access_param: i32,
    pub dl_timeout: i32,
    pub cck_flag: i32,
    pub hyperframe_or_cck: i32,
    pub opt_flag: i32,
    pub opt_value: u32,
    pub la: i32,
    pub subscriber_class: i32,
    pub bs_service: i32,
}

/// One neighbour cell of a D-NWRK-BROADCAST (18.4.2.x); `-1` marks an element that was not present.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Neighbour {
    pub cell_id: i32,
    pub resel_types: i32,
    pub cell_load: i32,
    pub carrier: i32,
    pub synced: i32,
    pub has_ext: i32,
    pub band: i32,
    pub offset: i32,
    pub duplex: i32,
    pub reverse: i32,
    pub mcc: i32,
    pub mnc: i32,
    pub la: i32,
    pub max_tx: i32,
    pub min_rx: i32,
    pub subscriber_class: i32,
    pub bs_service: i32,
    pub timeshare: i32,
    pub frame_offset: i32,
}

impl Default for Neighbour {
    fn default() -> Self {
        Neighbour {
            cell_id: 0,
            resel_types: 0,
            cell_load: 0,
            carrier: 0,
            synced: 0,
            has_ext: 0,
            band: 0,
            offset: 0,
            duplex: 0,
            reverse: 0,
            mcc: -1,
            mnc: -1,
            la: -1,
            max_tx: -1,
            min_rx: -1,
            subscriber_class: -1,
            bs_service: -1,
            timeshare: -1,
            frame_offset: -1,
        }
    }
}

/// A D-NWRK-BROADCAST; its neighbours are `nbr[nbr_first .. nbr_first + nbr_count]` of the neighbour array of the call.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Nwrk {
    pub reselect: i32,
    pub cell_load: i32,
    pub has_time: i32,
    pub nbr_first: u32,
    pub nbr_count: u32,
    pub pad: u32,
    pub time48: u64,
}

/// The call set-up facts of one clear CMCE PDU (ETSI EN 300 392-2 clause 14): kind 1 D-SETUP, 2 D-CONNECT, 3 D-DISCONNECT / D-RELEASE,
/// 4 D-TX-GRANTED / D-TX-CONTINUE / D-TX-CEASED (the call goes on: a new talk spurt, or the end of one); the call
/// identifier; `enc` the end-to-end encryption flag of the basic service information (0 clear, 1 encrypted, -1 not stated); `slots` the
/// "timeslot assigned" bit map (MSB = timeslot 1) and `carrier` the carrier number of the channel allocation of the same MAC-RESOURCE, -1 when it has none;
/// `gssi` the group (GSSI) a D-SETUP of a group call is addressed to, -1 otherwise (a call to a single terminal keeps no address).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct CallInfo {
    pub kind: i32,
    pub call_id: i32,
    pub enc: i32,
    pub slots: i32,
    pub carrier: i32,
    pub gssi: i32,
}

/// Call events kept per block.
pub const MAX_CALLS: usize = 4;

/// What a signalling block held (counters and the SYSINFO), see `mac::parse_block`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct MacBlock {
    pub sysinfo: i32,
    pub access_define: i32,
    pub resources: i32,
    pub encrypted: i32,
    pub fragments: i32,
    pub by_pd: [i32; 8],
    pub n_nwrk: u32,
    pub n_nbr: u32,
    pub n_call: i32,
    pub call: [CallInfo; MAX_CALLS],
    pub si: Sysinfo,
}
