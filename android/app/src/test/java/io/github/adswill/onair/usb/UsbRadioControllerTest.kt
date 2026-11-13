// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The permission / open / hot-plug flow and the shape of the open-by-descriptor API, with a fake UsbManager and a fake core. */
class UsbRadioControllerTest {
    private class FakeConn(override val fd: Int, override val serial: String?, val log: MutableList<String>) : OpenedUsb {
        var closed = false
        override fun close() { closed = true; log += "close $fd" }
    }

    private class FakeBackend(val log: MutableList<String>) : UsbBackend {
        override var hostSupported = true
        val plugged = LinkedHashMap<String, UsbDeviceRef>()
        val granted = HashSet<String>()
        var openFails = false
        var canAsk = true
        val asked = ArrayList<String>()
        val conns = ArrayList<FakeConn>()
        private var nextFd = 40
        override fun devices() = plugged.values.toList()
        override fun hasPermission(name: String) = name in granted
        override fun requestPermission(name: String): Boolean { asked += name; return canAsk }
        override fun open(name: String): OpenedUsb? {
            if (openFails || name !in granted || name !in plugged) return null
            return FakeConn(nextFd++, "SN-$name", log).also { conns += it; log += "open $name fd ${it.fd}" }
        }
    }

    private class FakeCore(val log: MutableList<String>) : NativeUsbLink {
        val attached = LinkedHashMap<String, Int>()
        var refuse = false
        val drivers = mutableSetOf("rtlsdr", "hackrf", "airspy", "airspyhf")
        override suspend fun attach(dev: UsbDeviceRef, kind: String, fd: Int, serial: String): Boolean {
            log += "attach ${dev.name} $kind fd $fd serial $serial"
            if (refuse || fd < 0) return false
            attached[dev.name] = fd; return true
        }
        override suspend fun detach(name: String) { log += "detach $name"; attached.remove(name) }
        override suspend fun supports(kind: String) = kind in drivers
    }

    private val rtl = UsbDeviceRef("/dev/bus/usb/001/004", 0x0bda, 0x2838, "RTLSDRBlog", "Blog V4")
    private val hackrf = UsbDeviceRef("/dev/bus/usb/001/005", 0x1d50, 0x6089, "Great Scott Gadgets", "HackRF One", maxPowerMa = 500)
    private val mouse = UsbDeviceRef("/dev/bus/usb/001/006", 0x046d, 0xc52b)

    @Test fun grantedDeviceOpensAndItsDescriptorGoesToTheCore() = runTest {
        val log = ArrayList<String>()
        val b = FakeBackend(log).apply { plugged[rtl.name] = rtl; granted += rtl.name }
        val core = FakeCore(log)
        val c = UsbRadioController(b, core)
        c.attached(rtl)
        val r = c.radio(rtl.name)!!
        assertEquals(UsbRadioStatus.READY, r.status)
        assertEquals("SN-${rtl.name}", r.device.serial)                    // the serial read once opened
        assertEquals(b.conns[0].fd, core.attached[rtl.name])                 // the descriptor of that connection, nothing else
        assertEquals(listOf("open ${rtl.name} fd 40", "attach ${rtl.name} rtlsdr fd 40 serial SN-${rtl.name}"), log)
        assertEquals("RTL-SDR Blog V4", r.model.name)
        assertTrue(b.asked.isEmpty())
    }

    @Test fun permissionDialogThenOpen() = runTest {
        val b = FakeBackend(ArrayList()).apply { plugged[hackrf.name] = hackrf }
        val core = FakeCore(ArrayList())
        val c = UsbRadioController(b, core)
        c.attached(hackrf, askPermission = true)
        assertEquals(UsbRadioStatus.ASKING, c.radio(hackrf.name)!!.status)
        assertEquals(listOf(hackrf.name), b.asked)
        // no: denied, with a plain message; asking again shows the dialog again
        c.onPermissionResult(hackrf.name, false)
        assertEquals(UsbRadioStatus.DENIED, c.radio(hackrf.name)!!.status)
        assertTrue(c.radio(hackrf.name)!!.message.contains("ask again"))
        c.requestPermission(hackrf.name)
        assertEquals(2, b.asked.size)
        // yes
        b.granted += hackrf.name
        c.onPermissionResult(hackrf.name, true)
        val r = c.radio(hackrf.name)!!
        assertEquals(UsbRadioStatus.READY, r.status)
        assertTrue(r.powerWarning.contains("powered USB hub"))   // a HackRF on a phone's port
        assertTrue(core.attached.containsKey(hackrf.name))
    }

    @Test fun noDialogWhenNotAskedAndUnsupportedDevicesIgnored() = runTest {
        val b = FakeBackend(ArrayList()).apply { plugged[rtl.name] = rtl; plugged[mouse.name] = mouse }
        val c = UsbRadioController(b, FakeCore(ArrayList()))
        c.rescan(askPermission = false)
        assertEquals(listOf(rtl.name), c.radios.value.map { it.key })   // the mouse is not a radio
        assertEquals(UsbRadioStatus.NEEDS_PERMISSION, c.radios.value[0].status)
        assertTrue(b.asked.isEmpty())
    }

    @Test fun cannotAskOrOpenOrAttachGivePlainErrors() = runTest {
        val b = FakeBackend(ArrayList()).apply { plugged[rtl.name] = rtl; canAsk = false }
        val core = FakeCore(ArrayList())
        val c = UsbRadioController(b, core)
        c.attached(rtl)
        assertEquals(UsbRadioController.MSG_CANNOT_ASK, c.radio(rtl.name)!!.message)
        b.granted += rtl.name; b.openFails = true
        c.requestPermission(rtl.name)
        assertEquals(UsbRadioStatus.FAILED, c.radio(rtl.name)!!.status)
        assertTrue(c.radio(rtl.name)!!.message.contains("Another app"))
        b.openFails = false; core.refuse = true
        c.requestPermission(rtl.name)
        assertEquals(UsbRadioStatus.FAILED, c.radio(rtl.name)!!.status)
        assertTrue(b.conns.last().closed)                     // a connection the core refused is closed again
    }

    @Test fun buildWithoutTheDriverSaysSo() = runTest {
        val b = FakeBackend(ArrayList()).apply { plugged[hackrf.name] = hackrf; granted += hackrf.name }
        val core = FakeCore(ArrayList()).apply { drivers -= "hackrf" }
        val c = UsbRadioController(b, core)
        c.attached(hackrf)
        assertEquals(UsbRadioStatus.NO_DRIVER, c.radio(hackrf.name)!!.status)
        assertTrue(b.conns.isEmpty())                          // not even opened
    }

    @Test fun unplugStopsTheSessionFirstThenDetachesThenCloses() = runTest {
        val log = ArrayList<String>()
        val b = FakeBackend(log).apply { plugged[rtl.name] = rtl; granted += rtl.name }
        val core = FakeCore(log)
        val c = UsbRadioController(b, core, onLost = { r -> log += "lost ${r.key}" })
        c.attached(rtl)
        log.clear()
        b.plugged.remove(rtl.name)
        c.detached(rtl.name)
        assertEquals(listOf("lost ${rtl.name}", "detach ${rtl.name}", "close 40"), log)   // never a closed descriptor in the core
        assertTrue(c.radios.value.isEmpty())
        // re-plug: a new device name and a new descriptor
        val again = rtl.copy(name = "/dev/bus/usb/001/007")
        b.plugged[again.name] = again; b.granted += again.name
        c.attached(again)
        assertEquals(41, core.attached[again.name])
    }

    @Test fun rescanNoticesAMissedDetach() = runTest {
        val log = ArrayList<String>()
        val b = FakeBackend(log).apply { plugged[rtl.name] = rtl; granted += rtl.name }
        val c = UsbRadioController(b, FakeCore(log))
        c.rescan()
        assertEquals(UsbRadioStatus.READY, c.radios.value.single().status)
        b.plugged.clear()
        c.rescan()
        assertTrue(c.radios.value.isEmpty())
        assertTrue(log.contains("close 40"))
    }

    @Test fun noUsbHost() = runTest {
        val b = FakeBackend(ArrayList()).apply { hostSupported = false; plugged[rtl.name] = rtl }
        val c = UsbRadioController(b, FakeCore(ArrayList()))
        c.rescan()
        assertTrue(c.radios.value.isEmpty())
    }
}
