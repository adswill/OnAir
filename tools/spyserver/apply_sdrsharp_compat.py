#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Apply SDR#-style SpyServer session setup to a SoapySpyServer source checkout.

Run from the OnAir repository:
  python tools/spyserver/apply_sdrsharp_compat.py ~/SoapySpyServer

The upstream plugin establishes a much shorter session than SDR#. This patch
adds explicit FFT+IQ session initialization before enabling streaming.
It is a compatibility candidate, not a verified resolution.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else Path.home() / "SoapySpyServer"
client_path = root / "3rdparty/SDRPlusPlus/spyserver_client.cpp"
settings_path = root / "Settings.cpp"
protocol_path = root / "3rdparty/SDRPlusPlus/spyserver_protocol.h"
for p in (client_path, settings_path, protocol_path):
    if not p.is_file():
        raise SystemExit("File not found: " + str(p))

client = client_path.read_text()
settings = settings_path.read_text()
protocol = protocol_path.read_text()

# Use the same tested protocol level as the working SDR# client.
protocol = protocol.replace("((0) << 16) | (1700)", "((0) << 16) | (1592)")

# SpyServer's capabilities come from its sync response; a command should be
# checked against a *new* response, never the first greeting.
old_wait = """        clientSyncCnd.wait_until(lck, now + std::chrono::milliseconds(timeoutMS), [this]() { return clientSyncAvailable; });
        return clientSyncAvailable;"""
new_wait = """        const bool received = clientSyncCnd.wait_until(
            lck, now + std::chrono::milliseconds(timeoutMS),
            [this]() { return clientSyncAvailable; });
        if (received) clientSyncAvailable = false;
        return received;"""
if old_wait in client:
    client = client.replace(old_wait, new_wait, 1)

# Match SDR# setup: setting only STREAMING_ENABLED is insufficient on servers
# which require FFT+IQ stream metadata and format parameters.
start = client.index("    void SpyServerClientClass::startStream() {")
end = client.index("    void SpyServerClientClass::stopStream()", start)
new_start = """    void SpyServerClientClass::startStream() {
        // SDR#-compatible SpyServer session configuration.
        setSetting(SPYSERVER_SETTING_STREAMING_MODE, SPYSERVER_STREAM_MODE_FFT_IQ);
        setSetting(SPYSERVER_SETTING_FFT_DISPLAY_PIXELS, 1000);
        setSetting(SPYSERVER_SETTING_FFT_DB_OFFSET, 0);
        setSetting(SPYSERVER_SETTING_FFT_DB_RANGE, 127);
        setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, UINT32_MAX);
        setSetting(SPYSERVER_SETTING_FFT_FORMAT, SPYSERVER_STREAM_FORMAT_UINT8);
        setSetting(SPYSERVER_SETTING_IQ_FORMAT, SPYSERVER_STREAM_FORMAT_UINT8);
        // Sample rate/decimation already selected by Settings.cpp; do not
        // hardcode a decimation index, because devices differ.
        setSetting(SPYSERVER_SETTING_FFT_DECIMATION, 0);
        setSetting(SPYSERVER_SETTING_STREAMING_ENABLED, true);
    }

"""
client = client[:start] + new_start + client[end:]

# Apply both frequency settings in the same order as the successful SDR# trace.
# If the user has already made this modification, do not duplicate it.
start = settings.index("void SoapySpyServerClient::setFrequency(")
end = settings.index("double SoapySpyServerClient::getFrequency(", start)
section = settings[start:end]
if "SPYSERVER_SETTING_FFT_FREQUENCY" not in section:
    needle = """        _sdrppClient.syncFields();"""
    addition = """        _sdrppClient.client->setSetting(
            static_cast<uint32_t>(SPYSERVER_SETTING_FFT_FREQUENCY),
            static_cast<uint32_t>(frequency));

"""
    if needle not in section:
        raise SystemExit("Expected frequency-setting block not found; no files changed.")
    section = section.replace(needle, addition + needle, 1)
    settings = settings[:start] + section + settings[end:]

for path, before, after in (
    (client_path, client_path.read_text(), client),
    (settings_path, settings_path.read_text(), settings),
    (protocol_path, protocol_path.read_text(), protocol),
):
    if before != after:
        backup = path.with_suffix(path.suffix + ".onair-backup")
        if not backup.exists():
            backup.write_text(before)
        path.write_text(after)
        print("Updated:", path)
    else:
        print("Already updated:", path)
print("Build and install SoapySpyServer, then copy DLL into OnAir's module directory.")
