// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.netradio

import io.github.adswill.onair.netradio.AddNetworkRadioForm.Field
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkRadioTest {
    @Test fun specMatchesTheCoreFormat() {
        // the core's settings text (rust/onair_net/src/radios.rs): key,host,port[,remote]
        assertEquals("rtltcp,192.168.1.20,1234", NetworkRadio(NetRadioType.RTL_TCP, "192.168.1.20", 0).spec)
        assertEquals("spyserver,sdr.example.org,5555", NetworkRadio(NetRadioType.SPYSERVER, " sdr.example.org ", 5555).spec)
        assertEquals("kiwisdr,kiwi.local,8073,pass word", NetworkRadio(NetRadioType.KIWISDR, "kiwi.local", 0, "pass word").spec)
        assertEquals("tci,10.0.0.5,50001", NetworkRadio(NetRadioType.TCI, "10.0.0.5", 0, "ignored").spec)   // no remote field for TCI
    }

    @Test fun listRoundTripAndBadItemsSkipped() {
        val v = NetworkRadio.parseList("kiwisdr,kiwi.example.org,8074,pass, word;bogus,h,1;rtltcp,,1;rtltcp,h3,99999;spyserver,h4,5555")
        assertEquals(3, v.size)
        assertEquals("pass, word", v[0].remote)       // the password runs to the end of the item, commas included
        assertEquals(0, v[1].port)                    // out of range -> the type's default
        assertEquals(1234, v[1].portOrDefault)
        assertEquals(v.map { it.spec }, NetworkRadio.parseList(NetworkRadio.formatList(v)).map { it.spec })
        val labelled = listOf(v[2].copy(label = "Roof"), v[0])
        assertEquals(labelled, NetworkRadioList.decode(NetworkRadioList.encode(labelled)))
    }

    @Test fun formValidation() {
        val ok = AddNetworkRadioForm(NetRadioType.RTL_TCP, "192.168.1.20:1234").validate()
        assertTrue(ok.errors.isEmpty())
        assertEquals(1234, ok.radio!!.port)                 // "host:port" pasted into the host field
        assertEquals("192.168.1.20", ok.radio!!.host)

        assertNotNull(AddNetworkRadioForm(host = "").validate().errors[Field.HOST])
        assertNotNull(AddNetworkRadioForm(host = "my host").validate().errors[Field.HOST])
        assertNotNull(AddNetworkRadioForm(host = "a;b").validate().errors[Field.HOST])
        assertNotNull(AddNetworkRadioForm(host = "h", port = "70000").validate().errors[Field.PORT])
        assertNotNull(AddNetworkRadioForm(host = "h", port = "abc").validate().errors[Field.PORT])
        assertNull(AddNetworkRadioForm(host = "h", port = "").validate().errors[Field.PORT])
        assertNotNull(AddNetworkRadioForm(NetRadioType.KIWISDR, "kiwi", remote = "a;b").validate().errors[Field.REMOTE])
        assertNotNull(AddNetworkRadioForm(NetRadioType.SOAPY_REMOTE, "server").validate().errors[Field.TYPE])   // no SoapySDR on Android
        assertTrue(AddNetworkRadioForm(host = "http://sdr.example.org/").validate().errors.isEmpty())
        assertTrue(AddNetworkRadioForm(host = "[fe80::1]", port = "1234").validate().errors.isEmpty())
    }

    @Test fun typeChangeFollowsTheDefaultPort() {
        val f = AddNetworkRadioForm(NetRadioType.RTL_TCP, "h", "1234").withType(NetRadioType.SPYSERVER)
        assertEquals("", f.port)                             // the old default goes, the new one applies
        val g = AddNetworkRadioForm(NetRadioType.RTL_TCP, "h", "7000").withType(NetRadioType.SPYSERVER)
        assertEquals("7000", g.port)                         // a typed port stays
    }

    @Test fun offeredTypes() {
        assertFalse(NetRadioType.SOAPY_REMOTE in NetRadioType.offered)
        assertTrue(NetRadioType.offered.containsAll(listOf(NetRadioType.RTL_TCP, NetRadioType.SPYSERVER, NetRadioType.KIWISDR, NetRadioType.TCI)))
        for (t in NetRadioType.entries) assertEquals(t, NetRadioType.ofKey(t.key))
    }
}
