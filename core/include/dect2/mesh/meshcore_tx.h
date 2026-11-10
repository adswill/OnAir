// MeshCore transmit (TX mode "meshcore"): the LoRa frames of MeshCore packets that the MeshCore device screen (app/mesh/meshcore_ui.cpp)
// built with the node (meshcore_node.h), sent once each, then the generator ends. Nothing is refused: the radio settings come from r.opt
// (meshcore.sf, meshcore.bwHz, meshcore.cr, meshcore.preamble; defaults: the EU/UK Narrow preset, SF8, 62.5 kHz, 4/8, preamble 32) with
// MeshCore's sync word 0x12 and CRC on. meshcore.ifHz places the frames that far from 0 Hz in the samples (the radio is tuned the other
// way by the caller; default -meshTuning().tuneOffsetHz, where the Transmit window puts a Mesh signal), meshcore.gapSec is the silence between frames.
// The payload (TxRequest::payload) holds the frames one after the other, each led by its length byte (meshCoreTxPayload). An empty payload
// with a typed text sends that text on the Public channel as "<name>: text" (r.name, default "OnAir").
#pragma once
#include "dect2/tx/tx.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

std::string meshCoreTxPayload(const std::vector<std::vector<uint8_t>>& frames);
bool meshCoreTxFrames(const std::string& payload, std::vector<std::vector<uint8_t>>& frames);   // false: a length runs past the end
std::unique_ptr<TxGenerator> makeMeshCoreTxGenerator(const TxRequest& r, double rateHz, std::string& err);

} // namespace dect2
