// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.about

import android.content.Context
import io.github.adswill.onair.diag.AppLog

/** A third-party component (tools/credits_data.tsv): name, author, licence, link. */
data class Credit(val name: String, val author: String, val licence: String, val url: String)

/** A contribution (tools/contributors.tsv). */
data class Contribution(val who: String, val link: String, val kind: String, val what: String)

/**
 * The credits data, read from the app's assets. The build copies them from the repository's single sources (android/app/build.gradle.kts,
 * task syncOnAirAssets): tools/credits_data.tsv, tools/contributors.tsv, app/panels/supporters_data.h and LICENSES/OFL-1.1.txt, so
 * the phone shows the same credits as the desktop's Help > Credits and nothing is maintained twice. Parsers are pure (unit tested).
 */
object Credits {
    fun parseCredits(tsv: String): List<Credit> = tsv.lineSequence()
        .filter { it.isNotBlank() && !it.startsWith("#") }
        .mapNotNull { l -> l.split('\t').takeIf { it.size >= 6 }?.let { Credit(it[0].trim(), it[1].trim(), it[2].trim(), it[5].trim()) } }
        .toList()

    fun parseContributors(tsv: String): List<Contribution> = tsv.lineSequence()
        .filter { it.isNotBlank() && !it.startsWith("#") }
        .mapNotNull { l -> l.split('\t').takeIf { it.size >= 4 }?.let { Contribution(it[0].trim(), it[1].trim().takeIf { u -> u != "-" } ?: "", it[2].trim(), it[3].trim()) } }
        .toList()

    /** The names inside `kSupporters[] = { "a", "b", ... };` of supporters_data.h (comments skipped). */
    fun parseSupporters(header: String): List<String> {
        val start = header.indexOf("kSupporters")
        if (start < 0) return emptyList()
        val open = header.indexOf('{', start)
        val close = header.indexOf("};", open)
        if (open < 0 || close < 0) return emptyList()
        val body = header.substring(open + 1, close).lineSequence().map { it.substringBefore("//") }.joinToString("\n")
        return Regex("\"((?:[^\"\\\\]|\\\\.)*)\"").findAll(body).map { it.groupValues[1].replace("\\\"", "\"").replace("\\\\", "\\") }.toList()
    }

    private fun asset(ctx: Context, name: String): String = try {
        ctx.assets.open(name).bufferedReader().use { it.readText() }
    } catch (e: Exception) { AppLog.d("app", "asset $name missing: ${e.message}"); "" }

    fun credits(ctx: Context) = parseCredits(asset(ctx, "credits/credits_data.tsv"))
    fun contributors(ctx: Context) = parseContributors(asset(ctx, "credits/contributors.tsv"))
    fun supporters(ctx: Context) = parseSupporters(asset(ctx, "credits/supporters_data.h"))
    fun ofl(ctx: Context) = asset(ctx, "credits/OFL-1.1.txt")
}
