// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD / SD simulcast in the TV player: remembers the services of every scanned multiplex, finds the HD version of the playing service (core/src/tv/hd_pair.h),
// draws the 'HD' checkbox next to the channel picker and switches between the two, retuning when the other version is in another multiplex.
#include "../app.h"
#include "dect2/tv/hd_pair.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace dect2;

namespace {
bool sameFreq(double x, double y) { return std::fabs(x - y) < 0.01; }

std::string chooseText(const std::map<std::string, int>& m) {
    std::string o;
    for (const auto& kv : m) o += kv.first + "\t" + std::to_string(kv.second) + "\n";
    return o;
}

SavedChannel channelAt(const App& a, double freqMhz) {
    for (const auto& c : a.channels) if (sameFreq(c.freqMhz, freqMhz)) return c;
    SavedChannel sc; sc.freqMhz = freqMhz; sc.bwMhz = kBw[a.bwIdx].mhz;
    return sc;
}

// Switch to service `idx` of the catalogue (same multiplex: at once, another one: retune first). `choice` is remembered for the programme.
void switchTo(App& a, int idx, int choiceForKey, const std::string& key) {
    if (idx < 0 || idx >= (int)a.hdCat.all().size()) return;
    const hd::Service t = a.hdCat.all()[idx];
    Player& pl = a.engine.player();
    if (!key.empty() && choiceForKey >= 0) { a.hdChoice[key] = choiceForKey; savePrefs(a); }
    a.hdLang.clear();
    {   // keep the audio language
        auto tr = pl.audioTracks();
        if (a.tvAudioIdx >= 0 && a.tvAudioIdx < (int)tr.size()) a.hdLang = tr[a.tvAudioIdx].lang;
    }
    if (sameFreq(t.freqMhz, a.freqMhz)) {
        pl.select(t.sid); pl.setVolume(a.volume);
        a.hdLangSid = t.sid;
        a.hdLastSid = t.sid; a.hdLastFreq = t.freqMhz;
        a.hdPendSid = -1;
    } else {
        tuneToChannel(a, channelAt(a, t.freqMhz));
        a.hdPendSid = t.sid; a.hdPendTsid = t.tsid; a.hdPendFreq = t.freqMhz; a.hdPendT = glfwGetTime();
        a.hdLangSid = t.sid;
        a.hdLastSid = -1;
    }
}

int currentIndex(const App& a) {
    const int sid = a.engine.player().selected();
    return sid < 0 ? -1 : a.hdCat.find(a.freqMhz, sid);
}

void refreshCache(App& a) {
    const int sid = a.engine.player().selected();
    if (sid == a.hdKeySid && a.hdKeyRev == a.hdRev && sameFreq(a.hdKeyFreq, a.freqMhz)) return;
    a.hdKeySid = sid; a.hdKeyRev = a.hdRev; a.hdKeyFreq = a.freqMhz;
    a.hdCacheHd = a.hdCacheSd = -1; a.hdCacheWhyHd.clear(); a.hdCacheWhySd.clear();
    const int i = currentIndex(a);
    if (i < 0) return;
    hd::Pairing h = hd::findHd(a.hdCat.all(), (size_t)i, a.hdTable), s = hd::findSd(a.hdCat.all(), (size_t)i, a.hdTable);
    a.hdCacheHd = h.index; a.hdCacheWhyHd = h.reason;
    a.hdCacheSd = s.index; a.hdCacheWhySd = s.reason;
}
} // namespace

hd::Service tvHdService(const App& a, const TsService& sv) {
    hd::Service o;
    o.freqMhz = a.freqMhz; o.onid = a.ts.onid; o.tsid = a.ts.tsid; o.sid = sv.id;
    o.name = sv.name; o.provider = sv.provider; o.lcn = sv.lcn; o.serviceType = sv.type;
    o.linkOnid = sv.linkOnid; o.linkTsid = sv.linkTsid; o.linkSid = sv.linkSid;
    for (const auto& st : sv.streams) if (st.kind == "video") { o.videoCodec = st.codec; o.streamType = st.streamType; break; }
    const int i = a.hdCat.find(a.freqMhz, sv.id);
    if (i >= 0) { o.width = a.hdCat.all()[i].width; o.height = a.hdCat.all()[i].height; }
    return o;
}

void tvHdLoad(App& a) {
    plat::Prefs& d = plat::prefs();
    a.hdCat.decode(d.getS("tvCatalog", ""));
    a.hdCatText = a.hdCat.encode();
    a.hdTable = hd::decodeOverrides(d.getS("tvHdPairs", ""));
    a.hdAlways = d.getB("tvHdAlways", false);
    a.hdChoice.clear();
    std::istringstream is(d.getS("tvHdChoice", ""));
    for (std::string l; std::getline(is, l);) {
        const size_t t = l.find('\t');
        if (t != std::string::npos && t > 0) a.hdChoice[l.substr(0, t)] = std::atoi(l.c_str() + t + 1) ? 1 : 0;
    }
    a.hdRev++;
}

void tvHdSave(const App& a) {
    plat::Prefs& d = plat::prefs();
    d.setS("tvCatalog", a.hdCat.encode());
    d.setS("tvHdPairs", hd::encodeOverrides(a.hdTable));
    d.setB("tvHdAlways", a.hdAlways);
    d.setS("tvHdChoice", chooseText(a.hdChoice));
}

void tvHdIngest(App& a) {
    if (a.engine.multiChannelActive()) return;
    Player& pl = a.engine.player();
    const double now = glfwGetTime();
    // the services of this multiplex into the catalogue
    std::vector<hd::Service> v;
    for (const TsService& sv : a.ts.services) if (!sv.name.empty() && (sv.type == 0 || sv.type == 1 || sv.type == 0x11 || sv.type == 0x16 || sv.type == 0x19 || sv.type == 0x1F || sv.type == 0x1C || sv.type == 0x20 || sv.type == 0x21 || sv.type == 0x22 || sv.type == 0x23 || sv.type == 0x24 || sv.type == 0x25 || sv.type == 0x26)) v.push_back(tvHdService(a, sv));
    if (!v.empty()) a.hdCat.update(a.freqMhz, v);
    const PlayerStats ps = pl.stats();
    if (ps.serviceId >= 0 && ps.width > 0 && ps.height > 0) a.hdCat.setResolution(a.freqMhz, ps.serviceId, ps.width, ps.height);
    {
        const std::string enc = a.hdCat.encode();
        if (enc != a.hdCatText) {
            a.hdCatText = enc; a.hdRev++;
            if (a.rxId <= 1 && now - a.hdCatT > 5.0) { a.hdCatT = now; plat::prefs().setS("tvCatalog", enc); plat::prefs().flush(); }
        }
    }
    // a switch that waited for the retune
    if (a.hdPendSid >= 0) {
        if (now - a.hdPendT > 90.0) a.hdPendSid = -1;
        else if (sameFreq(a.freqMhz, a.hdPendFreq) && a.ts.tsid == a.hdPendTsid)
            for (const TsService& sv : a.ts.services)
                if (sv.id == a.hdPendSid && sv.havePmt) { pl.select(sv.id); pl.setVolume(a.volume); a.hdLastSid = sv.id; a.hdLastFreq = a.freqMhz; a.hdPendSid = -1; break; }
    }
    // keep the audio language over the switch
    if (a.hdLangSid >= 0 && a.hdLangSid == pl.selected() && sameFreq(a.freqMhz, a.hdLastFreq ? a.hdLastFreq : a.freqMhz)) {
        auto tr = pl.audioTracks();
        if (!tr.empty()) {
            for (int i = 0; i < (int)tr.size() && !a.hdLang.empty(); i++)
                if (tr[i].lang == a.hdLang) { pl.setAudioTrack(i); a.tvAudioIdx = i; break; }
            a.hdLangSid = -1;
        }
    }
    // a service was picked: the remembered choice, or 'always prefer HD', moves from the SD to the HD version in the same multiplex
    const int sid = pl.selected();
    if (sid >= 0 && (sid != a.hdLastSid || !sameFreq(a.freqMhz, a.hdLastFreq)) && a.hdPendSid < 0) {
        a.hdLastSid = sid; a.hdLastFreq = a.freqMhz;
        const int i = a.hdCat.find(a.freqMhz, sid);
        if (i >= 0) {
            const hd::Service& cur = a.hdCat.all()[i];
            const std::string key = hd::normalizeName(cur.name).key;
            const auto ch = a.hdChoice.find(key);
            const bool wantHd = ch != a.hdChoice.end() ? ch->second == 1 : a.hdAlways;
            if (wantHd && cur.tier() < hd::kHD) {
                const hd::Pairing p = hd::findHd(a.hdCat.all(), (size_t)i, a.hdTable);
                if (p.index >= 0 && sameFreq(a.hdCat.all()[p.index].freqMhz, a.freqMhz)) {
                    const hd::Service t = a.hdCat.all()[p.index];
                    pl.select(t.sid); pl.setVolume(a.volume);
                    a.hdLastSid = t.sid;
                    auto tr = pl.audioTracks();
                    a.hdLang = a.tvAudioIdx >= 0 && a.tvAudioIdx < (int)tr.size() ? tr[a.tvAudioIdx].lang : std::string();
                    a.hdLangSid = t.sid;
                }
            }
        }
    }
}

void tvHdCheckbox(App& a) {
    refreshCache(a);
    const hd::Catalog& cat = a.hdCat;
    const int i = currentIndex(a);
    const bool isHd = i >= 0 && cat.all()[i].tier() >= hd::kHD;
    const int other = isHd ? a.hdCacheSd : a.hdCacheHd;
    const bool enabled = i >= 0 && other >= 0;
    bool on = isHd && other >= 0;
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Checkbox("HD##hdsw", &on) && enabled) {
        const std::string key = hd::normalizeName(cat.all()[i].name).key;
        switchTo(a, other, on ? 1 : 0, key);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (enabled) {
            const hd::Service& t = cat.all()[other];
            const bool farFreq = !sameFreq(t.freqMhz, a.freqMhz);
            char fq[32]; snprintf(fq, sizeof fq, "%.3f", t.freqMhz);
            const std::string b = std::string(isHd ? "Switches back to " : "Switches to ") + t.name + (farFreq ? std::string(" on ") + fq + " MHz (retunes)" : std::string()) +
                                  "  [" + (isHd ? a.hdCacheWhySd : a.hdCacheWhyHd) + "]";
            ImGui::SetTooltip("%s", b.c_str());
        } else ImGui::SetTooltip("%s", i < 0 ? "Pick a service to see whether it has an HD version" : "no HD version found on this multiplex or in your scan");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("...##hdo")) ImGui::OpenPopup("hdopts");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("HD options and the pairing table");
    if (ImGui::BeginPopup("hdopts")) {
        if (ImGui::Checkbox("Always prefer HD when available", &a.hdAlways)) { a.hdRev++; savePrefs(a); }
        ImGui::TextDisabled("A programme you switched by hand keeps your choice.");
        ImGui::Separator();
        ImGui::TextUnformatted("Pairing table (overrides the automatic match)");
        for (size_t k = 0; k < a.hdTable.size(); k++) {
            ImGui::PushID((int)k);
            if (ImGui::SmallButton("x")) { a.hdTable.erase(a.hdTable.begin() + k); a.hdRev++; savePrefs(a); ImGui::PopID(); break; }
            ImGui::SameLine();
            ImGui::Text("%s  ->  %s", a.hdTable[k].sd.c_str(), a.hdTable[k].hd == "-" ? "never pair" : a.hdTable[k].hd.c_str());
            ImGui::PopID();
        }
        if (a.hdTable.empty()) ImGui::TextDisabled("empty");
        if (i >= 0) {
            ImGui::Separator();
            const std::string me = cat.all()[i].name;
            ImGui::Text("For %s:", me.c_str());
            if (ImGui::SmallButton("never pair")) {
                a.hdTable.erase(std::remove_if(a.hdTable.begin(), a.hdTable.end(), [&](const hd::Override& o) { return o.sd == me; }), a.hdTable.end());
                a.hdTable.push_back({me, "-"}); a.hdRev++; savePrefs(a);
            }
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::BeginCombo("##hdpick", "pair with...")) {
                for (size_t k = 0; k < cat.all().size(); k++) {
                    const hd::Service& s = cat.all()[k];
                    if ((int)k == i || s.tier() < hd::kHD) continue;
                    char l[200]; snprintf(l, sizeof l, "%s  (%.3f MHz)", s.name.c_str(), s.freqMhz);
                    if (ImGui::Selectable(l)) {
                        a.hdTable.erase(std::remove_if(a.hdTable.begin(), a.hdTable.end(), [&](const hd::Override& o) { return o.sd == me; }), a.hdTable.end());
                        a.hdTable.push_back({me, s.name}); a.hdRev++; savePrefs(a);
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::EndPopup();
    }
}
