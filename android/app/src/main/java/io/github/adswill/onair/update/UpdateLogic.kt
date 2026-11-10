package io.github.adswill.onair.update

import java.io.File
import java.security.MessageDigest

/** The newest GitHub release and the Android package in it. [digest] is the "sha256:" value GitHub lists for the file ("" when it lists none). */
data class ReleaseInfo(
    val tag: String,
    val version: String,
    val notes: String,
    val pageUrl: String,
    val apkName: String,
    val apkUrl: String,
    val size: Long,
    val digest: String,
)

/**
 * The parts of the in-app update (issue #183) that need no Android classes, so JVM unit tests cover them: the same rules as the
 * desktop updater (core/src/engine/updater.cpp): only the project's own GitHub addresses are ever asked for, a mirror is a user
 * setting with {url} / {urlenc} or a base address, https only, and the package is accepted only with a matching SHA-256.
 */
object UpdateLogic {
    const val API_LATEST = "https://api.github.com/repos/adswill/OnAir/releases/latest"
    const val DOWNLOAD_BASE = "https://github.com/adswill/OnAir/releases/download/"
    const val DEFAULT_MIRROR = "https://mirror.on-air.dev/{url}"

    /** Where to ask: 0 automatic (GitHub, then the mirror when [mirrorFallback]), 1 GitHub only, 2 mirror only. */
    fun mirrorEnabled(source: Int, mirrorFallback: Boolean, template: String): Boolean =
        template.isNotBlank() && (source == 2 || (source == 0 && mirrorFallback))

    fun isOfficialUrl(u: String): Boolean =
        u.startsWith("https://api.github.com/repos/adswill/OnAir/r") || u.startsWith("https://github.com/adswill/OnAir/releases/")

    private fun percentEncode(s: String): String {
        val sb = StringBuilder()
        for (b in s.toByteArray(Charsets.UTF_8)) {
            val c = b.toInt() and 0xff
            if ((c in 'a'.code..'z'.code) || (c in 'A'.code..'Z'.code) || (c in '0'.code..'9'.code) || c == '-'.code || c == '_'.code || c == '.'.code || c == '~'.code) sb.append(c.toChar())
            else sb.append('%').append("0123456789ABCDEF"[c shr 4]).append("0123456789ABCDEF"[c and 15])
        }
        return sb.toString()
    }

    /**
     * The address to ask the mirror for [url]: {url} in the template becomes it ({urlenc}: percent-encoded); without a placeholder the
     * address is appended after a "/". "" when the template is not an https address, has spaces or control characters, is longer than
     * 400 characters, or when [url] is not one of the project's own GitHub addresses.
     */
    fun mirrorUrl(template: String, url: String, allowHttp: Boolean = false): String {
        if (template.isEmpty() || template.length > 400 || url.isEmpty() || url.length > 600) return ""
        if (template.any { it.code <= 32 || it.code == 127 }) return ""
        val https = template.startsWith("https://")
        if (!https && !(allowHttp && template.startsWith("http://"))) return ""
        if (template.length < 12) return ""
        if (!allowHttp && !isOfficialUrl(url)) return ""
        val out = if (template.contains("{url}") || template.contains("{urlenc}")) {
            val sb = StringBuilder()
            var i = 0
            while (i < template.length) {   // one pass: an {url} inside a replaced value is not replaced again
                if (template.startsWith("{url}", i)) { sb.append(url); i += 5 }
                else if (template.startsWith("{urlenc}", i)) { sb.append(percentEncode(url)); i += 8 }
                else { sb.append(template[i]); i++ }
            }
            sb.toString()
        } else template + (if (template.endsWith("/")) "" else "/") + url
        if (!out.startsWith("https://") && !(allowHttp && out.startsWith("http://"))) return ""
        return out
    }

    private val verRe = Regex("""^v?(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?""")

    /** Numeric, dot separated; a beta is older than its stable release. > 0 if [a] is newer than [b]; an unreadable version counts as 0.0.0. */
    fun compareVersions(a: String, b: String): Int {
        fun key(s: String): List<Int> {
            val m = verRe.find(s.trim()) ?: return listOf(0, 0, 0, 0)
            val beta = m.groupValues[4].toIntOrNull() ?: Int.MAX_VALUE
            return listOf(m.groupValues[1].toInt(), m.groupValues[2].toInt(), m.groupValues[3].toInt(), beta)
        }
        val x = key(a); val y = key(b)
        for (i in 0 until 4) if (x[i] != y[i]) return x[i].compareTo(y[i])
        return 0
    }

    /** The checksum of [name] in the text of a SHA256SUMS file ("<64 hex>  name" per line, also "*name"); "" if absent. */
    fun checksumFor(sums: String, name: String): String {
        for (raw in sums.lineSequence()) {
            val line = raw.trimEnd('\r', ' ')
            if (line.length < 66) continue
            val h = line.substring(0, 64)
            if (!h.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }) continue
            if (line[64] != ' ' && line[64] != '\t') continue
            var k = 64
            while (k < line.length && (line[k] == ' ' || line[k] == '\t')) k++
            if (k < line.length && line[k] == '*') k++
            if (line.substring(k) == name) return h.lowercase()
        }
        return ""
    }

    /** A small JSON reader (objects, arrays, strings, numbers, booleans, null) so the logic runs in plain JVM tests; null on any error. */
    fun parseJson(text: String): Any? {
        var i = 0
        fun ws() { while (i < text.length && text[i].isWhitespace()) i++ }
        fun str(): String {
            val sb = StringBuilder(); i++   // the opening quote
            while (i < text.length && text[i] != '"') {
                var c = text[i++]
                if (c == '\\') {
                    c = text[i++]
                    when (c) {
                        'n' -> sb.append('\n'); 't' -> sb.append('\t'); 'r' -> sb.append('\r'); 'b' -> sb.append('\b'); 'f' -> sb.append('\u000c')
                        'u' -> { sb.append(text.substring(i, i + 4).toInt(16).toChar()); i += 4 }
                        else -> sb.append(c)
                    }
                } else sb.append(c)
            }
            if (i >= text.length) throw IllegalArgumentException("unterminated string")
            i++
            return sb.toString()
        }
        fun value(depth: Int): Any? {
            if (depth > 32) throw IllegalArgumentException("too deep")
            ws()
            if (i >= text.length) throw IllegalArgumentException("end")
            val c = text[i]
            if (c == '{') {
                i++; val m = LinkedHashMap<String, Any?>(); ws()
                if (text[i] == '}') { i++; return m }
                while (true) {
                    ws(); if (text[i] != '"') throw IllegalArgumentException("key")
                    val k = str(); ws()
                    if (text[i++] != ':') throw IllegalArgumentException(":")
                    m[k] = value(depth + 1); ws()
                    val d = text[i++]
                    if (d == '}') return m
                    if (d != ',') throw IllegalArgumentException(",")
                }
            }
            if (c == '[') {
                i++; val l = ArrayList<Any?>(); ws()
                if (text[i] == ']') { i++; return l }
                while (true) {
                    l.add(value(depth + 1)); ws()
                    val d = text[i++]
                    if (d == ']') return l
                    if (d != ',') throw IllegalArgumentException(",")
                }
            }
            if (c == '"') return str()
            if (text.startsWith("true", i)) { i += 4; return true }
            if (text.startsWith("false", i)) { i += 5; return false }
            if (text.startsWith("null", i)) { i += 4; return null }
            val st = i
            while (i < text.length && (text[i].isDigit() || text[i] in "+-.eE")) i++
            if (st == i) throw IllegalArgumentException("value")
            return text.substring(st, i).toDouble()
        }
        return try { val v = value(0); ws(); if (i == text.length) v else null } catch (e: Exception) { null }
    }

    /** The package in a GitHub "latest release" answer: the .apk of the Android build, never the unsigned one. null when the answer has none. */
    fun parseLatest(json: String): ReleaseInfo? {
        val o = parseJson(json) as? Map<*, *> ?: return null
        val tag = o["tag_name"] as? String ?: ""
        if (tag.isEmpty() || o["draft"] == true) return null
        val assets = o["assets"] as? List<*> ?: return null
        for (a in assets) {
            val m = a as? Map<*, *> ?: continue
            val name = m["name"] as? String ?: ""
            if (!name.endsWith(".apk") || name.contains("unsigned", true) || !name.contains("android", true)) continue
            val url = m["browser_download_url"] as? String ?: ""
            if (!url.startsWith("https://github.com/adswill/OnAir/releases/download/")) continue   // only the project's own files
            val d = m["digest"] as? String ?: ""
            return ReleaseInfo(
                tag = tag, version = tag.removePrefix("v"), notes = o["body"] as? String ?: "", pageUrl = o["html_url"] as? String ?: "",
                apkName = name, apkUrl = url, size = (m["size"] as? Double)?.toLong() ?: 0L,
                digest = if (d.startsWith("sha256:") && d.length == 71) d.substring(7).lowercase() else "",
            )
        }
        return null
    }

    /**
     * The checksum to hold the package to: the one in the release's SHA256SUMS ([sums]) and, when GitHub's API lists one, that too: both
     * must agree. "" when there is none (the package is then refused); null when they disagree.
     */
    fun expectedChecksum(sums: String, apkName: String, apiDigest: String): String? {
        val s = checksumFor(sums, apkName)
        if (s.isNotEmpty() && apiDigest.isNotEmpty() && s != apiDigest) return null
        return s.ifEmpty { apiDigest }
    }

    /** The package may replace the installed app only when it is signed with exactly the installed app's certificates (SHA-256 fingerprints). */
    fun sameSigner(installed: Set<String>, candidate: Set<String>): Boolean = candidate.isNotEmpty() && installed == candidate

    fun sha256Hex(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        f.inputStream().use { ins -> val b = ByteArray(65536); while (true) { val n = ins.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    fun sha256Hex(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    /** A short size text: "45.6 MB". */
    fun mb(n: Long): String = "%.1f MB".format(java.util.Locale.ROOT, n / 1048576.0)
}
