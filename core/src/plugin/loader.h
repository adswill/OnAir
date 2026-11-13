// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Internal: loading a plugin library and the C host API trampolines shared by the in-process runner and the helper process.
#pragma once
#include "dect2/plugin/host.h"
#include "onair_plugin.h"

namespace dect2::plugin {

struct Lib { void* handle = nullptr; const OnAirPluginVTable* vt = nullptr; };
// Loads (once; libraries are never unloaded, an unload can run code of a plugin that has gone wrong) and checks the vtable against the manifest.
bool loadLibrary(const std::string& path, const Manifest& m, Lib& out, std::string& err);

// The pieces a host API implementation needs; both runners fill one in.
struct ApiSink {
    virtual ~ApiSink() = default;
    virtual void log(int level, const std::string& msg) = 0;
    virtual std::string settingText(const std::string& key, bool& found) = 0;
    virtual int requestIq(double centreHz, double sampleRate) = 0;
    virtual int tableDefine(const std::string& id, const std::string& title, const std::vector<Col>& cols) = 0;
    virtual int tableRow(const std::string& id, const std::vector<Cell>& cells) = 0;
    virtual int tableClear(const std::string& id) = 0;
    virtual int status(const std::string& key, const std::string& text, int lamp) = 0;
    virtual int resultLog(const std::string& line) = 0;
    virtual int plot(const std::string& id, const std::string& title, int kind, const float* d, size_t n, double x0, double dx) = 0;
    virtual int image(const std::string& id, int format, uint32_t w, uint32_t h, const uint8_t* d, size_t n) = 0;
    virtual int mapPoint(const std::string& id, const std::string& label, double lat, double lon) = 0;
    virtual int emitEvent(const std::string& type, const automation::Event& ev) = 0;
    virtual int audioOut(const float* pcm, size_t n, double rate) = 0;
    virtual std::string dataDir() = 0;
    virtual uint32_t granted() = 0;
    virtual bool alive() = 0;
};
// Fills the C table; ctx must be the ApiSink*.
void fillHostApi(OnAirHostApi& api, ApiSink* sink);

} // namespace dect2::plugin
