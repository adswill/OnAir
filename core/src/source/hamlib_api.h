// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The part of the Hamlib API that source_hamlib.cpp uses, either from <hamlib/rig.h> (the library linked at build time, Linux and macOS
// packages) or, with DECT2_HAMLIB_DYNAMIC (the Windows packages, #495), loaded at run time from libhamlib-4.dll / libhamlib.so.4 /
// libhamlib.4.dylib the way the radio drivers load theirs (native_common.h: the program folder, the OnAir codecs folders, then the system).
//
// The dynamic declarations follow Hamlib 4.x's rig.h (checked against 4.7.2; the same since 4.0): only the leading fields of struct rig_caps
// are read, and those have kept their order and types since Hamlib 1.x. rig_set_freq / rig_get_freq are called with a fourth argument (the
// caller's name), which a Hamlib built with BUILTINFUNC expects and every other build ignores (cdecl: the caller removes the arguments).
#pragma once
#include <string>

#ifndef DECT2_HAMLIB_DYNAMIC
#include <hamlib/rig.h>
namespace dect2 {
inline bool hamlibLibLoad(std::string&) { return true; }   // linked: always there
inline std::string hamlibLibPath() { return "linked at build time"; }
}
#else
#include <cstdint>

namespace dect2 {
namespace hamdyn {
typedef uint32_t rig_model_t;
typedef double freq_t;
typedef uint64_t rmode_t;
typedef long pbwidth_t;      // shortfreq_t: signed long
typedef unsigned int vfo_t;
typedef long token_t;        // hamlib_token_t
typedef void* rig_ptr_t;
enum rig_errcode_e { RIG_OK = 0, RIG_EINVAL, RIG_ECONF, RIG_ENOMEM, RIG_ENIMPL };
enum rig_debug_level_e { RIG_DEBUG_NONE = 0, RIG_DEBUG_BUG, RIG_DEBUG_ERR };
enum rig_port_e { RIG_PORT_NONE = 0, RIG_PORT_SERIAL, RIG_PORT_NETWORK, RIG_PORT_DEVICE, RIG_PORT_PACKET, RIG_PORT_DTMF, RIG_PORT_ULTRA, RIG_PORT_RPC,
                  RIG_PORT_PARALLEL, RIG_PORT_USB, RIG_PORT_UDP_NETWORK };
enum ptt_t { RIG_PTT_OFF = 0, RIG_PTT_ON };
enum split_t { RIG_SPLIT_OFF = 0, RIG_SPLIT_ON };
constexpr rmode_t RIG_MODE_NONE = 0;
constexpr vfo_t RIG_VFO_CURR = 1u << 29;
constexpr vfo_t RIG_VFO_TX = RIG_VFO_CURR | (1u << 30);   // RIG_VFO_TX_VFO(RIG_VFO_CURR)
constexpr pbwidth_t RIG_PASSBAND_NORMAL = 0;
struct RIG;   // opaque: only passed back to the library
struct rig_caps {   // the leading fields of Hamlib's struct rig_caps (the rest is never touched, so its size does not matter)
    rig_model_t rig_model;
    const char* model_name;
    const char* mfg_name;
    const char* version;
    const char* copyright;
    int status;
    int rig_type;
    int ptt_type;
    int dcd_type;
    int port_type;
    int serial_rate_min;
    int serial_rate_max;
    int serial_data_bits;
    int serial_stop_bits;
};

struct Api {
    RIG* (*init)(rig_model_t) = nullptr;
    int (*open)(RIG*) = nullptr;
    int (*close)(RIG*) = nullptr;
    int (*cleanup)(RIG*) = nullptr;
    int (*setFreq)(RIG*, vfo_t, freq_t, const char*) = nullptr;
    int (*getFreq)(RIG*, vfo_t, freq_t*, const char*) = nullptr;
    int (*setMode)(RIG*, vfo_t, rmode_t, pbwidth_t) = nullptr;
    int (*getMode)(RIG*, vfo_t, rmode_t*, pbwidth_t*) = nullptr;
    int (*setPtt)(RIG*, vfo_t, ptt_t) = nullptr;
    int (*setSplitVfo)(RIG*, vfo_t, split_t, vfo_t) = nullptr;
    int (*setSplitFreq)(RIG*, vfo_t, freq_t) = nullptr;
    int (*getSplitFreq)(RIG*, vfo_t, freq_t*) = nullptr;
    const char* (*getInfo)(RIG*) = nullptr;
    rmode_t (*parseMode)(const char*) = nullptr;
    const char* (*strrmode)(rmode_t) = nullptr;
    int (*loadAllBackends)() = nullptr;
    void (*setDebug)(rig_debug_level_e) = nullptr;
    const char* (*error)(int) = nullptr;
    const rig_caps* (*getCaps)(rig_model_t) = nullptr;
    token_t (*tokenLookup)(RIG*, const char*) = nullptr;
    int (*setConf)(RIG*, token_t, const char*) = nullptr;
    int (*listForeach)(int (*)(const rig_caps*, rig_ptr_t), rig_ptr_t) = nullptr;
};
Api& api();   // filled by hamlibLibLoad (source_hamlib.cpp); every call below is made only after it returned true

inline RIG* rig_init(rig_model_t m) { return api().init(m); }
inline int rig_open(RIG* r) { return api().open(r); }
inline int rig_close(RIG* r) { return api().close(r); }
inline int rig_cleanup(RIG* r) { return api().cleanup(r); }
inline int rig_set_freq(RIG* r, vfo_t v, freq_t f) { return api().setFreq(r, v, f, "onair"); }
inline int rig_get_freq(RIG* r, vfo_t v, freq_t* f) { return api().getFreq(r, v, f, "onair"); }
inline int rig_set_mode(RIG* r, vfo_t v, rmode_t m, pbwidth_t w) { return api().setMode(r, v, m, w); }
inline int rig_get_mode(RIG* r, vfo_t v, rmode_t* m, pbwidth_t* w) { return api().getMode(r, v, m, w); }
inline int rig_set_ptt(RIG* r, vfo_t v, ptt_t p) { return api().setPtt(r, v, p); }
inline int rig_set_split_vfo(RIG* r, vfo_t v, split_t s, vfo_t tx) { return api().setSplitVfo(r, v, s, tx); }
inline int rig_set_split_freq(RIG* r, vfo_t v, freq_t f) { return api().setSplitFreq(r, v, f); }
inline int rig_get_split_freq(RIG* r, vfo_t v, freq_t* f) { return api().getSplitFreq(r, v, f); }
inline const char* rig_get_info(RIG* r) { return api().getInfo(r); }
inline rmode_t rig_parse_mode(const char* s) { return api().parseMode(s); }
inline const char* rig_strrmode(rmode_t m) { return api().strrmode(m); }
inline int rig_load_all_backends() { return api().loadAllBackends(); }
inline void rig_set_debug(rig_debug_level_e l) { api().setDebug(l); }
inline const char* rigerror(int e) { const char* s = api().error ? api().error(e) : nullptr; return s ? s : "error"; }
inline const rig_caps* rig_get_caps(rig_model_t m) { return api().getCaps(m); }
inline token_t rig_token_lookup(RIG* r, const char* n) { return api().tokenLookup(r, n); }
inline int rig_set_conf(RIG* r, token_t t, const char* v) { return api().setConf(r, t, v); }
inline int rig_list_foreach(int (*f)(const rig_caps*, rig_ptr_t), rig_ptr_t d) { return api().listForeach(f, d); }
}   // namespace hamdyn
using namespace hamdyn;

// Loads the library once (a failure is tried again at most every 5 s, so a DLL added while OnAir runs is found). False: err says what was
// looked for and where to put it.
bool hamlibLibLoad(std::string& err);
std::string hamlibLibPath();   // the file that was loaded ("" before)
}   // namespace dect2
#endif
