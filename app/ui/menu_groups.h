// The drop-down menus' grouping data, in one place and without ImGui: the Tools menu (which item sits in which group) and the Mode menu's
// split of a big category into second-level chunks. app/ui/ui2_shell.cpp draws the menus from it; tests/app/test_menu_groups.cpp checks that
// every item still appears exactly once. Docs: docs/MENUS-4.0.md.
#pragma once
#include <cctype>
#include <cstring>
#include <string>

#ifndef N_
#define N_(s) s
#endif

namespace menugroups {

// ---- Tools menu ----
enum class Tool {
    Transcripts, Passes, Scheduler, Recordings, SatNogs, Df, Rfi, PassiveRadar, Images, Feeds, EchoLink, Wireshark, Swarm, Messages,
    ProtoLab, Esp32, SignalLab, ModeBuilder, TxStudio, VideoGen, Integrations, Conversations, Watchlists, Automations, Plugins, Backup,
    Memory, Datasets, Packs, Frequencies, FreqTab, Transmit, Map, Sweep, StationHealth, Heard, Insights, MyStation, Doctor, RfFp,
    WideAll, Trunking, Scanner, Propagation, AntDesigner, Rotator, VoiceKeys, Calibrate,
    PluginStore,   // new in 4.0: opens the plugin window on its Store tab
};
constexpr int kTopLevel = -1;   // shown above the groups
constexpr int kNumToolGroups = 9;
inline const char* toolGroupTitle(int g) {
    static const char* const t[kNumToolGroups] = {
        N_("Analysis and measurement"), N_("Scanning and hunting"), N_("Propagation and planning"), N_("Satellites and scheduling"),
        N_("Voice, messages and archives"), N_("Data feeds and network"), N_("Automation and extending"), N_("Transmit and generate"),
        N_("Calibration and hardware")};
    return g >= 0 && g < kNumToolGroups ? t[g] : "";
}
struct ToolItem { Tool id; const char* label; int group; };
// the label is the English text the item has always had (tr() runs when it is drawn); within a group, the order shown
inline const ToolItem* toolItems(int& n) {
    static const ToolItem items[] = {
        {Tool::Frequencies, N_("Frequencies..."), kTopLevel}, {Tool::FreqTab, N_("Show Frequencies as a tab"), kTopLevel},
        {Tool::Memory, N_("Memory..."), kTopLevel}, {Tool::Map, N_("Map"), kTopLevel}, {Tool::Recordings, N_("Recordings..."), kTopLevel},
        {Tool::PluginStore, N_("Plugin store..."), kTopLevel},
        {Tool::SignalLab, N_("Signal Lab..."), 0}, {Tool::ProtoLab, N_("Protocol lab..."), 0}, {Tool::RfFp, N_("RF fingerprint"), 0},
        {Tool::WideAll, N_("Everything in view"), 0}, {Tool::Insights, N_("Insights"), 0},
        {Tool::Sweep, N_("Sweep scanner"), 1}, {Tool::Scanner, N_("Scanner (highly experimental)"), 1}, {Tool::Rfi, N_("Interference hunter"), 1},
        {Tool::Df, N_("Direction finder"), 1}, {Tool::PassiveRadar, N_("Passive radar"), 1}, {Tool::Trunking, N_("Multi-radio trunking"), 1},
        {Tool::Propagation, N_("Propagation..."), 2}, {Tool::AntDesigner, N_("Antenna designer..."), 2}, {Tool::Rotator, N_("Antenna rotator..."), 2},
        {Tool::MyStation, N_("My station"), 2},
        {Tool::Passes, N_("Passes"), 3}, {Tool::Scheduler, N_("Scheduler..."), 3}, {Tool::SatNogs, N_("SatNOGS transmitters"), 3}, {Tool::Images, N_("Images"), 3},
        {Tool::Transcripts, N_("Transcripts ..."), 4}, {Tool::VoiceKeys, N_("Voice keys..."), 4}, {Tool::Conversations, N_("Conversations..."), 4},
        {Tool::Messages, N_("Messages..."), 4}, {Tool::Heard, N_("Search everything heard"), 4}, {Tool::Datasets, N_("Datasets..."), 4},
        {Tool::Packs, N_("Data packs..."), 4}, {Tool::Backup, N_("Backup and restore..."), 4},
        {Tool::Feeds, N_("Data feeds..."), 5}, {Tool::EchoLink, N_("EchoLink..."), 5}, {Tool::Wireshark, N_("Wireshark..."), 5},
        {Tool::Swarm, N_("LAN swarm..."), 5}, {Tool::Integrations, N_("Integrations..."), 5},
        {Tool::Watchlists, N_("Watchlists..."), 6}, {Tool::Automations, N_("Automations..."), 6}, {Tool::Plugins, N_("Plugins..."), 6},
        {Tool::ModeBuilder, N_("Mode Builder..."), 6},
        {Tool::Transmit, N_("Transmit..."), 7}, {Tool::TxStudio, N_("TX studio..."), 7}, {Tool::VideoGen, N_("Digital video generator..."), 7},
        {Tool::Esp32, N_("nRF24 / ESP32 radio..."), 7},
        {Tool::Calibrate, N_("Calibrate frequency..."), 8}, {Tool::StationHealth, N_("Station health"), 8}, {Tool::Doctor, N_("Reception doctor"), 8},
    };
    n = (int)(sizeof items / sizeof items[0]);
    return items;
}
// case-insensitive substring test for the menu's search entry (an empty query matches everything)
inline bool matchesQuery(const char* label, const char* q) {
    if (!q || !*q) return true;
    const std::string a = label, b = q;
    auto low = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    return low(a).find(low(b)) != std::string::npos;
}

// ---- Mode menu: a category with more than kMaxPerSubmenu modes gets a second level of consecutive chunks (table order) ----
constexpr int kMaxPerSubmenu = 25;
inline int chunkCount(int n) { return n <= kMaxPerSubmenu ? 1 : (n + kMaxPerSubmenu - 1) / kMaxPerSubmenu; }
// chunk i of n rows: [begin, end), sizes differ by at most one
inline void chunkRange(int n, int i, int& b, int& e) {
    const int c = chunkCount(n);
    b = (int)((long long)n * i / c); e = (int)((long long)n * (i + 1) / c);
}

}  // namespace menugroups
