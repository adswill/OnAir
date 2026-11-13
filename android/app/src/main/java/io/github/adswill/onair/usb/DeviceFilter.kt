// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

/**
 * Reads the `<usb-device vendor-id=".." product-id=".."/>` entries of a USB device filter (res/xml/usb_device_filter.xml) from its
 * source text, the way Android's own DeviceFilter reads the attributes: decimal, or hexadecimal with "0x". Entries without both ids
 * (Android would then match any product of a vendor) are reported in [Parsed.wildcards]. Comments are skipped.
 *
 * Used by the JVM tests to check that the filter and [UsbCatalog] list the same radios; Android itself reads the compiled resource.
 */
object DeviceFilter {
    data class Parsed(val ids: List<UsbCatalog.UsbId>, val wildcards: List<String>, val errors: List<String>)

    private val comment = Regex("<!--.*?-->", RegexOption.DOT_MATCHES_ALL)
    private val element = Regex("<usb-device\\b([^>]*)/?>")
    private val attr = Regex("([\\w:-]+)\\s*=\\s*\"([^\"]*)\"")

    fun parseNumber(v: String): Int? {
        val s = v.trim()
        return when {
            s.startsWith("0x", true) -> s.substring(2).toIntOrNull(16)
            else -> s.toIntOrNull()
        }?.takeIf { it in 0..0xffff }
    }

    fun parse(xml: String): Parsed {
        val ids = ArrayList<UsbCatalog.UsbId>()
        val wild = ArrayList<String>()
        val errs = ArrayList<String>()
        for (m in element.findAll(comment.replace(xml, ""))) {
            val a = attr.findAll(m.groupValues[1]).associate { it.groupValues[1] to it.groupValues[2] }
            val v = a["vendor-id"]
            val p = a["product-id"]
            if (v == null || p == null) { wild += m.value; continue }
            val vi = parseNumber(v)
            val pi = parseNumber(p)
            if (vi == null || pi == null) errs += m.value else ids += UsbCatalog.UsbId(vi, pi)
        }
        return Parsed(ids, wild, errs)
    }
}
