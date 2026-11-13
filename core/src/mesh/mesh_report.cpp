// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh session report (mesh_report.h)
#include "dect2/mesh/mesh_report.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <vector>

namespace dect2 {

namespace {

const char* netName(int proto) { return proto == 1 ? "Meshtastic" : proto == 2 ? "MeshCore" : proto == 3 ? "LoRa APRS" : proto == 4 ? "MeshCom" : "LoRaWAN"; }

std::string clockText(double ut, bool utc, const char* fmt) {
    if (ut <= 0) return "-";
    const std::time_t tt = (std::time_t)ut;
    std::tm tm{};
#ifdef _WIN32
    if (utc) gmtime_s(&tm, &tt); else localtime_s(&tm, &tt);
#else
    if (utc) gmtime_r(&tt, &tm); else localtime_r(&tt, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, fmt, &tm);
    return b;
}

std::string fmt(const char* f, double a) { char b[64]; snprintf(b, sizeof b, f, a); return b; }

} // namespace

std::string meshReport(const MeshTelemetry& t, const std::string& plan, double nowUnix, bool utc) {
    auto clock = [&](double sec) { return sec < 0 ? 0.0 : nowUnix - (t.timeSec - sec); };
    double first = -1, last = -1;
    for (const auto& p : t.packets) { if (first < 0 || p.timeSec < first) first = p.timeSec; last = std::max(last, p.timeSec); }
    for (const auto& n : t.nodes) {
        if (n.firstHeard >= 0 && (first < 0 || n.firstHeard < first)) first = n.firstHeard;
        last = std::max(last, n.lastHeard);
    }
    std::string o = "OnAir mesh report\n";
    if (first >= 0) {
        const int mins = (int)((last - first) / 60 + 0.5);
        o += "Period: " + clockText(clock(first), utc, "%Y-%m-%d %H:%M") + " - " + clockText(clock(last), utc, "%H:%M") +
             (mins >= 60 ? " (" + std::to_string(mins / 60) + " h " + std::to_string(mins % 60) + " min)" : " (" + std::to_string(mins) + " min)") + "\n";
    }
    if (!plan.empty()) o += "Listening to: " + plan + "\n";
    // frames: the counters of this run; the packet list for what did not decrypt
    const uint64_t frames = t.blocksOk + t.blocksBad;
    o += "Frames: " + std::to_string(t.blocksOk) + " good, " + std::to_string(t.blocksBad) + " with a CRC error";
    if (frames) o += fmt(" (%.1f %%)", 100.0 * (double)t.blocksBad / (double)frames);
    o += "\n";
    size_t enc = 0;
    for (const auto& p : t.packets) if (p.crcOk && p.protocol == 1 && !p.decrypted) enc++;
    if (enc) o += "Not decrypted (unknown channel key) among the last " + std::to_string(t.packets.size()) + " packets: " + std::to_string(enc) + "\n";
    if (!t.counts.empty()) {
        std::vector<MeshTypeCount> c = t.counts;
        std::sort(c.begin(), c.end(), [](const MeshTypeCount& a, const MeshTypeCount& b) { return a.count > b.count; });
        o += "Packet types:";
        for (size_t i = 0; i < c.size(); i++) o += (i ? ", " : " ") + c[i].type + " " + std::to_string(c[i].count);
        o += "\n";
    }
    // nodes: the most heard first
    std::vector<const MeshNode*> ns;
    for (const auto& n : t.nodes) ns.push_back(&n);
    std::sort(ns.begin(), ns.end(), [](const MeshNode* a, const MeshNode* b) { return a->packets != b->packets ? a->packets > b->packets : a->id < b->id; });
    o += "\nNodes: " + std::to_string(ns.size()) + "\n";
    for (const MeshNode* n : ns) {
        std::string name = !n->longName.empty() ? n->longName : "(no name yet)";
        if (!n->shortName.empty() && n->shortName != n->longName) name += " (" + n->shortName + ")";
        o += "\n" + name + "  " + n->id + "  " + netName(n->protocol) + "\n";
        std::string hw = n->hwModel;
        if (!n->role.empty()) hw += (hw.empty() ? "" : ", ") + n->role;
        if (!hw.empty()) o += "  " + hw + "\n";
        o += "  " + std::to_string(n->packets) + (n->packets == 1 ? " packet" : " packets");
        if (n->encrypted) o += " (" + std::to_string(n->encrypted) + " not decrypted)";
        if (n->packets) o += ", SNR " + fmt("%.1f", n->snrMin) + " / " + fmt("%.1f", n->snrSum / n->packets) + " / " + fmt("%.1f", n->snrMax) + " dB (min / mean / max)";
        if (n->hopsAway == 0) o += ", direct";
        else if (n->hopsAway > 0) o += ", " + std::to_string(n->hopsAway) + (n->hopsAway == 1 ? " hop" : " hops");
        o += "\n";
        o += "  heard " + clockText(clock(n->firstHeard), utc, "%H:%M") + " - " + clockText(clock(n->lastHeard), utc, "%H:%M");
        o += "\n";
        if (n->batteryPct >= 0 || n->voltage > 0) {   // the node's last device telemetry, with when it was sent
            o += "  ";
            if (n->batteryPct > 100) o += "powered";
            else if (n->batteryPct >= 0) o += "battery " + fmt("%.0f %%", n->batteryPct);
            if (n->voltage > 0) o += fmt(n->batteryPct >= 0 ? " (%.2f V)" : "%.2f V", n->voltage);
            if (n->devSec >= 0) o += " at " + clockText(clock(n->devSec), utc, "%H:%M");
            o += "\n";
        }
        if (n->hasPosition) o += "  position " + fmt("%.5f", n->lat) + ", " + fmt("%.5f", n->lon) + fmt(", %.0f m", n->altM) + "\n";
        if (n->hasEnv) o += "  sensors " + fmt("%.1f C", n->tempC) + fmt(", %.0f %%", n->humidity) + fmt(", %.0f hPa", n->pressureHpa) +
                            (n->envSec >= 0 ? " at " + clockText(clock(n->envSec), utc, "%H:%M") : std::string()) + " (last reading)\n";
    }
    // the text messages, oldest first
    if (!t.messages.empty()) {
        o += "\nMessages: " + std::to_string(t.messages.size()) + "\n";
        for (const auto& m : t.messages) {
            std::string who = !m.fromName.empty() ? m.fromName : m.from;
            std::string to = m.to == "^all" || m.to.empty() ? m.channel : m.to;
            o += clockText(clock(m.timeSec), utc, "%H:%M") + "  " + who + " > " + to + ": " + m.text + "\n";
        }
    }
    return o;
}

} // namespace dect2
