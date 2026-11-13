# Credits

Short version: [README, Credits](../README.md#credits-and-contributors).

OnAir stands on the work of many people. Full list with authors, licences and what each licence asks of us: **[THIRD_PARTY.md](../THIRD_PARTY.md)** (also in the program: Help, Credits and licences). The main ones:

- **Code in the program:** [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut), [miniaudio](https://github.com/mackron/miniaudio) (David Reid), [codec2](https://github.com/drowe67/codec2) (David Rowe, LGPL-2.1, a separate library loaded at run time, see [docs/CODECS.md](CODECS.md)), `dump978` UAT code (Oliver Jowett), the DRA decoder (maliwen2015), the RX888 firmware (Oscar Steila, SDDC_Driver).
- **Decoders written from or ported from open-source receivers:** [nrsc5](https://github.com/theori-io/nrsc5) (HD Radio), [etisnoop](https://github.com/Opendigitalradio/etisnoop) (DAB), [WSJT-X](https://sourceforge.net/projects/wsjt/), JS8Call and fldigi (HF digital modes), [rtl_433](https://github.com/merbanan/rtl_433), [gr-dvbt2](https://github.com/drmpeg/gr-dvbt2), [gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr), [e-Czas](https://github.com/sp5wwp/e-Czas), [JAERO](https://github.com/jontio/JAERO), acarsdec, libacars, dumphfdl, dumpvdl2, iridium-toolkit, multimon-ng, Meshtastic, MeshCore, MeshCom.
- **Libraries it uses:** [FFmpeg](https://ffmpeg.org), [GLFW](https://www.glfw.org), [libusb](https://libusb.info), [libhackrf](https://github.com/greatscottgadgets/hackrf), [Hamlib](https://hamlib.github.io), SoapySDR.
- **Icom IC-R8600 I/Q driver:** written again in C++ from the MIT work of **Topol in collaboration with AI** (Pawel "Topol" Topolski, the first working SoapySDR module for macOS) and [Pieter Ibelings](https://github.com/ibelinp/ic-r8600-usb-iq) (ic-r8600-usb-iq, MIT). Icom's firmware is Icom's and is not included. Not affiliated with Icom.
- **Data:** map tiles and data (c) [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, aircraft data from adsbdb, aircraft photos from Planespotters.net with credit, satellite elements from CelesTrak.

- **0.2.8 additions:** the DVB-S / DecSAT work uses code by **Arclamp** (GPL-3.0-or-later); the mesh clock fix is PR #217 by [@ffishman](https://github.com/ffishman); the recommended source for the TETRA and Yaesu Fusion voice plug-ins is [mbelib-neo](https://github.com/arancormonk/mbelib-neo) by arancormonk (GPL-2.0-or-later, not bundled); the NRSC-5 patch for HD Radio sound is from nrsc5 with [FAAD2](https://github.com/knik0/faad2). The KA modes are Kristijonas Adomaitis's own designs.

If your work is used and not credited correctly, please open an issue.
