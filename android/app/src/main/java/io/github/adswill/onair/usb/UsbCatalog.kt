// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

/**
 * The USB radios OnAir drives on a phone, by USB id: the same table as res/xml/usb_device_filter.xml (the "always open with OnAir"
 * filter; DeviceFilterTest checks the two agree) and as usbFdKindForId() in the core (core/src/source/usb_fd.cpp).
 * Pure Kotlin: no Android classes, so it is tested on the JVM.
 *
 * SDRplay radios are not here, except the RSP1 (1df7:2500): the SDRplay API is a closed binary service that does not exist for Android,
 * but the RSP1 is a Mirics MSi2500 + MSi001 pair that OnAir drives itself (core/src/source/mirics_usb.cpp, untested on hardware, #503).
 */
object UsbCatalog {
    /** The driver families; [kind] is the core's driver name (UsbFdRadio::kind). */
    enum class Family(val kind: String, val label: String) {
        RTLSDR("rtlsdr", "RTL-SDR"), HACKRF("hackrf", "HackRF"), AIRSPY("airspy", "Airspy"), AIRSPYHF("airspyhf", "Airspy HF+"),
        MIRICS("mirics", "SDRplay RSP1 / Mirics");

        companion object { fun ofKind(k: String): Family? = entries.firstOrNull { it.kind == k } }
    }

    data class UsbId(val vid: Int, val pid: Int) {
        override fun toString(): String = "%04x:%04x".format(vid, pid)
    }

    /** One model: what the phone can expect of it before it was opened (the driver's own report replaces these: RadioInfo). */
    data class Model(
        val name: String,
        val family: Family,
        val minFreqHz: Double,
        val maxFreqHz: Double,
        /** The highest sample rate that streams without loss on a phone's USB 2 port. */
        val maxRateHz: Double,
        val biasTee: Boolean,
        /** About what it draws from the port, mA (makers' figures and measurements; a phone's OTG port often gives 500 mA or less). */
        val typicalMa: Int,
        val notes: String = "",
    ) {
        /** A plain-language warning about power when the radio draws a lot for a phone; "" when it is light. */
        val powerWarning: String get() = when {
            typicalMa >= 400 -> "$name draws about $typicalMa mA. Many phones give less than that over OTG: if it disconnects or the stream stops, " +
                "use a powered USB hub or an OTG Y cable with its own power."
            typicalMa >= 280 -> "$name draws about $typicalMa mA: fine on most phones; with a weak battery or a long cable, a powered hub helps."
            else -> ""
        }
    }

    // RTL2832U ids: every id of librtlsdr's known_devices list (osmocom rtl-sdr src/librtlsdr.c): the Realtek ones (generic sticks,
    // RTL-SDR Blog V3 / V4 / V4L, NooElec NESDR) and the DVB-T / DAB sticks sold under other names (Terratec, Compro, KWorld ...)
    val rtlIds: List<UsbId> = listOf(
        UsbId(0x0bda, 0x2832), UsbId(0x0bda, 0x2838),
        UsbId(0x0413, 0x6680), UsbId(0x0413, 0x6f0f),
        UsbId(0x0458, 0x707f),
        UsbId(0x0ccd, 0x00a9), UsbId(0x0ccd, 0x00b3), UsbId(0x0ccd, 0x00b4), UsbId(0x0ccd, 0x00b5),
        UsbId(0x0ccd, 0x00b7), UsbId(0x0ccd, 0x00b8), UsbId(0x0ccd, 0x00b9), UsbId(0x0ccd, 0x00c0),
        UsbId(0x0ccd, 0x00c6), UsbId(0x0ccd, 0x00d3), UsbId(0x0ccd, 0x00d7), UsbId(0x0ccd, 0x00e0),
        UsbId(0x1554, 0x5020),
        UsbId(0x15f4, 0x0131), UsbId(0x15f4, 0x0133),
        UsbId(0x185b, 0x0620), UsbId(0x185b, 0x0650), UsbId(0x185b, 0x0680),
        UsbId(0x1b80, 0xd393), UsbId(0x1b80, 0xd394), UsbId(0x1b80, 0xd395), UsbId(0x1b80, 0xd397),
        UsbId(0x1b80, 0xd398), UsbId(0x1b80, 0xd39d), UsbId(0x1b80, 0xd3a4), UsbId(0x1b80, 0xd3a8),
        UsbId(0x1b80, 0xd3af), UsbId(0x1b80, 0xd3b0),
        UsbId(0x1d19, 0x1101), UsbId(0x1d19, 0x1102), UsbId(0x1d19, 0x1103), UsbId(0x1d19, 0x1104),
        UsbId(0x1f4d, 0xa803), UsbId(0x1f4d, 0xb803), UsbId(0x1f4d, 0xc803), UsbId(0x1f4d, 0xd286),
        UsbId(0x1f4d, 0xd803),
    )
    val hackrfIds: List<UsbId> = listOf(UsbId(0x1d50, 0x6089), UsbId(0x1d50, 0x604b), UsbId(0x1d50, 0xcc15))
    val airspyIds: List<UsbId> = listOf(UsbId(0x1d50, 0x60a1))
    val airspyHfIds: List<UsbId> = listOf(UsbId(0x03eb, 0x800c))
    /** MSi2500 + MSi001: the SDRplay RSP1 (and its clones), then the TV sticks with the same chips (libmirisdr's list, without the RSP1A / RSP2). */
    val miricsIds: List<UsbId> = listOf(UsbId(0x1df7, 0x2500), UsbId(0x2040, 0xd300), UsbId(0x07ca, 0x8591), UsbId(0x04bb, 0x0537), UsbId(0x0511, 0x0037))

    /** Every supported id with its family. */
    val all: Map<UsbId, Family> = buildMap {
        rtlIds.forEach { put(it, Family.RTLSDR) }
        hackrfIds.forEach { put(it, Family.HACKRF) }
        airspyIds.forEach { put(it, Family.AIRSPY) }
        airspyHfIds.forEach { put(it, Family.AIRSPYHF) }
        miricsIds.forEach { put(it, Family.MIRICS) }
    }

    fun family(vid: Int, pid: Int): Family? = all[UsbId(vid, pid)]

    private val rtlGeneric = Model("RTL-SDR (R820T)", Family.RTLSDR, 24e6, 1766e6, 2.4e6, false, 270)

    /**
     * The model of a radio from its USB id and the strings the phone read ([manufacturer], [product] may be null when Android could
     * not read them, e.g. before the permission). null: not a radio OnAir drives.
     */
    fun identify(vid: Int, pid: Int, manufacturer: String?, product: String?): Model? {
        val f = family(vid, pid) ?: return null
        val p = (product ?: "").trim()
        val m = (manufacturer ?: "").trim()
        val pl = p.lowercase()
        return when (f) {
            Family.RTLSDR -> when {
                // RTL-SDR Blog: V4 (R828D, HF through its built-in upconverter), V4L (the low-power V4), V3 (R820T2, HF by direct sampling)
                pl.contains("v4l") -> Model("RTL-SDR Blog V4L", f, 0.5e6, 1766e6, 2.4e6, true, 200, "Low-power V4: HF through its upconverter")
                pl.contains("blog v4") || pl == "v4" -> Model("RTL-SDR Blog V4", f, 0.5e6, 1766e6, 2.4e6, true, 300, "R828D tuner; HF through its built-in upconverter")
                pl.contains("blog v3") || (m.contains("rtl-sdr blog", true) && !pl.contains("v4")) ->
                    Model("RTL-SDR Blog V3", f, 0.5e6, 1766e6, 2.4e6, true, 270, "HF (below 24 MHz) by direct sampling on the Q branch")
                pl.contains("nesdr") || m.contains("nooelec", true) -> Model("NooElec ${p.ifEmpty { "NESDR" }}", f, 24e6, 1766e6, 2.4e6, pl.contains("smart"), 280)
                p.isNotEmpty() && !pl.startsWith("rtl2838") && !pl.startsWith("rtl2832") -> rtlGeneric.copy(name = "RTL-SDR ($p)")
                else -> rtlGeneric
            }
            Family.HACKRF -> when {
                // the One and the Pro share the USB id; the board id read when it is opened tells them apart (RadioInfo.board)
                pl.contains("pro") -> Model("HackRF Pro", f, 0.1e6, 6000e6, 20e6, true, 500)
                pid == 0xcc15 -> Model("rad1o", f, 50e6, 4000e6, 20e6, false, 400)
                pid == 0x604b -> Model("HackRF Jawbreaker", f, 10e6, 6000e6, 20e6, false, 450)
                else -> Model("HackRF One", f, 1e6, 6000e6, 20e6, true, 500, "One or Pro: the board says which once opened")
            }
            Family.AIRSPY -> when {
                pl.contains("mini") -> Model("Airspy Mini", f, 24e6, 1700e6, 6e6, true, 260)
                else -> Model("Airspy R2 / Mini", f, 24e6, 1800e6, 10e6, true, 400, "R2 or Mini: the firmware says which once opened")
            }
            Family.AIRSPYHF -> Model(if (pl.contains("discovery")) "Airspy HF+ Discovery" else "Airspy HF+", f, 9e3, 260e6, 768e3, false, 300,
                "HF 9 kHz - 31 MHz and VHF 60 - 260 MHz")
            Family.MIRICS -> when (pid) {
                0x2500 -> Model("SDRplay RSP1 / MSi2500 (untested)", f, 10e3, 2000e6, 8.064e6, false, 200,
                    "OnAir's own driver, untested on hardware: please report whether it works (issue #503)")
                else -> Model("${p.ifEmpty { "Mirics MSi2500 stick" }} (untested)", f, 10e3, 2000e6, 8.064e6, false, 200,
                    "MSi2500 + MSi001 TV stick; OnAir's own driver, untested on hardware")
            }
        }
    }
}
