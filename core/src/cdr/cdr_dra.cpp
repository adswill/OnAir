// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Finding and calling the user's DRA+ add-on (cdr_dra.h). The same search as the mbelib and HDC loaders (source/native_common.h).
#include "dect2/cdr/cdr_dra.h"
#include "../source/native_common.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

#define DECT2_DRA_STEM "onair_dra"
#define DECT2_DRA_SO {}
#define DECT2_DRA_WIN {"onair_dra.dll", "libonair_dra.dll"}

namespace dect2 { namespace cdr {

namespace {

struct Lib {
    native::DynLib lib;
    bool ok = false;
    std::string problem;
    DraApi api;
};

std::vector<std::string> candidates() { return native::codecLibNames(DECT2_DRA_STEM, DECT2_DRA_SO, DECT2_DRA_WIN); }

const Lib* scan() {
    Lib* r = new Lib;   // kept for the life of the program: decoders hold the functions they started with
    std::vector<std::string> names;
    if (const char* e = getenv("DECT2_DRA_PATH")) if (*e) names.push_back(e);
    if (names.empty()) names = candidates();
    if (!r->lib.open(names)) return r;     // not found: problem stays empty
    native::DynLib& l = r->lib;
    if (!l.opt(r->api.create, "DRADecCreate") || !l.opt(r->api.destroy, "DRADecDestroy") || !l.opt(r->api.info, "DRADecGetFrameInfo") ||
        !l.opt(r->api.send, "DRADecSendData") || !l.opt(r->api.recv, "DRADecRecvFrame")) {
        r->problem = "this library is not a DRA+ add-on (DRADecCreate / DRADecDestroy / DRADecGetFrameInfo / DRADecSendData / DRADecRecvFrame are missing)";
        r->api = DraApi();
        return r;
    }
    void* h = r->api.create();     // the canary: a decoder state must be made
    if (!h) { r->problem = "DRADecCreate gave no decoder"; r->api = DraApi(); return r; }
    r->api.destroy(h);
    r->ok = true;
    return r;
}

std::mutex gMu;
std::atomic<const Lib*> gLib{nullptr};

const Lib& lib() {
    if (const Lib* a = gLib.load(std::memory_order_acquire)) return *a;
    std::lock_guard<std::mutex> lk(gMu);
    if (!gLib.load()) gLib.store(scan(), std::memory_order_release);
    return *gLib.load();
}
} // namespace

bool draAvailable(std::string* path, std::string* problem) {
    const Lib& a = lib();
    if (path && a.lib.h) *path = a.lib.path;
    if (problem) *problem = a.problem;
    return a.ok;
}
const DraApi* draApi() { const Lib& a = lib(); return a.ok ? &a.api : nullptr; }

bool draRescan() {
    if (lib().ok) return true;
    std::lock_guard<std::mutex> lk(gMu);
    gLib.store(scan(), std::memory_order_release);   // the old one stays allocated: a decoder may still hold it
    return gLib.load()->ok;
}

std::vector<std::string> draFileNames() { return native::codecFileNames(DECT2_DRA_STEM, DECT2_DRA_SO, DECT2_DRA_WIN); }
std::vector<std::string> draCandidatePaths() { return candidates(); }

}} // namespace dect2::cdr
