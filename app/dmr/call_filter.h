// #389: the Calls table can show only one kind of call ("Control" rows take the whole screen on a busy site).
#pragma once
namespace dmrfilter {
enum { All = 0, VoiceOnly = 1, GroupVoice = 2, NoControl = 3, DataOnly = 4 };
inline const char* name(int f) { static const char* n[5] = {"All calls", "Voice only", "Group voice only", "Hide control", "Data only"}; return f >= 0 && f < 5 ? n[f] : "?"; }
// kind: 0 group voice, 1 private voice, 2 all call, 3 data, 4 control
inline bool shown(int f, int kind) {
    switch (f) {
        case VoiceOnly: return kind >= 0 && kind <= 2;
        case GroupVoice: return kind == 0;
        case NoControl: return kind != 4;
        case DataOnly: return kind == 3;
        default: return true;
    }
}
}
