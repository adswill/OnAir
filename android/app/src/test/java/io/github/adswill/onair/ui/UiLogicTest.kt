// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import io.github.adswill.onair.Modes
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.ui.about.Credits
import io.github.adswill.onair.ui.modes.ModeFilter
import io.github.adswill.onair.ui.modes.PickerFilter
import io.github.adswill.onair.ui.modes.PickerRow
import io.github.adswill.onair.ui.nav.BackStack
import io.github.adswill.onair.ui.nav.DeepLink
import io.github.adswill.onair.ui.nav.Dest
import io.github.adswill.onair.ui.receiver.autoStepHz
import io.github.adswill.onair.ui.receiver.viewWindow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DeepLinkTest {
    @Test fun frequencyForms() {
        assertEquals(100.5, DeepLink.parseFreqMhz("100.5")!!, 1e-9)
        assertEquals(100.5, DeepLink.parseFreqMhz("100500000")!!, 1e-9)
        assertEquals(100.5, DeepLink.parseFreqMhz("100.5M")!!, 1e-9)
        assertEquals(100.5, DeepLink.parseFreqMhz("100,5 MHz")!!, 1e-9)
        assertEquals(7.074, DeepLink.parseFreqMhz("7074k")!!, 1e-9)
        assertEquals(1090.0, DeepLink.parseFreqMhz("1.09G")!!, 1e-9)
        assertEquals(1090.0, DeepLink.parseFreqMhz("1090e6")!!, 1e-9)
        assertNull(DeepLink.parseFreqMhz("abc"))
        assertNull(DeepLink.parseFreqMhz("-5"))
        assertNull(DeepLink.parseFreqMhz("0"))
    }

    @Test fun parseLinks() {
        val l = DeepLink.parse("onair://tune?freq=100.5M&mode=FM%20broadcast")!!
        assertEquals(100.5, l.freqMhz!!, 1e-9)
        assertEquals("FM broadcast", l.mode)
        assertEquals("vor", DeepLink.parse("ONAIR://tune?mode=vor")!!.mode)
        assertNull(DeepLink.parse("onair://tune"))
        assertNull(DeepLink.parse("onair://other?freq=1"))
        assertNull(DeepLink.parse("https://tune?freq=100"))
    }

    @Test fun resolveModes() {
        val names = listOf("FM broadcast", "DAB", "VOR / ILS")
        val subs = listOf(null, null, "vor")
        assertEquals(0, DeepLink.resolveMode("fm-broadcast", names, subs))
        assertEquals(1, DeepLink.resolveMode("dab", names, subs))
        assertEquals(2, DeepLink.resolveMode("vor", names, subs))
        assertEquals(2, DeepLink.resolveMode("#2", names, subs))
        assertEquals(0, DeepLink.resolveMode("fm", names, subs))
        assertNull(DeepLink.resolveMode("9", names, subs))
        assertNull(DeepLink.resolveMode("xyz", names, subs))
    }
}

class BackStackTest {
    @Test fun topLevelReplacesAndBackGoesHome() {
        var s = listOf(Dest.RECEIVER)
        s = BackStack.navigate(s, Dest.MODES)
        s = BackStack.navigate(s, Dest.SETTINGS)
        assertEquals(listOf(Dest.RECEIVER, Dest.SETTINGS), s)
        s = BackStack.navigate(s, Dest.LOG)
        s = BackStack.navigate(s, Dest.LOG)
        assertEquals(listOf(Dest.RECEIVER, Dest.SETTINGS, Dest.LOG), s)
        assertEquals(Dest.SETTINGS, BackStack.selectedTop(s))
        s = BackStack.back(s)!!
        s = BackStack.back(s)!!
        assertEquals(listOf(Dest.RECEIVER), s)
        assertNull(BackStack.back(s))
        assertEquals(listOf(Dest.RECEIVER), BackStack.navigate(listOf(Dest.RECEIVER, Dest.RADIO, Dest.ABOUT), Dest.RECEIVER))
    }

    @Test fun sanitize() {
        assertEquals(listOf(Dest.RECEIVER, Dest.LOG), BackStack.sanitize(listOf(Dest.LOG)))
        assertEquals(listOf(Dest.RECEIVER), BackStack.sanitize(listOf(Dest.ONBOARDING)))
    }
}

class ModeFilterTest {
    private val names = Modes.all.map { "mode${it.index}" }
    private val blurbs = Modes.all.map { if (it.index == 5) "weather satellite" else "" }

    @Test fun favouritesFirstAndGroups() {
        val rows = ModeFilter.rows(Modes.all, Modes.GROUPS, names, blurbs, PickerFilter(favourites = setOf(7)), 0) { true }
        assertEquals(PickerRow.Header(-1, 1, false), rows[0])
        assertEquals(7, (rows[1] as PickerRow.Mode).def.index)
        assertEquals(Modes.all.size + 1, rows.count { it is PickerRow.Mode })
        assertEquals(rows.size, rows.map { it.key }.toSet().size)        // keys are unique (LazyColumn needs it)
    }

    @Test fun searchHideAndCapable() {
        val search = ModeFilter.rows(Modes.all, Modes.GROUPS, names, blurbs, PickerFilter(query = "weather"), 0) { true }
        assertEquals(listOf(5), search.filterIsInstance<PickerRow.Mode>().map { it.def.index })
        val hidden = ModeFilter.rows(Modes.all, Modes.GROUPS, names, blurbs, PickerFilter(hidden = setOf(1, 2), hideUnused = true), 0) { true }
        assertTrue(hidden.filterIsInstance<PickerRow.Mode>().none { it.def.index == 1 || it.def.index == 2 })
        val capable = ModeFilter.rows(Modes.all, Modes.GROUPS, names, blurbs, PickerFilter(onlyCapable = true), 3) { it.index < 3 }
        assertEquals(listOf(0, 1, 2, 3), capable.filterIsInstance<PickerRow.Mode>().map { it.def.index })   // the selected one stays
        val collapsed = ModeFilter.rows(Modes.all, Modes.GROUPS, names, blurbs, PickerFilter(collapsed = setOf(0)), 0) { true }
        assertTrue(collapsed.filterIsInstance<PickerRow.Mode>().none { it.def.group == 0 })
    }
}

class SpectrumMathTest {
    @Test fun viewWindowStaysInsideTheCapture() {
        assertEquals(0f to 1f, viewWindow(1f, 100e6, 2e6, 100e6))
        val (lo, hi) = viewWindow(4f, 100e6, 2e6, 100e6)
        assertEquals(0.375f, lo, 1e-6f); assertEquals(0.625f, hi, 1e-6f)
        val (lo2, hi2) = viewWindow(4f, 100e6, 2e6, 99e6)            // anchored at the left edge: clamped
        assertEquals(0f, lo2, 1e-6f); assertEquals(0.25f, hi2, 1e-6f)
    }

    @Test fun autoStep() {
        assertEquals(100e3, autoStepHz(200e3), 1e-9)
        assertEquals(5e3, autoStepHz(12.5e3), 1e-9)
        assertEquals(1e3, autoStepHz(0.0), 1e-9)
    }
}

class PrefsAndCreditsTest {
    @Test fun networkRadioRoundTrip() {
        val l = listOf(NetworkRadio(SourceKind.RTL_TCP, "Roof|top;x", "192.168.1.20", 1234), NetworkRadio(SourceKind.SPYSERVER, "", "host", 5555))
        val back = NetworkRadio.decodeAll(NetworkRadio.encodeAll(l))
        assertEquals(2, back.size)
        assertEquals("Roof/top,x", back[0].name)
        assertEquals(l[1], back[1])
        assertTrue(NetworkRadio.decodeAll("0|x|h|1;;2|n|h|99999").isEmpty())   // test signal is not a network radio; bad port
    }

    @Test fun creditsParsers() {
        val credits = Credits.parseCredits("# header\nDear ImGui\tOmar\tMIT\tapp/\tvendored\thttps://x\tnotes\n\nshort\tline\n")
        assertEquals(1, credits.size); assertEquals("https://x", credits[0].url)
        val people = Credits.parseContributors("# c\nann\t-\tidea\tA thing\tFM\t#1\n")
        assertEquals("", people[0].link); assertEquals("A thing", people[0].what)
        val sup = Credits.parseSupporters("// x\ninline constexpr const char* kSupporters[] = {\n    \"ikkim\",\n    // \"not me\"\n    \"Jacco de Jong / NL\",\n};\n")
        assertEquals(listOf("ikkim", "Jacco de Jong / NL"), sup)
    }
}
