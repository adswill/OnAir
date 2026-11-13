// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir engine threading core: the lock-free single-producer / single-consumer ring of complex float samples (`IqRing`,
//! core/include/dect2/common/ring.h) behind a C ABI. The C++ side owns the storage (a `std::vector<cf32>`) and a header block; this crate
//! holds only the algorithm, so it needs no allocator and no `std`. The bounded queue / re-order buffer / pool of the C++ threading
//! framework are generic over the element type (`BoundedQueue<T>`, `StagePipeline<Item>`) and mutex based; they stay in C++ (see README).
//! Safe code goes in `ring`; only `ffi` and the raw copies in `ring` use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]

pub mod ffi;
pub mod ring;
