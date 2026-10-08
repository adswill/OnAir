// The parts of the SDRplay API 3.x that OnAir uses, written out from the official headers sdrplay_api.h, sdrplay_api_dev.h,
// sdrplay_api_rx_channel.h, sdrplay_api_tuner.h, sdrplay_api_control.h, sdrplay_api_callback.h, sdrplay_api_rsp1a.h, sdrplay_api_rsp2.h,
// sdrplay_api_rspDuo.h and sdrplay_api_rspDx.h of API version 3.15 (SDRPLAY_API_VERSION 3.15; the same files as in the API installer,
// public copies e.g. in github.com/DSheirer/sdrtrunk artifacts/sdrplay-api-headers/v3_15), and the "SDRplay API Specification" v3.15.
// The library is loaded at run time (source_sdrplay.cpp), so nothing here links against it.
//
// Every struct the library reads or writes is here with the official field order and types, the nested ones that OnAir only passes
// through included, because the library fills them in its own layout. The official headers set no packing and no calling convention:
// natural alignment and the platform's default C convention (DECT2_CALL, __cdecl on Windows). Their C enums are 4 bytes on every compiler
// the API is built with; the enums below say so explicitly. The sizes checked at the end hold on 64-bit Windows (LLP64) and 64-bit Unix (LP64)
// alike, because no `long` appears in these structs.
//
// Differences between API versions that matter here (3.07 and 3.15 headers compared):
//   DeviceT.valid is new in 3.08; in 3.07 those bytes are padding (same size, same offsets of everything else), so it is read from 3.08 on only.
//   RspDuoTunerParamsT gained resetSlaveFlags after 3.08, which moved RxChannelParamsT.rspDxTunerParams from offset 136 to 140 (the size
//   stays 144). OnAir writes tunerParams, ctrlParams, rsp1aTunerParams, rsp2TunerParams and rspDuoTunerParams of a channel (never
//   rspDxTunerParams), which sit at the same offsets in 3.07 to 3.15.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dect2 {
namespace sdrplay {

// ------------------------------------------------------------------ sdrplay_api.h
constexpr float kHeaderVersion = 3.15f;   // SDRPLAY_API_VERSION of the headers above
constexpr int kMaxDevices = 16;           // SDRPLAY_MAX_DEVICES
constexpr int kMaxSerNoLen = 64;          // SDRPLAY_MAX_SER_NO_LEN

// hwVer values (SDRPLAY_*_ID)
constexpr unsigned char kRsp1 = 1, kRsp1A = 255, kRsp2 = 2, kRspDuo = 3, kRspDx = 4, kRsp1B = 6, kRspDxR2 = 7;

enum ErrT : int {   // sdrplay_api_ErrT
    Success = 0, Fail = 1, InvalidParam = 2, OutOfRange = 3, GainUpdateError = 4, RfUpdateError = 5, FsUpdateError = 6, HwError = 7,
    AliasingError = 8, AlreadyInitialised = 9, NotInitialised = 10, NotEnabled = 11, HwVerError = 12, OutOfMemError = 13,
    ServiceNotResponding = 14, StartPending = 15, StopPending = 16, InvalidMode = 17, FailedVerification1 = 18, FailedVerification2 = 19,
    FailedVerification3 = 20, FailedVerification4 = 21, FailedVerification5 = 22, FailedVerification6 = 23, InvalidServiceVersion = 24
};

enum ReasonForUpdateT : unsigned int {   // sdrplay_api_ReasonForUpdateT (the last flag is 0x80000000, so unsigned)
    Update_None = 0x00000000,
    Update_Dev_Fs = 0x00000001,
    Update_Dev_Ppm = 0x00000002,
    Update_Dev_SyncUpdate = 0x00000004,
    Update_Dev_ResetFlags = 0x00000008,
    Update_Rsp1a_BiasTControl = 0x00000010,
    Update_Rsp1a_RfNotchControl = 0x00000020,
    Update_Rsp1a_RfDabNotchControl = 0x00000040,
    Update_Rsp2_BiasTControl = 0x00000080,
    Update_Rsp2_AmPortSelect = 0x00000100,
    Update_Rsp2_AntennaControl = 0x00000200,
    Update_Rsp2_RfNotchControl = 0x00000400,
    Update_Rsp2_ExtRefControl = 0x00000800,
    Update_RspDuo_ExtRefControl = 0x00001000,
    Update_Master_Spare_1 = 0x00002000,
    Update_Master_Spare_2 = 0x00004000,
    Update_Tuner_Gr = 0x00008000,
    Update_Tuner_GrLimits = 0x00010000,
    Update_Tuner_Frf = 0x00020000,
    Update_Tuner_BwType = 0x00040000,
    Update_Tuner_IfType = 0x00080000,
    Update_Tuner_DcOffset = 0x00100000,
    Update_Tuner_LoMode = 0x00200000,
    Update_Ctrl_DCoffsetIQimbalance = 0x00400000,
    Update_Ctrl_Decimation = 0x00800000,
    Update_Ctrl_Agc = 0x01000000,
    Update_Ctrl_AdsbMode = 0x02000000,
    Update_Ctrl_OverloadMsgAck = 0x04000000,
    Update_RspDuo_BiasTControl = 0x08000000,
    Update_RspDuo_AmPortSelect = 0x10000000,
    Update_RspDuo_Tuner1AmNotchControl = 0x20000000,
    Update_RspDuo_RfNotchControl = 0x40000000,
    Update_RspDuo_RfDabNotchControl = 0x80000000u
};

enum ReasonForUpdateExtension1T : unsigned int {   // sdrplay_api_ReasonForUpdateExtension1T
    Update_Ext1_None = 0x00000000,
    Update_RspDx_HdrEnable = 0x00000001,
    Update_RspDx_BiasTControl = 0x00000002,
    Update_RspDx_AntennaControl = 0x00000004,
    Update_RspDx_RfNotchControl = 0x00000008,
    Update_RspDx_RfDabNotchControl = 0x00000010,
    Update_RspDx_HdrBw = 0x00000020,
    Update_RspDuo_ResetSlaveFlags = 0x00000040
};

enum DbgLvl_t : int { DbgLvl_Disable = 0, DbgLvl_Verbose = 1, DbgLvl_Warning = 2, DbgLvl_Error = 3, DbgLvl_Message = 4 };

// ------------------------------------------------------------------ sdrplay_api_tuner.h
enum Bw_MHzT : int {   // sdrplay_api_Bw_MHzT: the analogue IF filter, in kHz
    BW_Undefined = 0, BW_0_200 = 200, BW_0_300 = 300, BW_0_600 = 600, BW_1_536 = 1536, BW_5_000 = 5000, BW_6_000 = 6000, BW_7_000 = 7000,
    BW_8_000 = 8000
};
enum If_kHzT : int { IF_Undefined = -1, IF_Zero = 0, IF_0_450 = 450, IF_1_620 = 1620, IF_2_048 = 2048 };
enum LoModeT : int { LO_Undefined = 0, LO_Auto = 1, LO_120MHz = 2, LO_144MHz = 3, LO_168MHz = 4 };
enum MinGainReductionT : int { EXTENDED_MIN_GR = 0, NORMAL_MIN_GR = 20 };
enum TunerSelectT : int { Tuner_Neither = 0, Tuner_A = 1, Tuner_B = 2, Tuner_Both = 3 };

struct GainValuesT {   // sdrplay_api_GainValuesT
    float curr;
    float max;
    float min;
};
struct GainT {   // sdrplay_api_GainT
    int gRdB;                   // IF gain reduction, 20..59 dB (MAX_BB_GR 59)
    unsigned char LNAstate;
    unsigned char syncUpdate;
    MinGainReductionT minGr;
    GainValuesT gainVals;       // output
};
struct RfFreqT {   // sdrplay_api_RfFreqT
    double rfHz;
    unsigned char syncUpdate;
};
struct DcOffsetTunerT {   // sdrplay_api_DcOffsetTunerT
    unsigned char dcCal;
    unsigned char speedUp;
    int trackTime;
    int refreshRateTime;
};
struct TunerParamsT {   // sdrplay_api_TunerParamsT
    Bw_MHzT bwType;
    If_kHzT ifType;
    LoModeT loMode;
    GainT gain;
    RfFreqT rfFreq;
    DcOffsetTunerT dcOffsetTuner;
};

// ------------------------------------------------------------------ sdrplay_api_control.h
enum AgcControlT : int { AGC_DISABLE = 0, AGC_100HZ = 1, AGC_50HZ = 2, AGC_5HZ = 3, AGC_CTRL_EN = 4 };
enum AdsbModeT : int {
    ADSB_DECIMATION = 0, ADSB_NO_DECIMATION_LOWPASS = 1, ADSB_NO_DECIMATION_BANDPASS_2MHZ = 2, ADSB_NO_DECIMATION_BANDPASS_3MHZ = 3
};
struct DcOffsetT {   // sdrplay_api_DcOffsetT
    unsigned char DCenable;
    unsigned char IQenable;
};
struct DecimationT {   // sdrplay_api_DecimationT
    unsigned char enable;
    unsigned char decimationFactor;   // 1, 2, 4, 8, 16, 32
    unsigned char wideBandSignal;
};
struct AgcT {   // sdrplay_api_AgcT
    AgcControlT enable;
    int setPoint_dBfs;
    unsigned short attack_ms;
    unsigned short decay_ms;
    unsigned short decay_delay_ms;
    unsigned short decay_threshold_dB;
    int syncUpdate;
};
struct ControlParamsT {   // sdrplay_api_ControlParamsT
    DcOffsetT dcOffset;
    DecimationT decimation;
    AgcT agc;
    AdsbModeT adsbMode;
};

// ------------------------------------------------------------------ sdrplay_api_rsp1a.h, _rsp2.h, _rspDuo.h, _rspDx.h
struct Rsp1aParamsT {   // sdrplay_api_Rsp1aParamsT (also used for the RSP1B)
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};
struct Rsp1aTunerParamsT {   // sdrplay_api_Rsp1aTunerParamsT
    unsigned char biasTEnable;
};
enum Rsp2_AntennaSelectT : int { Rsp2_ANTENNA_A = 5, Rsp2_ANTENNA_B = 6 };
enum Rsp2_AmPortSelectT : int { Rsp2_AMPORT_1 = 1, Rsp2_AMPORT_2 = 0 };
struct Rsp2ParamsT {   // sdrplay_api_Rsp2ParamsT
    unsigned char extRefOutputEn;
};
struct Rsp2TunerParamsT {   // sdrplay_api_Rsp2TunerParamsT
    unsigned char biasTEnable;
    Rsp2_AmPortSelectT amPortSel;
    Rsp2_AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
};
enum RspDuoModeT : int {   // sdrplay_api_RspDuoModeT (a bit mask in DeviceT.rspDuoMode after GetDevices)
    RspDuoMode_Unknown = 0, RspDuoMode_Single_Tuner = 1, RspDuoMode_Dual_Tuner = 2, RspDuoMode_Master = 4, RspDuoMode_Slave = 8
};
enum RspDuo_AmPortSelectT : int { RspDuo_AMPORT_1 = 1, RspDuo_AMPORT_2 = 0 };
struct RspDuoParamsT {   // sdrplay_api_RspDuoParamsT
    int extRefOutputEn;
};
struct RspDuo_ResetSlaveFlagsT {   // sdrplay_api_RspDuo_ResetSlaveFlagsT (after 3.08)
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
};
struct RspDuoTunerParamsT {   // sdrplay_api_RspDuoTunerParamsT
    unsigned char biasTEnable;
    RspDuo_AmPortSelectT tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
    RspDuo_ResetSlaveFlagsT resetSlaveFlags;   // after 3.08
};
enum RspDx_AntennaSelectT : int { RspDx_ANTENNA_A = 0, RspDx_ANTENNA_B = 1, RspDx_ANTENNA_C = 2 };
enum RspDx_HdrModeBwT : int { RspDx_HDRMODE_BW_0_200 = 0, RspDx_HDRMODE_BW_0_500 = 1, RspDx_HDRMODE_BW_1_200 = 2, RspDx_HDRMODE_BW_1_700 = 3 };
struct RspDxParamsT {   // sdrplay_api_RspDxParamsT (also used for the RSPdx-R2)
    unsigned char hdrEnable;
    unsigned char biasTEnable;
    RspDx_AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};
struct RspDxTunerParamsT {   // sdrplay_api_RspDxTunerParamsT
    RspDx_HdrModeBwT hdrBw;
};

// ------------------------------------------------------------------ sdrplay_api_dev.h
enum TransferModeT : int { ISOCH = 0, BULK = 1 };
struct FsFreqT {   // sdrplay_api_FsFreqT
    double fsHz;                // ADC sample rate
    unsigned char syncUpdate;
    unsigned char reCal;
};
struct SyncUpdateT {   // sdrplay_api_SyncUpdateT
    unsigned int sampleNum;
    unsigned int period;
};
struct ResetFlagsT {   // sdrplay_api_ResetFlagsT
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
    unsigned char resetFsUpdate;
};
struct DevParamsT {   // sdrplay_api_DevParamsT
    double ppm;
    FsFreqT fsFreq;
    SyncUpdateT syncUpdate;
    ResetFlagsT resetFlags;
    TransferModeT mode;
    unsigned int samplesPerPkt;   // output
    Rsp1aParamsT rsp1aParams;
    Rsp2ParamsT rsp2Params;
    RspDuoParamsT rspDuoParams;
    RspDxParamsT rspDxParams;
};

// ------------------------------------------------------------------ sdrplay_api_rx_channel.h
struct RxChannelParamsT {   // sdrplay_api_RxChannelParamsT
    TunerParamsT tunerParams;
    ControlParamsT ctrlParams;
    Rsp1aTunerParamsT rsp1aTunerParams;
    Rsp2TunerParamsT rsp2TunerParams;
    RspDuoTunerParamsT rspDuoTunerParams;
    RspDxTunerParamsT rspDxTunerParams;
};

// ------------------------------------------------------------------ sdrplay_api.h (structs)
struct DeviceT {   // sdrplay_api_DeviceT
    char SerNo[kMaxSerNoLen];
    unsigned char hwVer;
    TunerSelectT tuner;
    RspDuoModeT rspDuoMode;
    unsigned char valid;          // 3.08 and newer
    double rspDuoSampleFreq;
    void* dev;                    // HANDLE
};
struct DeviceParamsT {   // sdrplay_api_DeviceParamsT (owned by the library)
    DevParamsT* devParams;        // null for an RSPduo slave
    RxChannelParamsT* rxChannelA;
    RxChannelParamsT* rxChannelB;
};
struct ErrorInfoT {   // sdrplay_api_ErrorInfoT
    char file[256];
    char function[256];
    int line;
    char message[1024];
};

// ------------------------------------------------------------------ sdrplay_api_callback.h
enum PowerOverloadCbEventIdT : int { Overload_Detected = 0, Overload_Corrected = 1 };
enum RspDuoModeCbEventIdT : int {
    MasterInitialised = 0, SlaveAttached = 1, SlaveDetached = 2, SlaveInitialised = 3, SlaveUninitialised = 4, MasterDllDisappeared = 5,
    SlaveDllDisappeared = 6
};
enum EventT : int { GainChange = 0, PowerOverloadChange = 1, DeviceRemoved = 2, RspDuoModeChange = 3, DeviceFailure = 4 };   // DeviceFailure: 3.15
struct GainCbParamT {   // sdrplay_api_GainCbParamT
    unsigned int gRdB;
    unsigned int lnaGRdB;
    double currGain;
};
struct PowerOverloadCbParamT {   // sdrplay_api_PowerOverloadCbParamT
    PowerOverloadCbEventIdT powerOverloadChangeType;
};
struct RspDuoModeCbParamT {   // sdrplay_api_RspDuoModeCbParamT
    RspDuoModeCbEventIdT modeChangeType;
};
union EventParamsT {   // sdrplay_api_EventParamsT
    GainCbParamT gainParams;
    PowerOverloadCbParamT powerOverloadParams;
    RspDuoModeCbParamT rspDuoModeParams;
};
struct StreamCbParamsT {   // sdrplay_api_StreamCbParamsT
    unsigned int firstSampleNum;
    int grChanged;
    int rfChanged;
    int fsChanged;
    unsigned int numSamples;
};
}   // namespace sdrplay
}   // namespace dect2

// the calling convention comes from the includer (DECT2_CALL in native_common.h; empty for the test's fake library)
#ifndef DECT2_SDRPLAY_CALL
#ifdef _WIN32
#define DECT2_SDRPLAY_CALL __cdecl
#else
#define DECT2_SDRPLAY_CALL
#endif
#endif

namespace dect2 {
namespace sdrplay {
typedef void (DECT2_SDRPLAY_CALL* StreamCallback_t)(short* xi, short* xq, StreamCbParamsT* params, unsigned int numSamples, unsigned int reset, void* cbContext);
typedef void (DECT2_SDRPLAY_CALL* EventCallback_t)(EventT eventId, TunerSelectT tuner, EventParamsT* params, void* cbContext);
struct CallbackFnsT {   // sdrplay_api_CallbackFnsT
    StreamCallback_t StreamACbFn;
    StreamCallback_t StreamBCbFn;
    EventCallback_t EventCbFn;
};

// ------------------------------------------------------------------ layout checks
// Sizes and offsets worked out by hand from the official 3.15 headers (natural alignment, 4-byte enums, 8-byte pointers and doubles).
// Checked against the official headers themselves with both compilers (clang for macOS, MinGW g++ for Windows) when this file was written.
static_assert(sizeof(ErrT) == 4 && sizeof(ReasonForUpdateT) == 4 && sizeof(ReasonForUpdateExtension1T) == 4 && sizeof(Bw_MHzT) == 4 &&
              sizeof(If_kHzT) == 4 && sizeof(TunerSelectT) == 4 && sizeof(RspDuoModeT) == 4 && sizeof(EventT) == 4, "C enums are 4 bytes");
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(sizeof(GainValuesT) == 12, "GainValuesT");
static_assert(sizeof(GainT) == 24 && offsetof(GainT, LNAstate) == 4 && offsetof(GainT, minGr) == 8 && offsetof(GainT, gainVals) == 12, "GainT");
static_assert(sizeof(RfFreqT) == 16 && offsetof(RfFreqT, syncUpdate) == 8, "RfFreqT");
static_assert(sizeof(DcOffsetTunerT) == 12 && offsetof(DcOffsetTunerT, trackTime) == 4, "DcOffsetTunerT");
static_assert(sizeof(TunerParamsT) == 72 && offsetof(TunerParamsT, ifType) == 4 && offsetof(TunerParamsT, loMode) == 8 &&
              offsetof(TunerParamsT, gain) == 12 && offsetof(TunerParamsT, rfFreq) == 40 && offsetof(TunerParamsT, dcOffsetTuner) == 56, "TunerParamsT");
static_assert(sizeof(DecimationT) == 3 && sizeof(AgcT) == 20 && offsetof(AgcT, attack_ms) == 8 && offsetof(AgcT, syncUpdate) == 16, "AgcT");
static_assert(sizeof(ControlParamsT) == 32 && offsetof(ControlParamsT, decimation) == 2 && offsetof(ControlParamsT, agc) == 8 &&
              offsetof(ControlParamsT, adsbMode) == 28, "ControlParamsT");
static_assert(sizeof(Rsp2TunerParamsT) == 16 && offsetof(Rsp2TunerParamsT, rfNotchEnable) == 12, "Rsp2TunerParamsT");
static_assert(sizeof(RspDuoTunerParamsT) == 16 && offsetof(RspDuoTunerParamsT, tuner1AmNotchEnable) == 8 &&
              offsetof(RspDuoTunerParamsT, resetSlaveFlags) == 11, "RspDuoTunerParamsT");
static_assert(sizeof(RspDxParamsT) == 12 && offsetof(RspDxParamsT, antennaSel) == 4 && offsetof(RspDxParamsT, rfNotchEnable) == 8, "RspDxParamsT");
static_assert(sizeof(RxChannelParamsT) == 144 && offsetof(RxChannelParamsT, ctrlParams) == 72 && offsetof(RxChannelParamsT, rsp1aTunerParams) == 104 &&
              offsetof(RxChannelParamsT, rsp2TunerParams) == 108 && offsetof(RxChannelParamsT, rspDuoTunerParams) == 124 &&
              offsetof(RxChannelParamsT, rspDxTunerParams) == 140, "RxChannelParamsT");
static_assert(sizeof(FsFreqT) == 16 && sizeof(SyncUpdateT) == 8 && sizeof(ResetFlagsT) == 3, "FsFreqT, SyncUpdateT, ResetFlagsT");
static_assert(sizeof(DevParamsT) == 64 && offsetof(DevParamsT, fsFreq) == 8 && offsetof(DevParamsT, syncUpdate) == 24 &&
              offsetof(DevParamsT, resetFlags) == 32 && offsetof(DevParamsT, mode) == 36 && offsetof(DevParamsT, samplesPerPkt) == 40 &&
              offsetof(DevParamsT, rsp1aParams) == 44 && offsetof(DevParamsT, rsp2Params) == 46 && offsetof(DevParamsT, rspDuoParams) == 48 &&
              offsetof(DevParamsT, rspDxParams) == 52, "DevParamsT");
static_assert(sizeof(DeviceT) == 96 && offsetof(DeviceT, hwVer) == 64 && offsetof(DeviceT, tuner) == 68 && offsetof(DeviceT, rspDuoMode) == 72 &&
              offsetof(DeviceT, valid) == 76 && offsetof(DeviceT, rspDuoSampleFreq) == 80 && offsetof(DeviceT, dev) == 88, "DeviceT");
static_assert(sizeof(DeviceParamsT) == 24 && offsetof(DeviceParamsT, rxChannelA) == 8 && offsetof(DeviceParamsT, rxChannelB) == 16, "DeviceParamsT");
static_assert(sizeof(ErrorInfoT) == 1540 && offsetof(ErrorInfoT, line) == 512 && offsetof(ErrorInfoT, message) == 516, "ErrorInfoT");
static_assert(sizeof(GainCbParamT) == 16 && offsetof(GainCbParamT, currGain) == 8 && sizeof(EventParamsT) == 16, "EventParamsT");
static_assert(sizeof(StreamCbParamsT) == 20 && offsetof(StreamCbParamsT, numSamples) == 16, "StreamCbParamsT");
static_assert(sizeof(CallbackFnsT) == 24 && offsetof(CallbackFnsT, StreamBCbFn) == 8 && offsetof(CallbackFnsT, EventCbFn) == 16, "CallbackFnsT");
#endif

}   // namespace sdrplay
}   // namespace dect2
