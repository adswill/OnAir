// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The SPSC ring. Behaviour is that of `dect2::IqRing` (ring.h), operation for operation:
//!
//! * indexes `w` / `r` are free-running `usize` counters (wrapping), `w - r` is the fill, the slot is `index & mask`;
//! * `write` stores as many elements as fit and counts the rest in `dropped`; `read` returns what is there;
//! * `skip_to_newest` / `drop_backlog` / `clear` are consumer-side, `note_lost` / `reset_dropped` touch only the counter;
//! * `last_write` is a seqlock pair (offered total, time, size of the last write) written by the producer.
//!
//! Threads: exactly one producer (write, note_lost) and one consumer (read, skip_to_newest, drop_backlog, clear). Everybody may call the
//! const queries. Memory ordering, as in C++:
//!
//! * `write`: load own `w` Relaxed (only the producer stores it); load `r` Acquire, pairing with the consumer's Release store of `r`:
//!   every slot the consumer released (it finished reading it) is free before the producer overwrites it. Slots are copied, then `w` is
//!   stored Release, pairing with the consumer's Acquire load of `w`: the copied bytes are visible once the new `w` is.
//! * `read`: mirror image. Load own `r` Relaxed, `w` Acquire, copy out, then `r` Release.
//! * `skip_to_newest` / `clear` only move `r` forward after an Acquire load of `w`, with a Release store, like `read` (they release slots
//!   without reading them, which is fine: the producer only needs to know the slot is no longer going to be read).
//! * `dropped`, `offered` and friends are statistics: Relaxed. `dropped` is touched from both sides by `fetch_add`, so it needs no more.
//! * The stamp is a seqlock with one writer: `seq` is made odd (AcqRel add: nothing after it moves above), the data are stored Relaxed, and
//!   `seq` is made even again (Release add: the data stores do not move below). The reader loads `seq` Acquire, reads the data Relaxed, an
//!   Acquire fence, then re-reads `seq` Relaxed: if it is unchanged and even the three values come from the same write.

use core::sync::atomic::{fence, AtomicI64, AtomicU64, AtomicUsize, Ordering};

/// An atomic on its own cache line pair (128 bytes also covers the adjacent-line prefetcher and Apple silicon's 128-byte lines), so the
/// producer's `w` and the consumer's `r` never share a line (the C++ ring has them side by side).
#[repr(C, align(128))]
pub struct Padded<T>(pub T);

/// The header block. `#[repr(C)]`; the C++ wrapper reserves `onair_ring_hdr_size()` bytes, aligned to 128, for it.
#[repr(C)]
pub struct Ring {
    w: Padded<AtomicUsize>,
    r: Padded<AtomicUsize>,
    // statistics and the stamp: producer-written, rarely read; one line for all of them
    dropped: Padded<AtomicU64>,
    seq: AtomicU64,
    offered: AtomicU64,
    write_ns: AtomicI64,
    last_n: AtomicUsize,
    buf: *mut u8,
    elem: usize,
    cap: usize,
    mask: usize,
}

// The raw pointer is the storage owned by the C++ side for the life of the ring; the rules above make the shared use sound.
unsafe impl Send for Ring {}
unsafe impl Sync for Ring {}

impl Ring {
    /// An empty ring over `buf` (`cap` elements of `elem` bytes; `cap` a power of two, not 0). `start` is the initial value of both
    /// indexes (0 normally; tests use it to cross the wrap of the counters).
    ///
    /// # Safety
    /// `buf` must be valid for `cap * elem` bytes for reads and writes for as long as the ring is used, and not used otherwise.
    pub unsafe fn new(buf: *mut u8, cap: usize, elem: usize, start: usize) -> Option<Ring> {
        if buf.is_null() || cap == 0 || !cap.is_power_of_two() || elem == 0 || cap.checked_mul(elem).is_none() {
            return None;
        }
        Some(Ring {
            w: Padded(AtomicUsize::new(start)),
            r: Padded(AtomicUsize::new(start)),
            dropped: Padded(AtomicU64::new(0)),
            seq: AtomicU64::new(0),
            offered: AtomicU64::new(0),
            write_ns: AtomicI64::new(0),
            last_n: AtomicUsize::new(0),
            buf,
            elem,
            cap,
            mask: cap - 1,
        })
    }

    pub fn capacity(&self) -> usize {
        self.cap
    }

    /// Two-piece copy in: up to the end of the storage, then from its start.
    ///
    /// # Safety
    /// `src` valid for `m * elem` bytes; the slots `pos .. pos + m` belong to the producer (free per the protocol).
    unsafe fn copy_in(&self, pos: usize, src: *const u8, m: usize) {
        let at = pos & self.mask;
        let first = m.min(self.cap - at);
        core::ptr::copy_nonoverlapping(src, self.buf.add(at * self.elem), first * self.elem);
        if m > first {
            core::ptr::copy_nonoverlapping(src.add(first * self.elem), self.buf, (m - first) * self.elem);
        }
    }

    /// # Safety
    /// `dst` valid for `m * elem` bytes; the slots `pos .. pos + m` hold published data owned by the consumer.
    unsafe fn copy_out(&self, pos: usize, dst: *mut u8, m: usize) {
        let at = pos & self.mask;
        let first = m.min(self.cap - at);
        core::ptr::copy_nonoverlapping(self.buf.add(at * self.elem), dst, first * self.elem);
        if m > first {
            core::ptr::copy_nonoverlapping(self.buf, dst.add(first * self.elem), (m - first) * self.elem);
        }
    }

    /// Producer. Returns the number written; the elements that do not fit are counted as dropped. `now_ns` stamps the write.
    ///
    /// # Safety
    /// `src` valid for `n * elem` bytes; called from the producer thread only.
    pub unsafe fn write(&self, src: *const u8, n: usize, now_ns: i64) -> usize {
        let w = self.w.0.load(Ordering::Relaxed);
        let r = self.r.0.load(Ordering::Acquire);
        let free = self.cap - w.wrapping_sub(r);
        let m = n.min(free);
        if m > 0 {
            self.copy_in(w, src, m);
        }
        self.w.0.store(w.wrapping_add(m), Ordering::Release);
        if m < n {
            self.dropped.0.fetch_add((n - m) as u64, Ordering::Relaxed);
        }
        self.seq.fetch_add(1, Ordering::AcqRel);
        self.offered.store(self.offered.load(Ordering::Relaxed).wrapping_add(n as u64), Ordering::Relaxed);
        self.write_ns.store(now_ns, Ordering::Relaxed);
        self.last_n.store(n, Ordering::Relaxed);
        self.seq.fetch_add(1, Ordering::Release);
        m
    }

    /// Consumer. Returns the number read.
    ///
    /// # Safety
    /// `dst` valid for `n * elem` bytes; called from the consumer thread only.
    pub unsafe fn read(&self, dst: *mut u8, n: usize) -> usize {
        let r = self.r.0.load(Ordering::Relaxed);
        let w = self.w.0.load(Ordering::Acquire);
        let m = n.min(w.wrapping_sub(r));
        if m > 0 {
            self.copy_out(r, dst, m);
        }
        self.r.0.store(r.wrapping_add(m), Ordering::Release);
        m
    }

    /// Consumer. Keeps the newest `keep` elements; returns how many were discarded (counted as dropped).
    pub fn skip_to_newest(&self, keep: usize) -> usize {
        let r = self.r.0.load(Ordering::Relaxed);
        let w = self.w.0.load(Ordering::Acquire);
        let avail = w.wrapping_sub(r);
        if avail <= keep {
            return 0;
        }
        let skip = avail - keep;
        self.r.0.store(r.wrapping_add(skip), Ordering::Release);
        self.dropped.0.fetch_add(skip as u64, Ordering::Relaxed);
        skip
    }

    /// Consumer. More than half the ring in backlog: jump to the newest sixteenth.
    pub fn drop_backlog(&self) -> usize {
        if self.available() > self.cap / 2 {
            self.skip_to_newest(self.cap / 16)
        } else {
            0
        }
    }

    pub fn available(&self) -> usize {
        // C++ loads w then r, both Acquire; same here. Wrapping: a racing r can make this momentarily "negative", as in C++.
        let w = self.w.0.load(Ordering::Acquire);
        let r = self.r.0.load(Ordering::Acquire);
        w.wrapping_sub(r)
    }

    pub fn dropped(&self) -> u64 {
        self.dropped.0.load(Ordering::Relaxed)
    }

    pub fn note_lost(&self, n: u64) {
        self.dropped.0.fetch_add(n, Ordering::Relaxed);
    }

    /// Consumer: discard everything currently written (not counted as dropped).
    pub fn clear(&self) {
        let w = self.w.0.load(Ordering::Acquire);
        self.r.0.store(w, Ordering::Release);
    }

    pub fn reset_dropped(&self) {
        self.dropped.0.store(0, Ordering::Relaxed);
    }

    /// (offered total, time of the last write, its size); `None` while nothing was written yet. Spins while a write is in flight.
    pub fn last_write(&self) -> Option<(u64, i64, usize)> {
        loop {
            let s0 = self.seq.load(Ordering::Acquire);
            if s0 & 1 != 0 {
                core::hint::spin_loop();
                continue;
            }
            let offered = self.offered.load(Ordering::Relaxed);
            let ns = self.write_ns.load(Ordering::Relaxed);
            let n = self.last_n.load(Ordering::Relaxed);
            fence(Ordering::Acquire);
            if self.seq.load(Ordering::Relaxed) == s0 {
                return if s0 != 0 { Some((offered, ns, n)) } else { None };
            }
        }
    }
}
