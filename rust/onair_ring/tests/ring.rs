// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Tests of the Rust SPSC ring: against a trivially correct model (random operation sequences, wrapped counters), threaded stress, and an
//! exhaustive search of the interleavings of a step-by-step model of the algorithm (a std-only stand-in for loom: sequentially consistent
//! steps, so it checks the logic, not the memory orderings; those are argued in ring.rs and checked by ThreadSanitizer on the C++ side).
use onair_ring::ring::Ring;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}

/// Ring plus its storage, made movable between threads.
struct Fx {
    ring: Ring,
    _buf: Vec<u64>,
}
unsafe impl Send for Fx {}
unsafe impl Sync for Fx {}
fn fx(cap: usize, start: usize) -> Fx {
    let mut buf = vec![0u64; cap];
    let ring = unsafe { Ring::new(buf.as_mut_ptr() as *mut u8, cap, 8, start) }.unwrap();
    Fx { ring, _buf: buf }
}
fn wr(r: &Ring, v: &[u64], ns: i64) -> usize {
    unsafe { r.write(v.as_ptr() as *const u8, v.len(), ns) }
}
fn rd(r: &Ring, n: usize) -> Vec<u64> {
    let mut v = vec![0u64; n];
    let m = unsafe { r.read(v.as_mut_ptr() as *mut u8, n) };
    v.truncate(m);
    v
}

#[test]
fn header_fits_the_cpp_reservation() {
    // ONAIR_RING_HDR_BYTES in core/include/dect2/rs/ring.h
    assert!(core::mem::size_of::<Ring>() <= 640 && core::mem::align_of::<Ring>() <= 128);
}

#[test]
fn rejects_bad_arguments() {
    let mut b = [0u8; 64];
    unsafe {
        assert!(Ring::new(b.as_mut_ptr(), 0, 8, 0).is_none());
        assert!(Ring::new(b.as_mut_ptr(), 3, 8, 0).is_none());
        assert!(Ring::new(core::ptr::null_mut(), 8, 8, 0).is_none());
        assert!(Ring::new(b.as_mut_ptr(), 8, 0, 0).is_none());
        assert!(Ring::new(b.as_mut_ptr(), 8, 8, 0).is_some());
    }
}

/// The reference: a deque with the ring's documented rules.
struct Model {
    q: VecDeque<u64>,
    cap: usize,
    dropped: u64,
}
impl Model {
    fn write(&mut self, v: &[u64]) -> usize {
        let m = v.len().min(self.cap - self.q.len());
        self.q.extend(&v[..m]);
        self.dropped += (v.len() - m) as u64;
        m
    }
    fn read(&mut self, n: usize) -> Vec<u64> {
        let m = n.min(self.q.len());
        self.q.drain(..m).collect()
    }
    fn skip(&mut self, keep: usize) -> usize {
        if self.q.len() <= keep {
            return 0;
        }
        let s = self.q.len() - keep;
        self.q.drain(..s);
        self.dropped += s as u64;
        s
    }
}

fn random_ops(cap: usize, start: usize, seed: u64, steps: usize) {
    let f = fx(cap, start);
    let mut m = Model { q: VecDeque::new(), cap, dropped: 0 };
    let mut rng = Rng(seed);
    let mut next = 1u64;
    let mut offered = 0u64;
    let mut last = None;
    for _ in 0..steps {
        match rng.below(10) {
            0..=3 => {
                let n = rng.below(cap * 2 + 2);
                let v: Vec<u64> = (0..n)
                    .map(|_| {
                        next += 1;
                        next
                    })
                    .collect();
                let ns = rng.next() as i64;
                assert_eq!(wr(&f.ring, &v, ns), m.write(&v));
                offered += n as u64;
                last = Some((offered, ns, n));
            }
            4..=6 => {
                let n = rng.below(cap * 2 + 2);
                assert_eq!(rd(&f.ring, n), m.read(n));
            }
            7 => {
                let keep = rng.below(cap + 2);
                assert_eq!(f.ring.skip_to_newest(keep), m.skip(keep));
            }
            8 => match rng.below(4) {
                0 => assert_eq!(f.ring.drop_backlog(), if m.q.len() > cap / 2 { m.skip(cap / 16) } else { 0 }),
                1 => {
                    f.ring.clear();
                    m.q.clear();
                }
                2 => {
                    f.ring.note_lost(5);
                    m.dropped += 5;
                }
                _ => {
                    f.ring.reset_dropped();
                    m.dropped = 0;
                }
            },
            _ => {}
        }
        assert_eq!(f.ring.available(), m.q.len());
        assert_eq!(f.ring.dropped(), m.dropped);
        assert_eq!(f.ring.last_write(), last);
    }
}

#[test]
fn random_sequences_match_the_model() {
    for (i, &cap) in [1usize, 2, 4, 16, 64, 1024].iter().enumerate() {
        for seed in 1..=40u64 {
            random_ops(cap, 0, seed * 7919 + i as u64, 600);
        }
    }
}

#[test]
fn counters_wrap() {
    // indexes start just below usize::MAX: they overflow within the first writes
    for cap in [2usize, 8, 256] {
        for off in [1usize, 3, cap + 1] {
            random_ops(cap, usize::MAX - off, 99 + cap as u64, 2000);
        }
    }
}

#[test]
fn nothing_written_has_no_stamp() {
    let f = fx(8, 0);
    assert_eq!(f.ring.last_write(), None);
    wr(&f.ring, &[], 5);
    assert_eq!(f.ring.last_write(), Some((0, 5, 0)));
}

fn stress(cap: usize, total: u64, max_chunk: usize, block: bool) {
    let f = Arc::new(fx(cap, 0));
    let done = Arc::new(AtomicBool::new(false));
    let (pf, pd) = (f.clone(), done.clone());
    let prod = std::thread::spawn(move || {
        let mut rng = Rng(12345);
        let mut sent = 0u64; // next value to offer
        let mut accepted = 0u64;
        while sent < total {
            let n = (1 + rng.below(max_chunk)).min((total - sent) as usize);
            let v: Vec<u64> = (0..n as u64).map(|i| sent + i).collect();
            let mut off = 0;
            // blocking mode: retry what did not fit so that nothing is lost; lossy mode: the ring counts the loss
            loop {
                let m = wr(&pf.ring, &v[off..], 0);
                accepted += m as u64;
                off += m;
                if !block || off == n {
                    break;
                }
                std::hint::spin_loop();
            }
            sent += n as u64;
        }
        pd.store(true, Ordering::Release);
        accepted
    });
    let cf = f.clone();
    let cons = std::thread::spawn(move || {
        let mut rng = Rng(777);
        let mut seen = 0u64;
        let mut prev: Option<u64> = None;
        let mut gaps = 0u64;
        loop {
            let fin = done.load(Ordering::Acquire);
            let v = rd(&cf.ring, 1 + rng.below(max_chunk));
            if v.is_empty() && fin && cf.ring.available() == 0 {
                break;
            }
            for x in v {
                if let Some(p) = prev {
                    assert!(x > p, "out of order or duplicate: {x} after {p}");
                    if x != p + 1 {
                        gaps += 1;
                    }
                }
                prev = Some(x);
                seen += 1;
            }
        }
        (seen, gaps)
    });
    let accepted = prod.join().unwrap();
    let (seen, gaps) = cons.join().unwrap();
    assert_eq!(seen, accepted, "every accepted element is read exactly once");
    if !block {
        assert_eq!(accepted + f.ring.dropped(), total, "accepted + dropped = offered");
    } else {
        assert_eq!(accepted, total);
        assert_eq!(gaps, 0);
    }
}

#[test]
fn stress_blocking_no_loss_no_dup() {
    stress(64, 3_000_000, 17, true);
    stress(4096, 6_000_000, 1000, true);
    stress(2, 200_000, 3, true);
}

#[test]
fn stress_lossy_counts_every_loss() {
    stress(256, 3_000_000, 200, false);
}

// ---- exhaustive interleavings of a step model ------------------------------------------------------------------------------------------

#[derive(Clone, Hash, PartialEq, Eq)]
struct S {
    w: u32,
    r: u32,
    slots: [i32; 4],
    // producer: index of the next write op, program counter, locals (w, r, accepted, copy progress)
    pi: u32,
    ppc: u8,
    pw: u32,
    pm: u32,
    pk: u32,
    // consumer
    ci: u32,
    cpc: u8,
    cr: u32,
    cm: u32,
    ck: u32,
    got: Vec<i32>,
    sent: Vec<i32>,
}
const CAP: u32 = 4;
const WRITES: &[u32] = &[3, 2, 3, 2];
const READS: u32 = 4;
const READ_N: u32 = 3;

fn step_p(s: &mut S) -> bool {
    if s.pi as usize >= WRITES.len() {
        return false;
    }
    let n = WRITES[s.pi as usize];
    match s.ppc {
        0 => {
            s.pw = s.w;
            s.ppc = 1;
        }
        1 => {
            let pr = s.r;
            s.pm = n.min(CAP - (s.pw - pr));
            s.ppc = 2;
        }
        // the copy is one step per slot: a consumer step may run between any two of them
        2 => {
            if s.pk < s.pm {
                let v = s.sent.len() as i32 + 1;
                s.slots[((s.pw + s.pk) % CAP) as usize] = v;
                s.sent.push(v);
                s.pk += 1;
            } else {
                s.pk = 0;
                s.ppc = 3;
            }
        }
        _ => {
            s.w = s.pw + s.pm;
            s.pi += 1;
            s.ppc = 0;
        }
    }
    true
}
fn step_c(s: &mut S) -> bool {
    if s.ci >= READS {
        return false;
    }
    match s.cpc {
        0 => {
            s.cr = s.r;
            s.cpc = 1;
        }
        1 => {
            let cw = s.w;
            s.cm = READ_N.min(cw - s.cr);
            s.cpc = 2;
        }
        2 => {
            if s.ck < s.cm {
                let v = s.slots[((s.cr + s.ck) % CAP) as usize];
                s.got.push(v);
                s.ck += 1;
            } else {
                s.ck = 0;
                s.cpc = 3;
            }
        }
        _ => {
            s.r = s.cr + s.cm;
            s.ci += 1;
            s.cpc = 0;
        }
    }
    true
}

fn explore(s: S, seen: &mut std::collections::HashSet<S>, finals: &mut u64) {
    if !seen.insert(s.clone()) {
        return;
    }
    let mut any = false;
    for who in 0..2 {
        let mut t = s.clone();
        let moved = if who == 0 { step_p(&mut t) } else { step_c(&mut t) };
        if moved {
            any = true;
            explore(t, seen, finals);
        }
    }
    // invariants in every reachable state: what was read is a prefix of what was written, in order, no garbage
    assert!(s.got.len() <= s.sent.len(), "read more than written");
    for (i, v) in s.got.iter().enumerate() {
        assert_eq!(*v, s.sent[i], "read {i} differs from what was written");
    }
    // the fill never exceeds the capacity
    assert!(s.w - s.r <= CAP);
    if !any {
        *finals += 1;
    }
}

#[test]
fn all_interleavings_of_the_step_model_are_correct() {
    let s =
        S { w: 0, r: 0, slots: [0; 4], pi: 0, ppc: 0, pw: 0, pm: 0, pk: 0, ci: 0, cpc: 0, cr: 0, cm: 0, ck: 0, got: vec![], sent: vec![] };
    let (mut seen, mut finals) = (std::collections::HashSet::new(), 0);
    explore(s, &mut seen, &mut finals);
    assert!(seen.len() > 1000 && finals > 0, "explored {} states", seen.len());
}
