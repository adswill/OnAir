// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The few functions, structures and constants of Tektronix's RSA API (RSA_API.dll / libRSA_API.so, "V2", for the RSA306, RSA306B,
// RSA500A and RSA600A) that source_rsa.cpp uses, declared here from the API's programmer manual (077-1031) so that nothing of
// Tektronix's is needed to build OnAir. The library itself is the user's to install; it is loaded at run time. docs/TEKTRONIX-RSA.md
#pragma once
#include <cstdint>

namespace dect2 {
namespace tekrsa {

typedef int Status;                    // ReturnStatus: 0 is "no error"
constexpr Status kNoError = 0;
constexpr Status kErrorBandwidthOutOfRange = 1303;   // IQSTREAM_SetAcqBandwidth: the nearest legal bandwidth was taken

constexpr int kMaxDevices = 20, kSerialLen = 100, kTypeLen = 20, kInfoLen = 100;

// IQSTREAM_SetOutputConfiguration
constexpr int kOutputClient = 0;       // the samples are handed to the program (no file)
constexpr int kTypeSingle = 0;         // float32 I/Q pairs in volts into 50 ohm

// IQSTREAM_GetIQData: what comes with a block of samples
struct IqInfo {
    uint64_t timestamp;                // of the first sample
    int triggerCount;
    int* triggerIndices;
    double scaleFactor;                // for the integer sample types
    uint32_t acqStatus;                // kStatus... bits of this block; the same bits 16 higher stay set for the whole run
};
constexpr uint32_t kStatusOverrange = 1u << 0;        // the RF input was overdriven
constexpr uint32_t kStatusDiscontinuity = 1u << 1;    // samples were lost between the device and the computer
constexpr uint32_t kStatusInputOverflow = 1u << 3;    // the API's processing did not keep up
constexpr uint32_t kStatusOutputOverflow = 1u << 5;   // the program did not fetch the blocks fast enough

struct DeviceInfo {
    char nomenclature[kInfoLen], serialNum[kInfoLen], apiVersion[kInfoLen], fwVersion[kInfoLen], fpgaVersion[kInfoLen], hwVersion[kInfoLen];
};

// The functions, as the library exports them (C linkage, the platform's default calling convention)
extern "C" {
typedef const char* (*GetErrorStringFn)(Status);
typedef Status (*SearchFn)(int* found, int ids[], char serials[][kSerialLen], char types[][kTypeLen]);
typedef Status (*ConnectFn)(int id);
typedef Status (*VoidFn)(void);                        // DEVICE_Disconnect, DEVICE_Run, DEVICE_Stop, IQSTREAM_Start, IQSTREAM_Stop
typedef Status (*GetInfoFn)(DeviceInfo*);
typedef Status (*SetDoubleFn)(double);                 // CONFIG_SetCenterFreq, CONFIG_SetReferenceLevel, IQSTREAM_SetAcqBandwidth
typedef Status (*GetDoubleFn)(double*);
typedef Status (*SetBoolFn)(bool);                     // CONFIG_SetRFPreampEnable
typedef Status (*GetBoolFn)(bool*);
typedef Status (*GetAcqParametersFn)(double* bandwidthHz, double* sampleRate);
typedef Status (*SetOutputConfigurationFn)(int dest, int dtype);
typedef Status (*GetBufferSizeFn)(int* samples);
typedef Status (*GetIqDataFn)(void* iq, int* samples, IqInfo*);
}

} // namespace tekrsa
} // namespace dect2
