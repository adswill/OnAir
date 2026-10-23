//! OnAir TETRA: the network information layer of ETSI EN 300 392-2, i.e. only what every cell broadcasts in the clear to all terminals:
//! the synchronisation PDU (system code, colour code, timeslot / frame / multiframe numbers, sharing mode, MCC, MNC), the system information
//! broadcast (carrier, band, duplex spacing, access parameters, location area, BS service details, security class), the neighbour cell and
//! network time broadcast (D-NWRK-BROADCAST), the ACCESS-ASSIGN slot usage and a counter-only walk over a signalling block. Nothing addressed to a
//! terminal is read: no terminal address is kept, no voice, no call or short data content; of the call set-up PDUs only the call identifier, the
//! encryption flag and the group (GSSI) a group call is set up for. The burst layers, receiver, generator and the encoders stay in C++.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod abi;
pub mod bits;
pub mod mac;
pub mod sysinfo;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
