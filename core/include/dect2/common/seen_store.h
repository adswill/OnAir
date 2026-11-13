// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What a receiver has heard over days and years, kept on disk (graphs1090, Virtual Radar Server "first seen", AIS-catcher statistics):
// every identity ever heard (an aircraft address, an MMSI ...) with its first and last day, a record per day (unique identities, new ones,
// positions, messages, the farthest one), an all-time range outline per bearing sector and altitude band, and the farthest ever.
// Shared by ADS-B and AIS. docs/SEEN-STORE-3.9.md.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

struct SeenDay {
    std::string day;                 // "2026-11-10" (local)
    uint32_t unique = 0, fresh = 0;  // identities heard that day, and of them the ones never heard before
    uint64_t positions = 0, messages = 0;
    double maxKm = 0;                // the farthest position that day
    std::string maxId;
    std::array<uint32_t, 24> hours{};   // identities heard in each local hour of the day
};

struct SeenId {
    int64_t firstUtc = 0, lastUtc = 0;
    std::string firstDay, lastDay;
    uint32_t days = 0;               // on how many days it was heard
    std::string name;                // a callsign / ship name, the last one seen
    std::string kind;                // a type ("B738", "Cargo"), the last one seen
    int64_t lastHour = 0;            // (utc + tz) / 3600 of the last hearing, for the hourly count (not saved)
};

class SeenStore {
public:
    static constexpr int kSectors = 36, kBands = 3;   // 10 degree sectors; bands: below 10 000 ft, 10 000 - 25 000, above (AIS: one band, 0)
    void setTzOffset(int sec) { tz_ = sec; }
    // an identity heard now; returns true the first time it is ever heard. name may be empty.
    bool seen(const std::string& id, int64_t utc, const std::string& name = {}, const std::string& kind = {});
    // a position of it: counts, and feeds the daily farthest and the outline
    void position(const std::string& id, int64_t utc, double km, double bearingDeg, int band);
    // messages heard (a count to add)
    void messages(int64_t utc, uint64_t n);
    bool known(const std::string& id) const { return ids_.count(id) != 0; }
    const SeenId* get(const std::string& id) const;
    bool newToday(const std::string& id, int64_t utc) const;   // first heard on the day of utc
    size_t uniqueEver() const { return ids_.size(); }
    // days, oldest first, the current one included (at most `maxDays` kept)
    const std::vector<SeenDay>& days() const { return days_; }
    const SeenDay* today(int64_t utc) const;
    const std::array<float, kSectors>& outline(int band) const { return outline_[band < 0 || band >= kBands ? 0 : band]; }
    double farthestKm(int band) const { return band >= 0 && band < kBands ? bandFarKm_[(size_t)band] : 0; }   // farthest ever in one band (AIS: one per class)
    const std::string& farthestId(int band) const { return bandFarId_[(size_t)(band >= 0 && band < kBands ? band : 0)]; }
    const std::unordered_map<std::string, SeenId>& ids() const { return ids_; }
    double farthestKm() const { return farKm_; }
    const std::string& farthestId() const { return farId_; }
    int64_t farthestUtc() const { return farUtc_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    // text form: one record per line; parse() replaces everything (false on a file that is not a store)
    std::string serialize() const;
    bool parse(const std::string& text);
    std::string dayOf(int64_t utc) const;
    static constexpr size_t kMaxDays = 400, kMaxIds = 2000000;
private:
    SeenDay& day(int64_t utc);
    int tz_ = 0;
    std::unordered_map<std::string, SeenId> ids_;
    std::vector<SeenDay> days_;
    std::array<std::array<float, kSectors>, kBands> outline_{};
    double farKm_ = 0; std::string farId_; int64_t farUtc_ = 0;
    std::array<double, kBands> bandFarKm_{};
    std::array<std::string, kBands> bandFarId_;
    bool dirty_ = false;
};

// CSV of the days: day, unique, new, positions, messages, max_km, max_id
std::string seenDaysCsv(const SeenStore& s);

}  // namespace dect2
