// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #546: Android passes a picked IQ file as an open descriptor (/proc/self/fd/N, fd:N). The file source must use that descriptor, not
// reopen the path (permission denied there), and the path text must be read right.
#include "dect2/common/ring.h"
#include "dect2/source/source.h"
#include "../../core/src/source/fd_path.h"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(parseFdPath("/proc/self/fd/87") == 87, "proc path");
    CHECK(parseFdPath("/dev/fd/3") == 3, "dev path");
    CHECK(parseFdPath("fd:12") == 12, "fd: path");
    CHECK(parseFdPath("/proc/self/fd/") == -1, "no number");
    CHECK(parseFdPath("/proc/self/fd/8x") == -1, "junk after number");
    CHECK(parseFdPath("/proc/self/fd/12345678901") == -1, "too long");
    CHECK(parseFdPath("/tmp/a.cs8") == -1, "plain path");
    CHECK(parseFdPath("") == -1, "empty");

    const char* tmp = "/tmp/test_file_fd.cs8";
    FILE* w = fopen(tmp, "wb");
    for (int i = 0; i < 40000; i++) fputc(i & 0x7f, w);
    fclose(w);
    const int fd = open(tmp, O_RDONLY);
    unlink(tmp);   // only the descriptor can reach the data now, like a provider file the app may not open by path
    CHECK(fd >= 0, "open");
    for (const char* form : {"/proc/self/fd/", "fd:"}) {
        auto src = makeFileSource(std::string(form) + std::to_string(fd), FileFormat::CS8, 1e5, false);
        IqRing ring(1u << 16);
        TuneSettings t; std::string err;
        const bool ok = src->start(t, ring, err);
        CHECK(ok, "start through %s%d failed: %s", form, fd, err.c_str());
        if (ok) { usleep(300000); src->stop(); CHECK(ring.available() > 0, "no samples read through %s", form); }
    }
    close(fd);
    printf(fails ? "file fd: FAILED\n" : "file fd: ok\n");
    return fails ? 1 : 0;
}
