#pragma once
// The mode-list entries of 4.0 that showed one type of an existing receiver (a sensor brand, a radiosonde type, an AIS station kind, a time station,
// an analog TV standard, a GNSS system, an NFC protocol, a wireless M-Bus mode or maker, a satellite framing) are gone: the receiver's own screen
// filters by type instead. A setting, preset, memory, scan list or workspace saved with one of them opens the receiver it showed.
#include <string>

namespace dect2 {

struct RetiredMode { int family; const char* id; int parentFamily; const char* parentId; };

inline const RetiredMode* retiredModes(int& n) {
    static const RetiredMode k[] = {
    {564, "oregon", 241, "sensors"},
    {567, "acurite", 241, "sensors"},
    {570, "lacrosse", 241, "sensors"},
    {573, "fineoffset", 241, "sensors"},
    {576, "bresser", 241, "sensors"},
    {579, "hideki", 241, "sensors"},
    {582, "tfa", 241, "sensors"},
    {585, "nexus", 241, "sensors"},
    {588, "alecto", 241, "sensors"},
    {591, "ambient", 241, "sensors"},
    {594, "alarmsens", 241, "sensors"},
    {663, "x10rf", 241, "sensors"},
    {666, "ert", 241, "sensors"},
    {669, "r900", 241, "sensors"},
    {672, "tpmsschrader", 241, "sensors"},
    {675, "tpmsrenault", 241, "sensors"},
    {678, "tpmstoyota", 241, "sensors"},
    {681, "tpmscitroen", 241, "sensors"},
    {412, "weatherall", 241, "sensors"},
    {415, "tpmsall", 241, "sensors"},
    {418, "metersall", 241, "sensors"},
    {421, "remotesall", 241, "sensors"},
    {684, "srs41", 13, "sonde"},
    {687, "srs92", 13, "sonde"},
    {690, "sdfm", 13, "sonde"},
    {693, "sm10", 13, "sonde"},
    {702, "sm20", 13, "sonde"},
    {705, "slms6", 13, "sonde"},
    {708, "simet", 13, "sonde"},
    {711, "simet54", 13, "sonde"},
    {714, "smrz", 13, "sonde"},
    {717, "smeisei", 13, "sonde"},
    {720, "wmbuss1", 86, "wmbus"},
    {723, "wmbust1", 86, "wmbus"},
    {726, "wmbusc1", 86, "wmbus"},
    {732, "wmbusr2", 86, "wmbus"},
    {735, "wmbusn1", 86, "wmbus"},
    {743, "wmbuskam", 86, "wmbus"},
    {797, "wmbuslgb", 86, "wmbus"},
    {467, "wmbusitw", 86, "wmbus"},
    {470, "wmbussen", 86, "wmbus"},
    {792, "g3ruh9600", 61, "cubesat"},
    {795, "ccsdsrs", 61, "cubesat"},
    {804, "ccsdsconcat", 61, "cubesat"},
    {738, "aisclassa", 14, "ais"},
    {807, "aisclassb", 14, "ais"},
    {810, "aisbase", 14, "ais"},
    {813, "aisaton", 14, "ais"},
    {816, "aissar", 14, "ais"},
    {857, "aissart", 14, "ais"},
    {607, "dcf77", 58, "time"},
    {610, "msf", 58, "time"},
    {613, "wwvb", 58, "time"},
    {616, "jjy40", 58, "time"},
    {628, "jjy60", 58, "time"},
    {631, "wwv", 58, "time"},
    {634, "bpm", 58, "time"},
    {637, "bpc", 58, "time"},
    {642, "hla", 58, "time"},
    {645, "bpmcoded", 58, "time"},
    {648, "rwm", 58, "time"},
    {860, "gnssgps", 12, "gnss"},
    {866, "gnssqzss", 12, "gnss"},
    {872, "gnssglo", 12, "gnss"},
    {875, "gnssbds", 12, "gnss"},
    {424, "atvpalbg", 8, "atv"},
    {427, "atvpali", 8, "atv"},
    {441, "atvpaldk", 8, "atv"},
    {444, "atvsecamdk", 8, "atv"},
    {447, "atvsecaml", 8, "atv"},
    {450, "atvntscm", 8, "atv"},
    {453, "atvpalm", 8, "atv"},
    {456, "atvpaln", 8, "atv"},
    {473, "nfctypea", 148, "nfc"},
    {476, "nfctypeb", 148, "nfc"},
    {482, "nfc15693", 148, "nfc"},
    };
    n = (int)(sizeof k / sizeof *k);
    return k;
}
// the family to open for a saved family (the same one unless it was retired)
inline int liveFamily(int family) {
    int n = 0; const RetiredMode* r = retiredModes(n);
    for (int i = 0; i < n; i++) if (r[i].family == family) return r[i].parentFamily;
    return family;
}
// the mode id (ModeTuning::id, as presets and scan lists store it) to use for a saved id
inline std::string liveModeId(const std::string& id) {
    int n = 0; const RetiredMode* r = retiredModes(n);
    for (int i = 0; i < n; i++) if (id == r[i].id) return r[i].parentId;
    return id;
}

} // namespace dect2
