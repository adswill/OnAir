// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR sound: the DRA+ decoder as an add-on the user installs ("libonair_dra"), loaded at run time. OnAir ships no DRA+ decoder and no decoder
// source (the codec's patent and licence status is not verified, docs/CODECS.md "DRA+ (CDR sound)"); without the library the CDR services, signal
// information and data still work and the audio units are only counted.
//
// Finding it works like the HDC and mbelib loaders: DECT2_DRA_PATH (a full path), the OnAir codecs folders, the program's folder, the system's places.
// The library exports five C functions (DRADecCreate, DRADecDestroy, DRADecGetFrameInfo, DRADecSendData, DRADecRecvFrame; cdr_dra_abi.h).
#pragma once
#include <string>
#include <vector>

namespace dect2 { namespace cdr {

struct DraFrameInfo { int nChannels; int nSampleRate; int nFrameSize; };   // DRAFrameInfo of the library

struct DraApi {
    void* (*create)() = nullptr;
    void (*destroy)(void*) = nullptr;
    int (*info)(void*, DraFrameInfo*) = nullptr;
    int (*send)(void*, unsigned char*, int) = nullptr;
    int (*recv)(void*, unsigned char**, int*) = nullptr;
};

bool draAvailable(std::string* path = nullptr, std::string* problem = nullptr);   // found and usable; else `problem` says why not ("" = no file found)
const DraApi* draApi();                                                           // null when not usable; valid for the life of the program
bool draRescan();                                                                 // look again (the "Look again" button); true when usable
std::vector<std::string> draFileNames();                                          // the names OnAir looks for, no folder
std::vector<std::string> draCandidatePaths();                                     // every path tried, in order

}} // namespace dect2::cdr
