// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The part of Aaronia's RTSA API (the C API of the RTSA-Suite PRO, library AaroniaRTSAAPI.dll / libAaroniaRTSAAPI.so) that the SPECTRAN V6
// driver (source_spectran.cpp) uses, declared here so that OnAir builds without the SDK. Aaronia's own header (aaroniartsaapi.h) is not
// included or copied: it comes with the RTSA-Suite PRO installer under Aaronia's licence. These declarations were written from the public
// documentation: the README and samples of github.com/Aaronia-Open-source/RTSA-API-Samples (function names, device and config names, the
// packet fields) and the result codes as other open projects list them (SatDump, SDR++: see THIRD_PARTY.md). The library is loaded at run
// time; the user installs the RTSA-Suite PRO from aaronia.com/downloads. docs/SPECTRAN-V6.md.
#pragma once
#include <cstdint>
#include <cwchar>

namespace dect2 {
namespace aartsa {

typedef uint32_t Result;
constexpr Result kOk = 0x00000000, kEmpty = 0x00000001, kRetry = 0x00000002;
// device states (AARTSAAPI_GetDeviceState)
constexpr Result kIdle = 0x10000000, kConnecting = 0x10000001, kConnected = 0x10000002, kStarting = 0x10000003, kRunning = 0x10000004,
                 kStopping = 0x10000005, kDisconnecting = 0x10000006;
// warnings: the call worked, a value was changed to one the device takes
constexpr Result kWarning = 0x40000000, kWarningValueAdjusted = 0x40000001, kWarningValueDisabled = 0x40000002;
constexpr Result kError = 0x80000000, kErrorNotInitialized = 0x80000001, kErrorNotFound = 0x80000002, kErrorBusy = 0x80000003,
                 kErrorNotOpen = 0x80000004, kErrorNotConnected = 0x80000005, kErrorInvalidConfig = 0x80000006, kErrorBufferSize = 0x80000007,
                 kErrorInvalidChannel = 0x80000008, kErrorInvalidParameter = 0x80000009, kErrorInvalidSize = 0x8000000a,
                 kErrorMissingPathsFile = 0x8000000b, kErrorValueInvalid = 0x8000000c, kErrorValueMalformed = 0x8000000d;
inline bool failed(Result r) { return (r & 0x80000000u) != 0; }

constexpr uint32_t kMemorySmall = 0, kMemoryMedium = 1, kMemoryLarge = 2, kMemoryLudicrous = 3;

constexpr uint64_t kPacketStreamStart = 0x1, kPacketStreamEnd = 0x2, kPacketSegmentStart = 0x4, kPacketSegmentEnd = 0x8;

struct Handle { void* d; };
struct Device { void* d; };
struct Config { void* d; };

// One block of samples. IQ: num complex samples, fp32[k * stride] = I, fp32[k * stride + 1] = Q; the band starts at startFrequency and
// stepFrequency is the sample rate (the centre is startFrequency + stepFrequency / 2). Times are the device's stream clock in seconds.
struct Packet {
    int64_t cbsize;   // sizeof(Packet): set before every call
    uint64_t streamID;
    uint64_t flags;
    double startTime;
    double endTime;
    double startFrequency;
    double stepFrequency;
    double spanFrequency;
    double rbwFrequency;
    int64_t num;
    int64_t total;
    int64_t size;
    int64_t stride;
    float* fp32;
    int64_t interleave;
};

struct DeviceInfo {
    int64_t cbsize;   // sizeof(DeviceInfo)
    wchar_t serialNumber[120];
    bool ready;
    bool boost;
    bool superspeed;
    bool active;
};

// The functions, as the library exports them (C linkage, the platform's default calling convention)
extern "C" {
typedef Result (*InitFn)(uint32_t memory);
typedef Result (*InitWithPathFn)(uint32_t memory, const wchar_t* xmlLookupDir);   // newer libraries: the folder that holds paths.xml
typedef Result (*ShutdownFn)(void);
typedef uint32_t (*VersionFn)(void);
typedef Result (*OpenFn)(Handle*);
typedef Result (*CloseFn)(Handle*);
typedef Result (*RescanDevicesFn)(Handle*, int timeoutMs);
typedef Result (*EnumDeviceFn)(Handle*, const wchar_t* type, int32_t index, DeviceInfo*);
typedef Result (*OpenDeviceFn)(Handle*, Device*, const wchar_t* type, const wchar_t* serial);
typedef Result (*CloseDeviceFn)(Handle*, Device*);
typedef Result (*DeviceFn)(Device*);   // Connect, Disconnect, Start, Stop, GetDeviceState
typedef Result (*GetPacketFn)(Device*, int32_t channel, int32_t index, Packet*);
typedef Result (*ConsumePacketsFn)(Device*, int32_t channel, int32_t num);
typedef Result (*SendPacketFn)(Device*, int32_t channel, const Packet*);
typedef Result (*GetMasterStreamTimeFn)(Device*, double* seconds);   // (declared with a double& in C++: the same in the ABI)
typedef Result (*ConfigRootFn)(Device*, Config*);
typedef Result (*ConfigFindFn)(Device*, Config* group, Config* out, const wchar_t* path);
typedef Result (*ConfigSetFloatFn)(Device*, Config*, double);
typedef Result (*ConfigSetStringFn)(Device*, Config*, const wchar_t*);
typedef Result (*ConfigGetFloatFn)(Device*, Config*, double*);
}

} // namespace aartsa
} // namespace dect2
