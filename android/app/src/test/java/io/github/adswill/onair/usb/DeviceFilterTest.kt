// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class DeviceFilterTest {
    /** The filter file of the app (Gradle runs unit tests in the module folder, android/app). */
    private fun filterXml(): String {
        val f = listOf("src/main/res/xml/usb_device_filter.xml", "app/src/main/res/xml/usb_device_filter.xml").map(::File).first { it.exists() }
        return f.readText()
    }

    @Test fun parsesDecimalHexAndSkipsComments() {
        val p = DeviceFilter.parse(
            """
            <resources>
              <!-- <usb-device vendor-id="1" product-id="2" /> commented out -->
              <usb-device vendor-id="3034" product-id="10296" />
              <usb-device vendor-id="0x1d50" product-id="0x6089"/>
              <usb-device vendor-id="7504" />
              <usb-device vendor-id="abc" product-id="1" />
            </resources>
            """.trimIndent(),
        )
        assertEquals(listOf(UsbCatalog.UsbId(0x0bda, 0x2838), UsbCatalog.UsbId(0x1d50, 0x6089)), p.ids)
        assertEquals(1, p.wildcards.size)   // a vendor-only entry would match every product of the vendor
        assertEquals(1, p.errors.size)
    }

    @Test fun numbers() {
        assertEquals(0x0bda, DeviceFilter.parseNumber("3034"))
        assertEquals(0x0bda, DeviceFilter.parseNumber("0x0BDA"))
        assertNull(DeviceFilter.parseNumber("70000"))
        assertNull(DeviceFilter.parseNumber("-1"))
        assertNull(DeviceFilter.parseNumber(""))
    }

    @Test fun appFilterMatchesTheCatalogExactly() {
        val p = DeviceFilter.parse(filterXml())
        assertTrue("no wildcard entries: ${p.wildcards}", p.wildcards.isEmpty())
        assertTrue("unreadable entries: ${p.errors}", p.errors.isEmpty())
        assertEquals("no duplicates", p.ids.size, p.ids.toSet().size)
        assertEquals(UsbCatalog.all.keys, p.ids.toSet())
    }

    @Test fun everyFamilyIsInTheFilterAndNoSdrplay() {
        val ids = DeviceFilter.parse(filterXml()).ids.toSet()
        for (f in UsbCatalog.Family.entries) assertTrue(f.name, UsbCatalog.all.any { it.value == f && it.key in ids })
        assertTrue(ids.none { it.vid == 0x1df7 })   // SDRplay: closed API, not on Android
    }

    @Test fun identifiesModels() {
        assertEquals("RTL-SDR Blog V4", UsbCatalog.identify(0x0bda, 0x2838, "RTLSDRBlog", "Blog V4")?.name)
        assertEquals("RTL-SDR Blog V4L", UsbCatalog.identify(0x0bda, 0x2838, "RTLSDRBlog", "Blog V4L")?.name)
        assertEquals("RTL-SDR Blog V3", UsbCatalog.identify(0x0bda, 0x2838, "RTLSDRBlog", "Blog V3")?.name)
        assertEquals("RTL-SDR (R820T)", UsbCatalog.identify(0x0bda, 0x2838, "Realtek", "RTL2838UHIDIR")?.name)
        assertEquals("RTL-SDR (R820T)", UsbCatalog.identify(0x0bda, 0x2838, null, null)?.name)   // before the permission
        assertEquals("HackRF One", UsbCatalog.identify(0x1d50, 0x6089, "Great Scott Gadgets", "HackRF One")?.name)
        assertEquals("HackRF Pro", UsbCatalog.identify(0x1d50, 0x6089, "Great Scott Gadgets", "HackRF Pro")?.name)
        assertEquals(UsbCatalog.Family.AIRSPY, UsbCatalog.identify(0x1d50, 0x60a1, "Airspy", "AIRSPY")?.family)
        assertEquals("Airspy HF+", UsbCatalog.identify(0x03eb, 0x800c, "Airspy", "AIRSPY HF+")?.name)
        assertNull(UsbCatalog.identify(0x1df7, 0x3000, "SDRplay", "RSP1A"))
        assertNull(UsbCatalog.identify(0x046d, 0xc52b, "Logitech", "Receiver"))
    }

    @Test fun powerWarnings() {
        val hackrf = UsbCatalog.identify(0x1d50, 0x6089, null, "HackRF One")!!
        assertTrue(hackrf.powerWarning.contains("powered USB hub"))
        val rtl = UsbCatalog.identify(0x0bda, 0x2838, null, null)!!
        assertEquals("", rtl.powerWarning)
        assertNotNull(UsbCatalog.Family.ofKind("airspyhf"))
    }
}
