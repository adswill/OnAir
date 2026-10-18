// Radios on the network that the user adds by address: see netradio.h
#include "tcp_client.h"
#include "../source/native_common.h"
#include "dect2/net/netradio.h"
#include "dect2/net/net_proto.h"
#include "dect2/net/kiwisdr.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#ifdef DECT2_HAVE_SOAPY
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#endif
#include "dect2/common/str.h"   // (also without SoapySDR)

namespace dect2 {

#ifdef DECT2_HAVE_SOAPY
DeviceInfo soapyDeviceFor(const std::string& args, const std::string& board, const std::string& name);   // source_soapy.cpp
#endif

namespace {
std::mutex gNetMu;
std::vector<NetRadio> gNet;

// the host as typed: no spaces, no "ip:" (a Pluto's libiio URI), no "tcp://" (a SoapyRemote URL)
std::string cleanHost(std::string h) { return netp::cleanHost(h); }
std::string hostPort(const std::string& host, int port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
} // namespace

// the type tables and the settings text of the list are in net_proto.h (C++ or the Rust crate onair_net)
const char* netRadioTypeName(NetRadio::Type t) {
    static const std::string names[9] = {netp::radioTypeName(0), netp::radioTypeName(1), netp::radioTypeName(2), netp::radioTypeName(3), netp::radioTypeName(4), netp::radioTypeName(5), netp::radioTypeName(6), netp::radioTypeName(7), netp::radioTypeName(8)};
    return names[(unsigned)t < 9 ? (unsigned)t : 1].c_str();
}
int netRadioDefaultPort(NetRadio::Type t) { return netp::radioDefaultPort((uint32_t)t); }
const char* netRadioDefaultHost(NetRadio::Type t) {
    static const std::string pluto = netp::radioDefaultHost(0);
    return t == NetRadio::Pluto ? pluto.c_str() : "";
}
int netRadioPort(const NetRadio& r) { return netp::radioPort((uint32_t)r.type, r.port); }

std::string netRadiosToString(const std::vector<NetRadio>& v) {
    std::vector<netp::Radio> in;
    for (const auto& r : v) { netp::Radio x; x.kind = (uint32_t)r.type; x.host = r.host; x.port = r.port; x.remote = r.remote; in.push_back(std::move(x)); }
    return netp::radiosToString(in);
}

std::vector<NetRadio> netRadiosFromString(const std::string& s) {
    std::vector<NetRadio> v;
    for (const auto& x : netp::radiosFromString(s)) {
        NetRadio r;
        r.type = (NetRadio::Type)x.kind;
        r.host = x.host; r.port = x.port; r.remote = x.remote;
        v.push_back(std::move(r));
    }
    return v;
}

void setNetworkRadios(const std::vector<NetRadio>& v) { std::lock_guard<std::mutex> lk(gNetMu); gNet = v; }
std::vector<NetRadio> networkRadios() { std::lock_guard<std::mutex> lk(gNetMu); return gNet; }

namespace {
// A Pluto's libiio URI: "ip:host", with ":port" only when one was given
std::string plutoUri(const NetRadio& r) {
    const std::string h = cleanHost(r.host);
    return "ip:" + (r.port > 0 ? hostPort(h, r.port) : h);
}
std::string trimWs(const std::string& v) { return dect2::str::trimChars(v, " \t"); }
std::string lower(std::string v) { return dect2::str::lower(std::move(v)); }
// SoapyRemote's own network timeout is 100 ms (connect, and the wait for the server to answer the log connection). A server that is slow
// to answer (a small computer, a busy one, a server that is still probing its radios) makes the client give up and drop the connection:
// the server logs "handlerLoop() FAIL: SoapyRPCUnpacker::recv(header)" and the client sees no radios (issue #84). A few seconds
// cost nothing when the server is quick.
constexpr const char* kRemoteTimeout = ",remote:timeout=5000000";
std::string remoteBase(const NetRadio& r) { return "driver=remote,remote=tcp://" + hostPort(cleanHost(r.host), netRadioPort(r)) + kRemoteTimeout; }

std::mutex gEnumMu;
SoapyEnumerateFn gEnum = nullptr;
struct Offered { std::chrono::steady_clock::time_point at; std::vector<RemoteSoapyDevice> all; };
std::map<std::string, Offered> gOffered;   // by the server's "host:port"

std::vector<std::map<std::string, std::string>> defaultEnumerate(const std::string& args) {
    std::vector<std::map<std::string, std::string>> out;
#ifdef DECT2_HAVE_SOAPY
    try { for (const auto& kw : SoapySDR::Device::enumerate(args)) { std::map<std::string, std::string> m; for (const auto& e : kw) m[e.first] = e.second; out.push_back(std::move(m)); } } catch (...) {}
#else
    (void)args;
#endif
    return out;
}
std::string soapyArgs(const NetRadio& r);
int gRemoteModule = -1;   // tests: 0 = the client module is missing, 1 = present, -1 = ask SoapySDR
} // namespace

void setSoapyRemoteModuleOverride(int v) { std::lock_guard<std::mutex> lk(gEnumMu); gRemoteModule = v; }

// SoapySDR opens "driver=remote" only when SoapyRemote's client module (remoteSupport) is installed next to the library: it
// registers a "remote" driver. Without it SoapySDR says only "no match", which tells a user nothing (issue #84).
bool soapyRemoteModuleAvailable() {
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        if (gRemoteModule >= 0) return gRemoteModule == 1;
        if (gEnum) return true;   // a replaced enumerator is a test's fake server: nothing to install
    }
#ifdef DECT2_HAVE_SOAPY
    try { return SoapySDR::Registry::listFindFunctions().count("remote") > 0; } catch (...) { return false; }
#else
    return false;
#endif
}

// What is missing and what to do about it, for the platform OnAir runs on (0 Windows, 1 macOS, 2 Linux and others; -1 = this one).
// Windows and macOS packages carry the module themselves, so there it means a damaged installation, not something to install.
const char* soapyRemoteModuleHelpFor(int platform) {
    if (platform < 0) {
#if defined(_WIN32)
        platform = 0;
#elif defined(__APPLE__)
        platform = 1;
#else
        platform = 2;
#endif
    }
    if (platform == 0)
        return "The SoapyRemote module is missing from this installation of OnAir, so it cannot reach a remote radio. Reinstall OnAir, and please report it if it happens again. "
               "The radio's computer must run SoapySDRServer --bind.";
    if (platform == 1)
        return "The SoapyRemote module is missing from this installation of OnAir, so it cannot reach a remote radio. Reinstall OnAir (or run brew install soapyremote), and please report it if it happens again. "
               "The radio's computer must run SoapySDRServer --bind.";
    return "SoapySDR here has no SoapyRemote client module, so it cannot reach a remote radio. Install it (Debian/Ubuntu: sudo apt install soapysdr-module-remote; "
           "other systems: github.com/pothosware/SoapyRemote), then restart OnAir. The radio's computer must run SoapySDRServer --bind.";
}

const char* soapyRemoteModuleHelp() { return soapyRemoteModuleHelpFor(-1); }

std::string soapyRemoteArgs(const NetRadio& r) {
    std::string a = remoteBase(r);
    size_t pos = 0;
    while (pos <= r.remote.size()) {
        size_t e = r.remote.find(',', pos);
        if (e == std::string::npos) e = r.remote.size();
        const std::string kv = trimWs(r.remote.substr(pos, e - pos));
        pos = e + 1;
        if (kv.empty()) continue;
        a += kv.find('=') == std::string::npos ? ",remote:driver=" + kv : ",remote:" + kv;
    }
    return a;
}
namespace { std::string soapyArgs(const NetRadio& r) { return soapyRemoteArgs(r); } }

std::vector<RemoteSoapyDevice> soapyRemoteOffers(const std::vector<std::map<std::string, std::string>>& kwargs, bool includeAudio) {
    std::vector<RemoteSoapyDevice> out;
    auto get = [](const std::map<std::string, std::string>& m, const char* k) {
        const auto it = m.find(std::string("remote:") + k);
        if (it != m.end()) return it->second;
        const auto i2 = m.find(k);
        return i2 == m.end() ? std::string() : i2->second;
    };
    for (const auto& m : kwargs) {
        RemoteSoapyDevice d;
        d.driver = get(m, "driver"); d.serial = get(m, "serial"); d.label = get(m, "label");
        if (d.driver.empty() || d.driver == "remote") continue;
        if (!includeAudio && lower(d.driver) == "audio") continue;
        out.push_back(d);
    }
    return out;
}

void setSoapyEnumerator(SoapyEnumerateFn fn) { std::lock_guard<std::mutex> lk(gEnumMu); gEnum = fn; gOffered.clear(); }

std::vector<RemoteSoapyDevice> soapyRemoteList(const NetRadio& r, bool includeAudio) {
    const std::string key = hostPort(cleanHost(r.host), netRadioPort(r));
    SoapyEnumerateFn fn;
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        fn = gEnum;
        const auto it = gOffered.find(key);
        if (it != gOffered.end() && std::chrono::steady_clock::now() - it->second.at < std::chrono::seconds(60)) {
            std::vector<RemoteSoapyDevice> v;
            for (const auto& d : it->second.all) if (includeAudio || lower(d.driver) != "audio") v.push_back(d);
            return v;
        }
    }
    const auto kw = (fn ? fn : defaultEnumerate)(remoteBase(r));
    Offered o;
    o.at = std::chrono::steady_clock::now();
    o.all = soapyRemoteOffers(kw, true);
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        gOffered[key] = o;
    }
    return soapyRemoteOffers(kw, includeAudio);
}

std::string soapyRemoteFailureHelp(const std::string& args) {
    const size_t p = args.find("remote=tcp://");
    if (args.find("driver=remote") == std::string::npos || p == std::string::npos) return "";
    const size_t e = args.find(',', p);
    const std::string key = args.substr(p + 13, e == std::string::npos ? std::string::npos : e - p - 13);
    std::vector<RemoteSoapyDevice> all;
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        const auto it = gOffered.find(key);
        if (it != gOffered.end()) all = it->second.all;
    }
    if (!soapyRemoteModuleAvailable()) return soapyRemoteModuleHelp();
    std::string s = "The SoapyRemote server " + key + " did not give OnAir a radio. ";
    if (all.empty()) return s + "It lists no radios: check that SoapySDRServer runs there and that the radio's SoapySDR module (for example mirisdr or sdrplay) is installed on that computer, then pick the radio, or type its driver in the Remote driver box.";
    std::string pick;
    for (const auto& d : all) if (lower(d.driver) != "audio") { pick = d.driver; break; }
    s += "It offers:";
    for (const auto& d : all) s += " " + d.driver + (d.label.empty() && d.serial.empty() ? "" : " (" + (d.label.empty() ? d.serial : d.label) + ")") + ";";
    s += " pick the radio in the list, or type its driver ";
    if (!pick.empty()) s += "(for example " + pick + ") ";
    return s + "in the Remote driver box. A sound card (audio) is not a radio.";
}

std::string netRadioHostPort(const NetRadio& r) { return hostPort(cleanHost(r.host), netRadioPort(r)); }

std::string netRadioDeviceName(const NetRadio& r) {
    switch (r.type) {
    case NetRadio::Pluto: return native::plutoDevice(plutoUri(r)).name;
    case NetRadio::SoapyRemote: return "SoapyRemote " + hostPort(cleanHost(r.host), netRadioPort(r)) + (r.remote.empty() ? "" : " " + r.remote) + " (network)";
    case NetRadio::SpyServer: return native::spyServerDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::HermesLite2: return native::hermesLite2Device(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::RedPitaya: return native::redPitayaDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::SdrppServer: return native::sdrppDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::NrspSt: return native::nrspStDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::KiwiSdr: return native::kiwiSdrDevice(cleanHost(r.host), netRadioPort(r)).name;
    default: return native::rtlTcpDevice(cleanHost(r.host), netRadioPort(r)).name;
    }
}

std::vector<DeviceInfo> netRadioDevices(const std::vector<NetRadio>& v) {
    std::vector<DeviceInfo> out;
    for (const auto& r : v) {
        if (cleanHost(r.host).empty()) continue;
        switch (r.type) {
        case NetRadio::Pluto: out.push_back(native::plutoDevice(plutoUri(r))); break;
        case NetRadio::RtlTcp: out.push_back(native::rtlTcpDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::SpyServer: out.push_back(native::spyServerDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::HermesLite2: for (auto& d : native::hermesLite2Devices(cleanHost(r.host), netRadioPort(r))) out.push_back(d); break;   // one per receiver
        case NetRadio::RedPitaya: for (auto& d : native::redPitayaDevices(cleanHost(r.host), netRadioPort(r))) out.push_back(d); break;
        case NetRadio::SdrppServer: out.push_back(native::sdrppDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::NrspSt: out.push_back(native::nrspStDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::KiwiSdr: {
            const auto d = native::kiwiSdrDevice(cleanHost(r.host), netRadioPort(r));
            kiwiSetPassword(d.nativeArgs, r.remote);   // for the source; not in the device entry
            out.push_back(d);
            break;
        }
        case NetRadio::SoapyRemote:
#ifdef DECT2_HAVE_SOAPY
        {
            const std::string hp = hostPort(cleanHost(r.host), netRadioPort(r));
            const auto offers = r.remote.empty() ? soapyRemoteList(r) : std::vector<RemoteSoapyDevice>();
            if (offers.empty()) out.push_back(soapyDeviceFor(soapyArgs(r), "remote", netRadioDeviceName(r)));
            for (const auto& o : offers) {
                NetRadio one = r;
                one.remote = "driver=" + o.driver + (o.serial.empty() ? "" : ",serial=" + o.serial);
                const std::string det = o.label.empty() ? o.serial : o.label;
                out.push_back(soapyDeviceFor(soapyArgs(one), "remote", "SoapyRemote " + hp + ": " + o.driver + (det.empty() ? "" : " (" + det + ")") + " (network)"));
            }
        }
#endif
            break;
        }
    }
    return out;
}

bool probeNetRadio(const NetRadio& r, int timeoutMs, std::string& err) {
    const std::string host = cleanHost(r.host);
    if (host.empty()) { err = "type the radio's address"; return false; }
#ifndef DECT2_HAVE_SOAPY
    if (r.type == NetRadio::SoapyRemote) { err = "this build of OnAir has no SoapySDR, which SoapyRemote needs"; return false; }
#endif
    if (r.type == NetRadio::SoapyRemote && !soapyRemoteModuleAvailable()) { err = soapyRemoteModuleHelp(); return false; }
    if (r.type == NetRadio::SpyServer) return native::spyServerProbe(host, netRadioPort(r), timeoutMs, err);   // HELLO, then its DEVICE_INFO
    if (r.type == NetRadio::HermesLite2) return native::hermesLite2Probe(host, netRadioPort(r), timeoutMs, err);   // UDP: a discovery to it
    if (r.type == NetRadio::RedPitaya) return native::redPitayaProbe(host, netRadioPort(r), timeoutMs, err);
    if (r.type == NetRadio::KiwiSdr) { kiwiSetPassword(hostPort(host, netRadioPort(r)), r.remote); return native::kiwiProbe(host, netRadioPort(r), timeoutMs, err); }   // upgrade, auth, sample_rate; then the slot is given back
    if (r.type == NetRadio::NrspSt) return native::nrspStProbe(host, netRadioPort(r), timeoutMs, err);   // WebSocket upgrade, then api_version
    if (r.type == NetRadio::SdrppServer) return native::sdrppProbe(host, netRadioPort(r), timeoutMs, err);   // GET_UI, then its menu
    const sock_t s = tcpConnect(host, netRadioPort(r), timeoutMs, err);
    if (s == kBadSock) return false;
    bool ok = true;
    if (r.type == NetRadio::RtlTcp) {   // an rtl_tcp server greets every client with "RTL0" (or its own magic) and its tuner
        sockRecvTimeout(s, timeoutMs);
        unsigned char hdr[12];
        if (!sockRecvAll(s, hdr, sizeof hdr)) { err = "the server at " + hostPort(host, netRadioPort(r)) + " sent no rtl_tcp greeting (another client may be connected)"; ok = false; }
        else {
            std::string magic;
            bool other = false;
            if (!native::rtlTcpGreeting(hdr, magic, other)) { err = hostPort(host, netRadioPort(r)) + " is not an rtl_tcp server"; ok = false; }
            else native::rtlTcpNoteServer(native::rtlTcpDevice(host, netRadioPort(r)).nativeArgs, other);
        }
    }
    sockShutdown(s);
    sockClose(s);
#ifdef DECT2_HAVE_SOAPY
    if (ok && r.type == NetRadio::SoapyRemote && r.remote.empty() && soapyRemoteList(r).empty()) {   // reachable, but it offers no radio (a sound card is not one)
        err = soapyRemoteFailureHelp(soapyArgs(r));
        ok = false;
    }
#endif
    return ok;
}

} // namespace dect2
