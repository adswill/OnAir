// ATSC 3.0 test signal: the generator the synthetic source plays, so that the ATSC 3.0 receiver can be tried without a radio or a recording.
// Nothing is transmitted. One service ("ONAIR", 7.1) is made from nothing but code: a looping test card with a 1 kHz (left) and 3 kHz (right) tone,
// encoded in memory with libav (HEVC when the linked FFmpeg has an HEVC encoder, else H.264, else MPEG-2 video; AAC audio) and cut into fragmented MP4,
// one fragment per second. The fragments go round and round through the transmitter chain of this project: ROUTE (S-TSID, MPD and the init and
// media segments of both components) in UDP/IP, LLS (the service list table), ALP, baseband packets, BICM (BCH, LDPC, interleavers, constellation),
// and OFDM frames (bootstrap, Preamble with L1-Basic and L1-Detail, one subframe with one PLP), one frame after the other at a native
// 6.144 Msamples/s. Then the channel: carrier offset, clock offset, echo, noise, and resampling to the requested sample rate.
//
// The signal is endless and continuous, real time at any chunk size; the media time of the fragments keeps counting up (it never jumps back at the
// end of the 8 s loop). Peak below about 0.9, rms about 0.22.
//
// SynthConfig fields used:
//   snrDb     signal to noise power ratio in dB, measured in the 6.144 MHz of the native sample rate (the noise density does not change with the
//             output sample rate); 150 and more: no noise. The noise comes from a table of Gaussian values.
//   cfoHz     carrier frequency offset in Hz
//   sroPpm    sample clock offset of the "radio" in ppm (positive: its clock is fast, the signal looks stretched in time)
//   echoDb    0: no echo. Otherwise one echo, attenuated by this many dB (the sign does not matter), at echoDelay samples (output rate) with a fixed phase
//   modeOpt[0]  modulation of the PLP: 0 QPSK (default), 1 16QAM, 2 64QAM, 3 256QAM
//   modeOpt[1]  LDPC code rate: 0 default (8/15); 1..12 = 2/15, 3/15, 4/15, 5/15, 6/15, 7/15, 8/15, 9/15, 10/15, 11/15, 12/15, 13/15
//   modeOpt[2]  FEC frame: 0 BCH + 64800 bit LDPC (default), 1 BCH + 16200 bit LDPC
//   modeOpt[3]  guard interval (8192 point FFT): 0 default (1/8: 1024 samples), else the L1D_guard_interval code 1..12 (192, 384, 512, 768, 1024,
//               1536, 2048, 2432, 3072, 3648, 4096, 4864 samples). The scattered pilots are SP6_2 for all of them
//   modeOpt[4]  video encoder: 0 automatic (default: HEVC, then H.264, then MPEG-2 video: the first one the linked FFmpeg has), 1 HEVC, 2 H.264, 3 MPEG-2
//               video (without that encoder the signal falls back to the automatic choice)
//   modeOpt[5]  FEC mode of L1-Detail, 2 to 7: 0 default (2, QPSK, the most robust one that fits the single Preamble symbol), the others are 16QAM and 64QAM
//   modeVal[0]  video bit rate in bit/s: 0 default (700000, or less when the PLP is too slow to carry it twice over; MPEG-2 gets twice the rate)
// The defaults (QPSK, 8/15, 64K, 1/8) lock in the receiver from an SNR of about 8 dB, and the generator runs at five to seven times real time on one core of a laptop.
// Bit rate of the whole service: about 1 Mbit/s of ROUTE traffic.
#pragma once
#include "mode_synth.h"
#include <memory>

namespace dect2 {

// The generator of the ATSC 3.0 test signal at the given sample rate (any rate from 6.144 Msps; the app uses 8 to 10 Msps), or nullptr when the
// programme cannot be encoded (the linked FFmpeg has no usable video or audio encoder or no MP4 muxer).
std::unique_ptr<ModeSynth> makeAtsc3Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
