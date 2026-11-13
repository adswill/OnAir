// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.model

import org.json.JSONArray
import org.json.JSONObject

/*
 * Typed models of the radio-access JNI (usb/RadioNative.kt, native/src/jni_radio.cpp): USB radios, network radios, the device sheet.
 * Parsed here and nowhere else, as the rest of model/. Immutable.
 */

/** One gain stage of a radio (information and slider limits; the bridge takes one overall gain). */
data class GainStage(val name: String, val minDb: Double, val maxDb: Double, val stepDb: Double)

/**
 * What a radio is and can do, as its driver reports it after it was opened for a moment (nativeUsbRadios, nativeNetDevice).
 * [rates]: ranges of complex sample rates (lo == hi: one rate); empty = anything between [minRateHz] and [maxRateHz].
 */
data class RadioInfo(
    val name: String,
    /** USB: the registry key ("/dev/bus/usb/001/004"); "" for a network radio. */
    val usbName: String,
    /** "rtlsdr", "hackrf", "airspy", "airspyhf", "mirics", "net". */
    val driver: String,
    val board: String,
    val serial: String,
    val minFreqHz: Double,
    val maxFreqHz: Double,
    val minRateHz: Double,
    val maxRateHz: Double,
    val rates: List<Pair<Double, Double>>,
    val gainMinDb: Double,
    val gainMaxDb: Double,
    val stages: List<GainStage>,
    val biasTee: Boolean,
    val network: Boolean,
    val caps: DeviceCaps,
) {
    /** The sample rates to offer in a list: every single rate, and the ends of every range (sorted, no repeats). */
    val rateChoices: List<Double> get() = rates.flatMap { (lo, hi) -> if (lo == hi) listOf(lo) else listOf(lo, hi) }.distinct().sorted()
    val hasPpm: Boolean get() = caps.settings.any { it.key == "ppm" }

    companion object {
        fun of(o: JSONObject): RadioInfo {
            val rates = o.optJSONArray("rates")?.let { a -> (0 until a.length()).map { a.getJSONArray(it).let { r -> r.getDouble(0) to r.getDouble(1) } } } ?: emptyList()
            val stages = o.optJSONArray("stages")?.let { a ->
                (0 until a.length()).map { a.getJSONObject(it).let { s -> GainStage(s.optString("name"), s.optDouble("min"), s.optDouble("max"), s.optDouble("step", 1.0)) } }
            } ?: emptyList()
            return RadioInfo(
                o.optString("name"), o.optString("usbName"), o.optString("driver"), o.optString("board"), o.optString("serial"),
                o.optDouble("minFreq", 0.0), o.optDouble("maxFreq", 0.0), o.optDouble("minRate", 0.0), o.optDouble("maxRate", 0.0), rates,
                o.optDouble("gainMin", 0.0), o.optDouble("gainMax", 0.0), stages, o.optBoolean("biasTee"), o.optBoolean("network"),
                DeviceCaps.parse(o.toString()),
            )
        }

        /** nativeUsbRadios: {"radios":[...],"error":"..."} -> the radios and the driver's last error ("" = none). */
        fun parseList(json: String): Pair<List<RadioInfo>, String> = try {
            val o = JSONObject(json)
            val a = o.optJSONArray("radios") ?: JSONArray()
            (0 until a.length()).map { of(a.getJSONObject(it)) } to o.optString("error")
        } catch (_: Exception) { emptyList<RadioInfo>() to "" }

        /** nativeNetDevice: a radio, or the reason there is none. */
        fun parseOne(json: String): Result<RadioInfo> = try {
            val o = JSONObject(json)
            if (o.has("error")) Result.failure(IllegalStateException(o.optString("error"))) else Result.success(of(o))
        } catch (e: Exception) { Result.failure(e) }
    }
}

/** What this build of the native library can drive (nativeUsbFeatures). */
data class UsbFeatures(val rtlsdr: Boolean, val hackrf: Boolean, val airspy: Boolean, val airspyhf: Boolean, val mirics: Boolean = false) {
    fun supports(driver: String): Boolean = when (driver) {
        "rtlsdr" -> rtlsdr; "hackrf" -> hackrf; "airspy" -> airspy; "airspyhf" -> airspyhf; "mirics" -> mirics; else -> false
    }

    companion object {
        val none = UsbFeatures(false, false, false, false)
        fun parse(json: String): UsbFeatures = try {
            JSONObject(json).let { UsbFeatures(it.optBoolean("rtlsdr"), it.optBoolean("hackrf"), it.optBoolean("airspy"), it.optBoolean("airspyhf"), it.optBoolean("mirics")) }
        } catch (_: Exception) { none }
    }
}
