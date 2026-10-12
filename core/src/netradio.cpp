// Radios on the network that the user adds by address: see netradio.h
#include "tcp_client.h"
#include "native_common.h"
#include "dect2/netradio.h"
#include <mutex>

namespace dect2 {

#ifdef DECT2_HAVE_SOAPY
DeviceInfo soapyDeviceFor(const std::string& args, const std::string& board, const std::string& name);   // source_soapy.cpp
#endif

namespace {
std::mutex gNetMu;
std::vector<NetRadio> gNet;

const char* typeKey(NetRadio::Type t) {
    return t == NetRadio::Pluto ? "pluto" : t == NetRadio::SoapyRemote ? "soapyremote" : t == NetRadio::SpyServer ? "spyserver" : t == NetRadio::HermesLite2 ? "hl2" : "rtltcp";
}

// the host as typed: no spaces, no "ip:" (a Pluto's libiio URI), no "tcp://" (a SoapyRemote URL)
std::string cleanHost(std::string h) {
    std::string r;
    for (char c : h) if (c != ' ' && c != '\t' && c != ',' && c != ';') r += c;
    for (const char* pre : {"ip:", "tcp://"}) if (r.rfind(pre, 0) == 0) r.erase(0, strlen(pre));
    return r;
}
std::string hostPort(const std::string& host, int port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
} // namespace

const char* netRadioTypeName(NetRadio::Type t) {
    return t == NetRadio::Pluto ? "PlutoSDR" : t == NetRadio::SoapyRemote ? "SoapyRemote" : t == NetRadio::SpyServer ? "SpyServer"
         : t == NetRadio::HermesLite2 ? "Hermes Lite 2" : "rtl_tcp";
}
int netRadioDefaultPort(NetRadio::Type t) {
    return t == NetRadio::Pluto ? 0 : t == NetRadio::SoapyRemote ? 55132 : t == NetRadio::SpyServer ? 5555 : t == NetRadio::HermesLite2 ? 1024 : 1234;
}
const char* netRadioDefaultHost(NetRadio::Type t) { return t == NetRadio::Pluto ? "192.168.2.1" : ""; }
int netRadioPort(const NetRadio& r) {
    if (r.port > 0) return r.port;
    return r.type == NetRadio::Pluto ? 30431 : netRadioDefaultPort(r.type);
}

std::string netRadiosToString(const std::vector<NetRadio>& v) {
    std::string s;
    for (const auto& r : v) {
        const std::string h = cleanHost(r.host);
        if (h.empty()) continue;
        s += (s.empty() ? "" : ";") + std::string(typeKey(r.type)) + "," + h + "," + std::to_string(r.port > 0 ? r.port : 0);
    }
    return s;
}

std::vector<NetRadio> netRadiosFromString(const std::string& s) {
    std::vector<NetRadio> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(';', pos);
        if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(pos, e - pos);
        pos = e + 1;
        const size_t c1 = item.find(','), c2 = c1 == std::string::npos ? c1 : item.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        const std::string t = item.substr(0, c1);
        NetRadio r;
        if (t == "pluto") r.type = NetRadio::Pluto;
        else if (t == "rtltcp") r.type = NetRadio::RtlTcp;
        else if (t == "soapyremote") r.type = NetRadio::SoapyRemote;
        else if (t == "spyserver") r.type = NetRadio::SpyServer;
        else if (t == "hl2") r.type = NetRadio::HermesLite2;
        else continue;
        r.host = cleanHost(item.substr(c1 + 1, c2 - c1 - 1));
        r.port = atoi(item.c_str() + c2 + 1);
        if (r.port < 0 || r.port > 65535) r.port = 0;
        if (!r.host.empty() && std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
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
std::string soapyArgs(const NetRadio& r) { return "driver=remote,remote=tcp://" + hostPort(cleanHost(r.host), netRadioPort(r)); }
} // namespace

std::string netRadioDeviceName(const NetRadio& r) {
    switch (r.type) {
    case NetRadio::Pluto: return native::plutoDevice(plutoUri(r)).name;
    case NetRadio::SoapyRemote: return "SoapyRemote " + hostPort(cleanHost(r.host), netRadioPort(r)) + " (network)";
    case NetRadio::SpyServer: return native::spyServerDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::HermesLite2: return native::hermesLite2Device(cleanHost(r.host), netRadioPort(r)).name;
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
        case NetRadio::HermesLite2: out.push_back(native::hermesLite2Device(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::SoapyRemote:
#ifdef DECT2_HAVE_SOAPY
            out.push_back(soapyDeviceFor(soapyArgs(r), "remote", netRadioDeviceName(r)));
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
    if (r.type == NetRadio::SpyServer) return native::spyServerProbe(host, netRadioPort(r), timeoutMs, err);   // HELLO, then its DEVICE_INFO
    if (r.type == NetRadio::HermesLite2) return native::hermesLite2Probe(host, netRadioPort(r), timeoutMs, err);   // UDP: a discovery to it
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
    return ok;
}

} // namespace dect2
