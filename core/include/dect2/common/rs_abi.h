// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Start-up check shared by the Rust-backed layers (core/src/<mode>/*_rs.cpp): the records of a C header in core/include/dect2/rs/ must have the same size in C++ and in
// the Rust crate (the crate reports its sizes with onair_<module>_struct_sizes), otherwise reading them would be silent memory corruption. A mismatch stops the program.
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace dect2 {

// `rust` are the sizes the crate reports, `cpp` the sizeof() of the same records in the same order.
inline void rsAbiCheck(const char* crate, const size_t* rust, std::initializer_list<size_t> cpp) {
    size_t i = 0;
    for (size_t want : cpp) {
        if (rust[i] != want) {
            fprintf(stderr, "%s: struct layout mismatch (record %zu) between C++ and Rust\n", crate, i);
            abort();
        }
        i++;
    }
}

// The same for crates that report their sizes one by one.
inline void rsAbiCheck(const char* crate, std::initializer_list<size_t> rust, std::initializer_list<size_t> cpp) {
    rsAbiCheck(crate, rust.begin(), cpp);
}

} // namespace dect2
