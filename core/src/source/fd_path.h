// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
#pragma once
#include <string>

namespace dect2 {

// Android hands a picked IQ file over as an open descriptor, written as /proc/self/fd/N (or fd:N). Opening that path again is a new
// permission check that SELinux refuses for many providers (#546: "Permission denied"), so the file source uses the descriptor itself.
// Returns N, or -1 when the text is not such a path.
inline int parseFdPath(const std::string& p) {
    static const char* const prefixes[] = {"/proc/self/fd/", "/dev/fd/", "fd:"};
    for (const char* pre : prefixes) {
        const std::string s(pre);
        if (p.compare(0, s.size(), s) != 0) continue;
        const std::string d = p.substr(s.size());
        if (d.empty() || d.size() > 9) return -1;
        int n = 0;
        for (char c : d) { if (c < '0' || c > '9') return -1; n = n * 10 + (c - '0'); }
        return n;
    }
    return -1;
}

}  // namespace dect2
