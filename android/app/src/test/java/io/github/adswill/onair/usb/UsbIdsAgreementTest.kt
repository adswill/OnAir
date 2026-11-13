// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The third copy of the radio table: usbFdKindForId() in the core (core/src/source/usb_fd.cpp), which the drivers use to pick a
 * descriptor's driver. DeviceFilterTest checks the filter against UsbCatalog; this checks the core's table against UsbCatalog, so all
 * three agree (an id in the filter that the core does not know would open OnAir and then say "no driver").
 */
class UsbIdsAgreementTest {
    private fun coreTable(): Map<UsbCatalog.UsbId, String> {
        val f = listOf("../../core/src/source/usb_fd.cpp", "../core/src/source/usb_fd.cpp", "core/src/source/usb_fd.cpp").map(::File).first { it.exists() }
        val src = f.readText()
        val body = src.substring(src.indexOf("std::string usbFdKindForId"), src.indexOf("bool usbFdPlatform"))
        val m = Regex("""\{0x([0-9a-fA-F]{4}), 0x([0-9a-fA-F]{4}), "(\w+)"\}""").findAll(body)
            .map { UsbCatalog.UsbId(it.groupValues[1].toInt(16), it.groupValues[2].toInt(16)) to it.groupValues[3] }.toList()
        assertEquals("no id twice in the core's table", m.size, m.map { it.first }.toSet().size)
        return m.toMap()
    }

    @Test fun coreKnowsExactlyTheCatalogsIdsWithTheSameDriver() {
        val core = coreTable()
        assertEquals(UsbCatalog.all.keys, core.keys)
        for ((id, fam) in UsbCatalog.all) assertEquals("$id", fam.kind, core[id])
    }

    @Test fun everyLibrtlsdrIdIsListed() {
        // librtlsdr's known_devices has 42 entries (osmocom rtl-sdr, src/librtlsdr.c)
        assertEquals(42, UsbCatalog.rtlIds.size)
        assertTrue(UsbCatalog.family(0x1b80, 0xd3a8) == UsbCatalog.Family.RTLSDR)   // ASUS U3100MINI_PLUS_V2
        assertTrue(UsbCatalog.identify(0x0ccd, 0x00d7, "Terratec", "T Stick PLUS") != null)
    }
}
