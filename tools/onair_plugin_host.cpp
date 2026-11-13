// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// onair-plugin-host: the helper process of out-of-process plugins. Started by the app as: onair-plugin-host --plugin-host <library>
// (pipe from the app on fd 0, results to the app on fd 3). Not meant to be run by hand. See docs/plugins.md.
#include "dect2/plugin/host.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 3 || strcmp(argv[1], "--plugin-host") != 0) {
        fprintf(stderr, "onair-plugin-host is started by OnAir for out-of-process plugins.\n");
        return 64;
    }
    return dect2::plugin::helperMain(argv[2], 0, 3);
}
