// Packet builders of the test signals (xband_gen.cpp), next to the decoders in xband_instr.cpp.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::xband {

int viirsCount();
const char* viirsName(int c);
void viirsChannelInfo(int c, int& apid, int& zh, const int*& zw, const int*& os, int& ref, int& scale, int& decim);
std::vector<uint8_t> viirsBuildSegmentHeader(const uint8_t time[8], int nPackets);
std::vector<uint8_t> viirsBuildBody(int det, uint32_t syncPat, const std::vector<std::vector<uint16_t>>& zones);

void ocmBuildFrame(std::vector<uint8_t>& f, int line);
void modisDayPacket(int scan, int pos1, bool partTwo, std::vector<uint8_t>& p, uint16_t (*pix)(int ch, int x, int y), int line);

} // namespace dect2::xband
