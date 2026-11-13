// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Offline aircraft identity from the 24-bit ICAO address: the country (ICAO Annex 10 Vol III allocation blocks), the registration where the
// address is a fixed function of it (US N-numbers, Canada C-Fxxx / C-Gxxx, Australia VH-xxx), the type from the emitter category, and a
// user-supplied database (readsb / tar1090 aircraft CSV). No network, no photos.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace dect2 {

struct AdsbCountry { const char* iso; const char* name; };       // iso: two letters; {"", "unknown"} when no block covers the address
AdsbCountry adsbCountry(uint32_t icao);

// US N-numbers (FAA algorithm: A00001 = N1 ... ADF7C7 = N99999; the letters I and O are not used)
bool adsbNNumberFromIcao(uint32_t icao, std::string& reg);       // "N1", "N1000", "N99999", "N12AB"
bool adsbIcaoFromNNumber(const std::string& reg, uint32_t& icao);
// Canada: C-FAAA .. C-GZZZ, Australia: VH-AAA .. VH-ZZZ (three letters, base 26, in order from the start of the block)
bool adsbCanadaFromIcao(uint32_t icao, std::string& reg);
bool adsbIcaoFromCanada(const std::string& reg, uint32_t& icao);
bool adsbAustraliaFromIcao(uint32_t icao, std::string& reg);
bool adsbIcaoFromAustralia(const std::string& reg, uint32_t& icao);
// The registration that follows from the address alone, "" when none does
std::string adsbAlgorithmicRegistration(uint32_t icao);

// "A3" -> "Large (75 000 - 300 000 lb)", ... "" when unknown
std::string adsbCategoryText(const std::string& cat);

struct AdsbRegEntry { std::string registration, type, operatorName; };
class AdsbRegDb {
public:
    // CSV, one aircraft per line: icao,registration,type,operator. A ';' separated file (the readsb / tar1090 aircraft database) works too; its
    // operator is then taken from the ninth column when there is one. A line that cannot be read is skipped and counted.
    // Returns the number of aircraft read; skipped (if given) the number of lines left out.
    size_t loadText(const std::string& text, size_t* skipped = nullptr);
    bool loadFile(const std::string& path, size_t* count = nullptr, size_t* skipped = nullptr);
    const AdsbRegEntry* find(uint32_t icao) const;
    size_t size() const { return map_.size(); }
    void clear() { map_.clear(); }
    // registration: the database first, then the algorithmic one
    std::string registration(uint32_t icao) const;
private:
    std::unordered_map<uint32_t, AdsbRegEntry> map_;
};

} // namespace dect2
