// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parsing layer of the DRM signalling and data services (ETSI ES 201 980): FAC and SDC parsing and checks, the audio super frame split, the text message
// segment check, the descriptive tables, MJD conversion and the data service helpers. The records are the ones of drm_msg.h. These functions
// are implemented by the Rust crate rust/onair_drm (drm_proto_rs.cpp). MOT and the packet mode headers are in dab_proto.h.
#pragma once
#include "drm_msg.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2::drmp {

uint32_t crc8(const uint8_t* bits, size_t n);     // CRC-8 of the FAC over unpacked bits (one per byte)
uint32_t crc16(const uint8_t* bits, size_t n);    // CRC-16 of the SDC over unpacked bits

bool facParse(const uint8_t* bits, bool modeE, drm::FacInfo& f);    // bits: drm::facInfoBits(modeE) bits; false: wrong CRC or the RM flag disagrees
int sdcParse(const uint8_t* data, int bytes, drm::SdcInfo& out);    // the entities refine `out` (see drm_msg.h); returns the entities read
bool sdcCheckBits(const uint8_t* bits, int fieldBytes, int& afsIndex);   // CRC check of a decoded block of 4 + 8 * fieldBytes + 16 bits
int aacNumFrames(bool modeE, int rateHz);
int aacHeaderBytes(int numFrames);
// numFrames up to 16; false also when the higher protected part does not fit the logical frame
bool aacSuperFrameParse(const uint8_t* lf, int len, int lenA, int numFrames, drm::AacSuperFrame& out);

std::string languageName(int code);                  // table 18
std::string programmeTypeName(int code);             // table 19
std::string mscModeName(bool modeE, int mscMode);
std::string occupancyName(bool modeE, int occ);
std::string appName(int domain, int userApp);        // TS 101 756 table 16 as DRM lists the applications
int facAudioServices(int numServicesCode);           // -1 for the reserved codes
int facDataServices(int numServicesCode);
void mjdToDate(int mjd, int& y, int& m, int& d);
int dateToMjd(int y, int m, int d);

// Data services: the payload of an MSC data group (false when damaged), the text of an object for display, the "looks like text" test.
bool groupPayload(const uint8_t* d, size_t n, size_t& off, size_t& len);
std::string displayText(const uint8_t* b, size_t n);
bool looksText(const uint8_t* b, size_t n);

// Text message (clause 6.5): bytes a segment needs from its first byte, and the check of a whole segment of `need` bytes.
int textSegmentNeed(uint8_t b0);
struct TextSeg { bool crcOk = false, toggle = false, first = false, last = false, command = false; int cmd = 0, segnum = 0; uint32_t n = 0; };
TextSeg textSegment(const uint8_t* seg, size_t n, int need);

} // namespace dect2::drmp
