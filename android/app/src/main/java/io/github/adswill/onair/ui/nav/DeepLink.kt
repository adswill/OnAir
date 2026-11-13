// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.nav

import java.net.URLDecoder
import java.util.Locale

/**
 * `onair://tune?freq=...&mode=...`: open the receiver on a frequency, a mode, or both. Parsed without android.net.Uri, so it is
 * unit tested on the JVM.
 *
 * freq: `100.5` (MHz when below 100000, else Hz), `100500000`, `100.5M`, `100.5MHz`, `7074k`, `1.09G`, `1090e6`.
 * mode: a mode's name as the mode list shows it (`FM broadcast`, `fm-broadcast`, case and punctuation ignored), its protocol id
 * (`vor`, `navtex`), or its index in the list (`#12` or `12`).
 */
data class DeepLink(val freqMhz: Double?, val mode: String?, val run: Boolean = false) {
    companion object {
        const val SCHEME = "onair"
        const val HOST_TUNE = "tune"

        fun parse(link: String): DeepLink? {
            val s = link.trim()
            val schemeEnd = s.indexOf("://")
            if (schemeEnd < 0 || !s.substring(0, schemeEnd).equals(SCHEME, true)) return null
            val rest = s.substring(schemeEnd + 3)
            val host = rest.substringBefore('?').substringBefore('/').lowercase(Locale.ROOT)
            if (host != HOST_TUNE) return null
            val query = rest.substringAfter('?', "").substringBefore('#')
            val params = HashMap<String, String>()
            for (kv in query.split('&')) {
                if (kv.isEmpty()) continue
                val k = decode(kv.substringBefore('=')).lowercase(Locale.ROOT)
                val v = decode(kv.substringAfter('=', ""))
                if (k.isNotEmpty() && k !in params) params[k] = v
            }
            val f = (params["freq"] ?: params["f"])?.let { parseFreqMhz(it) }
            val m = params["mode"]?.trim()?.takeIf { it.isNotEmpty() }
            if (f == null && m == null) return null
            return DeepLink(f, m, params["run"] == "1")
        }

        private fun decode(s: String): String = try { URLDecoder.decode(s, "UTF-8") } catch (_: Exception) { s }

        /** A frequency in MHz from the forms above, or null. Kept between 1 kHz and 100 GHz. */
        fun parseFreqMhz(text: String): Double? {
            var t = text.trim().lowercase(Locale.ROOT).replace(" ", "").replace(',', '.').removeSuffix("hz")
            var mult = 0.0
            when {
                t.endsWith("g") -> { mult = 1e3; t = t.dropLast(1) }
                t.endsWith("m") -> { mult = 1.0; t = t.dropLast(1) }
                t.endsWith("k") -> { mult = 1e-3; t = t.dropLast(1) }
            }
            val v = t.toDoubleOrNull() ?: return null
            if (!v.isFinite() || v <= 0) return null
            val mhz = if (mult != 0.0) v * mult else if (v < 1e5) v else v / 1e6
            return mhz.takeIf { it in 0.001..100_000.0 }
        }

        private fun norm(s: String) = s.lowercase(Locale.ROOT).filter { it.isLetterOrDigit() }

        /**
         * The index of [mode] in a list of mode [names] (and protocol ids [subIds], same order), or null. An exact name wins over a
         * protocol id, which wins over a name that starts with the text.
         */
        fun resolveMode(mode: String, names: List<String>, subIds: List<String?>): Int? {
            val raw = mode.trim().removePrefix("#")
            raw.toIntOrNull()?.let { return it.takeIf { i -> i in names.indices } }
            val n = norm(raw)
            if (n.isEmpty()) return null
            names.indexOfFirst { norm(it) == n }.takeIf { it >= 0 }?.let { return it }
            subIds.indexOfFirst { it != null && norm(it) == n }.takeIf { it >= 0 }?.let { return it }
            return names.indexOfFirst { norm(it).startsWith(n) }.takeIf { it >= 0 }
        }
    }
}
