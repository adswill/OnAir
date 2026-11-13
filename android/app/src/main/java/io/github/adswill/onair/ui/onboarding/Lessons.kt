// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.onboarding

import android.content.Context
import io.github.adswill.onair.diag.AppLog
import org.json.JSONArray
import org.json.JSONObject

/** One page of a lesson. [mode] and [freq] (optional) give it a "Try it" button that tunes there. */
data class LessonStep(val title: String, val body: String, val mode: String? = null, val freq: String? = null)

data class Lesson(val id: String, val title: String, val summary: String, val steps: List<LessonStep>, val order: Int = 100)

/**
 * The lessons of the first-run guide, shared with the desktop: every data/lessons/<id>.json of the repository is copied into the
 * app's assets at build time (assets/lessons/, task syncOnAirAssets). The loader is generic over the files and lenient about the
 * shape, so new lessons need no app change:
 *
 *   { "id": "fm-first", "title": "...", "summary": "...", "order": 1,
 *     "steps": [ { "title": "...", "body": "...", "mode": "FM broadcast", "freq": "100.5M" } ] }
 *
 * Also accepted: "pages" or "slides" for "steps"; "text" or "content" for "body"; "description" for "summary"; a top-level array
 * of such objects; a step that is just a string. Text may be a {"en": "...", "lt": "..."} object (the phone's language is used, then
 * English). Files that do not parse are skipped and logged.
 */
object Lessons {
    fun loadAll(ctx: Context): List<Lesson> {
        val lang = ctx.resources.configuration.locales[0]?.language ?: "en"
        val names = try { ctx.assets.list("lessons")?.filter { it.endsWith(".json") }.orEmpty() } catch (_: Exception) { emptyList() }
        return names.flatMap { n ->
            try { parse(ctx.assets.open("lessons/$n").bufferedReader().use { it.readText() }, n.removeSuffix(".json"), lang) }
            catch (e: Exception) { AppLog.w("app", "lesson $n skipped", e); emptyList() }
        }.sortedWith(compareBy({ it.order }, { it.title }))
    }

    fun parse(json: String, fallbackId: String, lang: String = "en"): List<Lesson> {
        val t = json.trim()
        return if (t.startsWith("[")) JSONArray(t).let { a -> (0 until a.length()).mapNotNull { i -> a.optJSONObject(i)?.let { one(it, "$fallbackId-$i", lang) } } }
        else listOfNotNull(one(JSONObject(t), fallbackId, lang))
    }

    private fun text(o: JSONObject, lang: String, vararg keys: String): String {
        for (k in keys) {
            val v = o.opt(k) ?: continue
            when (v) {
                is String -> if (v.isNotBlank()) return v
                is JSONObject -> (v.optString(lang).ifBlank { v.optString("en") }).let { if (it.isNotBlank()) return it }
            }
        }
        return ""
    }

    private fun one(o: JSONObject, fallbackId: String, lang: String): Lesson? {
        val steps = (o.optJSONArray("steps") ?: o.optJSONArray("pages") ?: o.optJSONArray("slides") ?: JSONArray()).let { a ->
            (0 until a.length()).mapNotNull { i ->
                when (val s = a.opt(i)) {
                    is String -> LessonStep("", s)
                    is JSONObject -> LessonStep(
                        text(s, lang, "title", "heading"), text(s, lang, "body", "text", "content"),
                        s.optString("mode").ifBlank { null }, s.opt("freq")?.toString()?.ifBlank { null },
                    )
                    else -> null
                }
            }
        }
        val title = text(o, lang, "title", "name")
        if (title.isBlank() && steps.isEmpty()) return null
        return Lesson(o.optString("id").ifBlank { fallbackId }, title.ifBlank { fallbackId }, text(o, lang, "summary", "description"), steps, o.optInt("order", 100))
    }
}
