// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "loader.h"
#include <cstring>
#include <map>
#include <mutex>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2::plugin {

namespace {
std::mutex gMu;
std::map<std::string, Lib> gLibs;
bool inVt(const OnAirPluginVTable* v, size_t off, size_t sz) { return v->struct_size >= off + sz; }
}

bool loadLibrary(const std::string& path, const Manifest& m, Lib& out, std::string& err) {
    std::lock_guard<std::mutex> lk(gMu);
    Lib lib;
    auto it = gLibs.find(path);
    if (it != gLibs.end()) lib = it->second;
    else {
#if defined(_WIN32)
        void* h = (void*)LoadLibraryA(path.c_str());
        if (!h) { err = "cannot load the library (error " + std::to_string(GetLastError()) + ")"; return false; }
        auto fn = (onair_plugin_entry_fn)GetProcAddress((HMODULE)h, "onair_plugin_entry");
#else
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { const char* e = dlerror(); err = std::string("cannot load the library: ") + (e ? e : "unknown error"); return false; }
        auto fn = (onair_plugin_entry_fn)dlsym(h, "onair_plugin_entry");
#endif
        if (!fn) { err = "the library has no onair_plugin_entry (not an OnAir plugin)"; return false; }
        lib.handle = h;
        lib.vt = fn(ONAIR_PLUGIN_ABI_VERSION);
        gLibs[path] = lib;
    }
    const OnAirPluginVTable* v = lib.vt;
    if (!v) { err = "the plugin refused this OnAir (entry returned nothing)"; return false; }
    if (v->struct_size < offsetof(OnAirPluginVTable, process_iq)) { err = "plugin table is too small"; return false; }
    if (v->abi_version != ONAIR_PLUGIN_ABI_VERSION) { err = "the library was built for plugin ABI " + std::to_string(v->abi_version) + ", this OnAir speaks " + std::to_string(ONAIR_PLUGIN_ABI_VERSION); return false; }
    if (!v->id || m.id != v->id) { err = "the library's id does not match manifest.json"; return false; }
    if (!v->create || !v->destroy) { err = "the plugin has no create / destroy"; return false; }
    if (m.kind == "action" && !((v->caps & ONAIR_CAP_EVENTS) && inVt(v, offsetof(OnAirPluginVTable, on_event), sizeof(void*)) && v->on_event)) { err = "an action plugin must implement on_event"; return false; }
    if ((m.kind == "mode" || m.kind == "source") && !((v->caps & ONAIR_CAP_IQ_SINK) && v->process_iq)) { err = "a mode plugin must implement process_iq"; return false; }
    out = lib;
    return true;
}

bool probeLibrary(const std::string& path, const Manifest& m, std::string& err) {
    Lib l;
    return loadLibrary(path, m, l, err);
}

namespace {
ApiSink* S(void* c) { return (ApiSink*)c; }
std::string str(const char* s, size_t maxLen = 4096) { if (!s) return {}; size_t n = 0; while (n < maxLen && s[n]) n++; return std::string(s, n); }
constexpr int kDeniedCode = -3;

void cLog(void* c, int32_t lv, const char* m) { if (S(c)->alive()) S(c)->log(lv, str(m, 2000)); }
int32_t cGetText(void* c, const char* key, char* out, uint32_t cap) {
    bool f = false;
    const std::string v = S(c)->settingText(str(key, 64), f);
    if (!f) return -1;
    if (out && cap) { const size_t n = std::min<size_t>(v.size(), cap - 1); memcpy(out, v.data(), n); out[n] = 0; }
    return (int32_t)v.size();
}
double cGetNum(void* c, const char* key, double def) {
    bool f = false;
    const std::string v = S(c)->settingText(str(key, 64), f);
    if (!f || v.empty()) return def;
    char* e = nullptr;
    const double d = strtod(v.c_str(), &e);
    return e && e != v.c_str() ? d : def;
}
int32_t cGetFlag(void* c, const char* key, int32_t def) {
    bool f = false;
    const std::string v = S(c)->settingText(str(key, 64), f);
    if (!f || v.empty()) return def;
    return v == "1" || v == "true" ? 1 : 0;
}
int32_t cReq(void* c, double hz, double sr) { return S(c)->alive() ? S(c)->requestIq(hz, sr) : kDeniedCode; }
int32_t cTableDefine(void* c, const char* id, const char* title, uint32_t n, const OnAirColumn* cols) {
    if (!S(c)->alive() || !cols || n == 0 || n > kMaxCols) return -1;
    std::vector<Col> v;
    for (uint32_t i = 0; i < n; i++) v.push_back({str(cols[i].name, 40), cols[i].type});
    return S(c)->tableDefine(str(id, kMaxIdLen + 1), str(title, kMaxTitleLen + 1), v);
}
int32_t cTableRow(void* c, const char* id, uint32_t n, const OnAirCell* cells) {
    if (!S(c)->alive() || !cells || n == 0 || n > kMaxCols) return -1;
    std::vector<Cell> v;
    for (uint32_t i = 0; i < n; i++) { Cell x; x.type = cells[i].type; x.i = cells[i].i; x.d = cells[i].d; x.s = str(cells[i].s, kMaxCellText + 1); v.push_back(std::move(x)); }
    return S(c)->tableRow(str(id, kMaxIdLen + 1), v);
}
int32_t cTableClear(void* c, const char* id) { return S(c)->alive() ? S(c)->tableClear(str(id, kMaxIdLen + 1)) : kDeniedCode; }
int32_t cStatus(void* c, const char* k, const char* t, int32_t lamp) { return S(c)->alive() ? S(c)->status(str(k, kMaxIdLen + 1), str(t, kMaxCellText + 1), lamp) : kDeniedCode; }
int32_t cResultLog(void* c, const char* l) { return S(c)->alive() ? S(c)->resultLog(str(l, kMaxLogLine + 1)) : kDeniedCode; }
int32_t cPlot(void* c, const char* id, const char* t, int32_t kind, const float* d, uint32_t n, double x0, double dx) {
    return S(c)->alive() ? S(c)->plot(str(id, kMaxIdLen + 1), str(t, kMaxTitleLen + 1), kind, d, n, x0, dx) : kDeniedCode;
}
int32_t cImage(void* c, const char* id, int32_t fmt, uint32_t w, uint32_t h, const uint8_t* d, uint32_t n) {
    return S(c)->alive() ? S(c)->image(str(id, kMaxIdLen + 1), fmt, w, h, d, n) : kDeniedCode;
}
int32_t cMap(void* c, const char* id, const char* l, double la, double lo) { return S(c)->alive() ? S(c)->mapPoint(str(id, kMaxIdLen + 1), str(l, 100), la, lo) : kDeniedCode; }
int32_t cEvent(void* c, const char* type, uint32_t n, const OnAirField* f) {
    if (!S(c)->alive() || n > 32 || (n && !f)) return -1;
    automation::Event ev;
    for (uint32_t i = 0; i < n; i++) {
        const std::string k = str(f[i].key, 40);
        if (k.empty()) continue;
        if (f[i].is_text) ev.str(k, str(f[i].s, automation::kMaxFieldText + 1)); else ev.num(k, f[i].d);
    }
    return S(c)->emitEvent(str(type, 40), ev);
}
int32_t cAudio(void* c, const float* pcm, uint32_t n, double rate) {
    if (!S(c)->alive()) return kDeniedCode;
    if (!(S(c)->granted() & PermAudioOut)) return kDeniedCode;
    if (!pcm || n == 0 || n > (1u << 20) || !(rate >= 1000 && rate <= 400000)) return -1;
    return S(c)->audioOut(pcm, n, rate);
}
int32_t cDataDir(void* c, char* out, uint32_t cap) {
    if (!(S(c)->granted() & PermFilesystem)) return kDeniedCode;
    const std::string d = S(c)->dataDir();
    if (d.empty() || !out || d.size() + 1 > cap) return -1;
    memcpy(out, d.c_str(), d.size() + 1);
    return 0;
}
} // namespace

void fillHostApi(OnAirHostApi& a, ApiSink* sink) {
    memset(&a, 0, sizeof a);
    a.struct_size = sizeof a;
    a.abi_version = ONAIR_PLUGIN_ABI_VERSION;
    a.ctx = sink;
    a.log = cLog; a.get_setting_text = cGetText; a.get_setting_number = cGetNum; a.get_setting_flag = cGetFlag; a.request_iq = cReq;
    a.table_define = cTableDefine; a.table_row = cTableRow; a.table_clear = cTableClear; a.status = cStatus; a.result_log = cResultLog;
    a.plot = cPlot; a.image = cImage; a.map_point = cMap; a.emit_event = cEvent; a.audio_out = cAudio; a.data_dir = cDataDir;
}

} // namespace dect2::plugin
