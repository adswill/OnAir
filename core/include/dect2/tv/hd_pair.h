// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD / SD simulcast pairing: finds the HD (or UHD) service that carries the same programme as an SD service, in the same multiplex or in another
// multiplex of the scan. Evidence, strongest first: a user override, the DVB linkage descriptor (type 0x05 service replacement) of the SDT, the same
// programme name once the HD/UHD/+1/region decorations are removed, and the logical channel number rule (HD on LCN + 100). The picture size classifies
// a service as SD, HD or UHD (the size of the video, else the service type of the SDT, the name and the PMT codec). Pure logic, no I/O.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace hd {

enum Tier { kUnknown = 0, kSD = 1, kHD = 2, kUHD = 3 };
const char* tierName(Tier t);   // "?", "SD", "HD", "UHD"

// Size of the picture: height >= 2160 or width >= 3840 is UHD, height >= 720 or width >= 1280 is HD, anything else known is SD (576i, 480i, 540p)
Tier tierFromResolution(int width, int height);
// SDT service_type (EN 300 468 table 87): 0x11 and 0x19 HD, 0x1F HEVC (HD or better), 0x01 and 0x16 SD, others unknown
Tier tierFromServiceType(int serviceType);
// PMT stream_type of the video: 0x02 MPEG-2 video is SD in practice, 0x24 HEVC is HD or better, 0x1B H.264 can be either
Tier tierFromStreamType(int streamType);

// Picture size from the first sequence parameter set: `nal` starts at the NAL unit header (H.264: one byte, nal_unit_type 7; HEVC: two bytes,
// nal_unit_type 33). Emulation prevention bytes are removed here. False when the data is too short or not an SPS.
bool spsResolution(bool hevc, const uint8_t* nal, size_t n, int& width, int& height);

struct NameInfo {
    std::string key;        // lower case, no spaces or punctuation, no HD/UHD marker, no region, no timeshift suffix: "BBC ONE South" and "BBC ONE HD" both give "bbcone"
    Tier marker = kUnknown; // kHD / kUHD when the name carries "HD", "HD+", "FHD", "UHD", "4K" as a word or suffix
    int timeshift = 0;      // minutes of a "+1", "+2", "+25" style suffix (a different programme time), 0 when none
};
NameInfo normalizeName(const std::string& name);

// HD is often on LCN + 100 (UK: BBC ONE 1 and BBC ONE HD 101), sometimes + 1000
bool lcnPairs(int sdLcn, int hdLcn);

struct Service {
    double freqMhz = 0;
    int onid = 0, tsid = 0, sid = 0;
    std::string name, provider;
    int lcn = 0;
    int serviceType = 0;                 // SDT
    int streamType = 0;                  // PMT video stream_type, 0 unknown
    std::string videoCodec;              // "H.264", "HEVC", "MPEG-2"
    int width = 0, height = 0;           // the video size once it has been seen, else 0
    int linkOnid = 0, linkTsid = 0, linkSid = 0;   // SDT linkage descriptor type 0x05: the service this one is replaced by
    Tier tier() const;
};

// User-editable pairing table: the programme `sd` (a name, compared like normalizeName) is paired with the service named `hd`; hd = "-" says "never pair".
struct Override { std::string sd, hd; };

struct Pairing {
    int index = -1;          // into the service list, -1 none
    int score = 0;
    std::string reason;      // "pairing table", "linkage", "same name", "same name and LCN + 100", "LCN + 100 in the same multiplex"
};
// The HD / UHD counterpart of services[i] (a service of a better tier carrying the same programme), preferring the same multiplex
Pairing findHd(const std::vector<Service>& services, size_t i, const std::vector<Override>& table = {});
// The SD counterpart of an HD service: the service it is the HD counterpart of
Pairing findSd(const std::vector<Service>& services, size_t i, const std::vector<Override>& table = {});

// The services found in the multiplexes of the scan, remembered between runs
class Catalog {
public:
    // replace every service of the multiplex at freqMhz by `now` (an empty list keeps what is known)
    void update(double freqMhz, const std::vector<Service>& now);
    // the picture size seen for one service
    void setResolution(double freqMhz, int sid, int width, int height);
    const std::vector<Service>& all() const { return svcs_; }
    int find(double freqMhz, int sid) const;     // index or -1
    std::string encode() const;                  // one line per service, tab separated
    void decode(const std::string& s);
    void clear() { svcs_.clear(); }
private:
    std::vector<Service> svcs_;
};

std::string encodeOverrides(const std::vector<Override>& v);   // "sd\thd\n" lines
std::vector<Override> decodeOverrides(const std::string& s);

} // namespace hd
} // namespace dect2
