// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Small fixed-size FIFO ring of `Copy` values, no allocation. Not thread safe (the engine crate owns the concurrent one).

/// FIFO with capacity `N`. `push` on a full ring drops the oldest element and returns it.
pub struct Ring<T: Copy, const N: usize> {
    buf: [T; N],
    head: usize,
    len: usize,
}

impl<T: Copy, const N: usize> Ring<T, N> {
    /// `fill` only initialises the storage; it is never returned.
    pub fn new(fill: T) -> Self {
        Ring { buf: [fill; N], head: 0, len: 0 }
    }
    pub fn len(&self) -> usize {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    pub fn is_full(&self) -> bool {
        self.len == N
    }
    pub fn capacity(&self) -> usize {
        N
    }
    pub fn clear(&mut self) {
        self.head = 0;
        self.len = 0;
    }
    /// Appends `v`; when full, the oldest element is overwritten and returned.
    pub fn push(&mut self, v: T) -> Option<T> {
        if N == 0 {
            return Some(v);
        }
        let dropped = if self.len == N { self.pop() } else { None };
        let tail = (self.head + self.len) % N;
        self.buf[tail] = v;
        self.len += 1;
        dropped
    }
    /// Removes and returns the oldest element.
    pub fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        let v = self.buf[self.head];
        self.head = (self.head + 1) % N;
        self.len -= 1;
        Some(v)
    }
    /// The element `i` places after the oldest.
    pub fn get(&self, i: usize) -> Option<T> {
        if i < self.len {
            Some(self.buf[(self.head + i) % N])
        } else {
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fifo_and_overwrite() {
        let mut r: Ring<u8, 3> = Ring::new(0);
        assert!(r.is_empty());
        assert_eq!(r.pop(), None);
        for v in 1..=3 {
            assert_eq!(r.push(v), None);
        }
        assert!(r.is_full());
        assert_eq!(r.push(4), Some(1));
        assert_eq!((r.get(0), r.get(2), r.get(3)), (Some(2), Some(4), None));
        assert_eq!((r.pop(), r.pop(), r.pop(), r.pop()), (Some(2), Some(3), Some(4), None));
        let mut z: Ring<u8, 0> = Ring::new(0);
        assert_eq!(z.push(9), Some(9));
    }
}
