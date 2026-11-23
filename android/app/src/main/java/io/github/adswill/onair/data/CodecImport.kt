// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import io.github.adswill.onair.OnAirNative
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/**
 * Settings > Codecs (docs/CODECS.md, "Codecs on Android"). OnAir ships no patented codec: the user picks a .so (or a .zip of them), the core says
 * whether the name is on its list and the file is an arm64-v8a library, the user sees the SHA-256 and a warning, and only then is the file copied into
 * the app's private codecs folder. Nothing is downloaded and nothing is loaded here.
 */

/** One importable codec as the core lists it, with the file installed in the app's folder (empty when none). */
data class CodecEntry(val id: String, val name: String, val usedFor: String, val files: String, val source: String, val installedFile: String, val abi: String) {
    val installed get() = installedFile.isNotEmpty()
    companion object {
        fun parseList(s: String): List<CodecEntry> = s.lines().filter { it.isNotBlank() }.mapNotNull { line ->
            val p = line.split('\t')
            if (p.size < 5) null else CodecEntry(p[0], p[1], p[2], p[3], p[4], p.getOrElse(5) { "" }, p.getOrElse(6) { "" })
        }
    }
}

/** What the core says about a picked file. */
data class CodecCheck(val ok: Boolean, val id: String, val name: String, val usedFor: String, val abi: String, val problem: String) {
    companion object {
        fun parse(s: String): CodecCheck {
            val p = s.split('\t')
            return CodecCheck(p.getOrNull(0) == "1", p.getOrElse(1) { "" }, p.getOrElse(2) { "" }, p.getOrElse(3) { "" }, p.getOrElse(4) { "" }, p.getOrElse(5) { "" })
        }
    }
}

/** A file that passed the check and waits for the user's yes. [temp] is in the cache folder until then. */
data class PendingCodec(val file: String, val id: String, val name: String, val usedFor: String, val abi: String, val size: Long, val sha256: String, val temp: String)

/** The import screen state: files waiting for a yes, files refused (with the reason), and a one-line result. */
data class CodecImportState(val pending: List<PendingCodec> = emptyList(), val refused: List<Pair<String, String>> = emptyList(), val message: String = "") {
    val idle get() = pending.isEmpty() && refused.isEmpty()
    companion object { val none = CodecImportState() }
}

object CodecImport {
    private const val MAX_FILE = 128L * 1024 * 1024
    private const val MAX_ENTRIES = 64

    fun codecDir(ctx: Context): File = File(ctx.filesDir, "codecs")
    private fun stageDir(ctx: Context): File = File(ctx.cacheDir, "codec-import")

    fun sha256(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        f.inputStream().use { i -> val b = ByteArray(65536); while (true) { val n = i.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    /** The plain file name of a zip entry or a picked document: no folder part, so nothing can be written outside the codecs folder. */
    fun baseName(n: String): String = n.substringAfterLast('/').substringAfterLast('\\')

    /** Copies at most [MAX_FILE] bytes; false when the input is longer. */
    private fun copyCapped(input: InputStream, out: OutputStream): Boolean {
        val b = ByteArray(65536)
        var total = 0L
        while (true) {
            val n = input.read(b)
            if (n < 0) return true
            total += n
            if (total > MAX_FILE) return false
            out.write(b, 0, n)
        }
    }

    /** Copies the picked file (a .so or a .zip of them) to the cache folder and checks each library. Never touches the codecs folder. */
    fun stage(ctx: Context, uri: Uri): CodecImportState {
        val cr = ctx.contentResolver
        val name = cr.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
            ?: uri.lastPathSegment ?: ""
        val dir = stageDir(ctx).apply { deleteRecursively(); mkdirs() }
        val pending = ArrayList<PendingCodec>()
        val refused = ArrayList<Pair<String, String>>()
        fun consider(file: String, temp: File) {
            val c = CodecCheck.parse(OnAirNative.nativeCodecCheck(file, temp.absolutePath))
            if (c.ok) pending += PendingCodec(file, c.id, c.name, c.usedFor, c.abi, temp.length(), sha256(temp), temp.absolutePath)
            else { refused += file to c.problem; temp.delete() }
        }
        val isZip = name.endsWith(".zip", true)
        cr.openInputStream(uri)?.use { input ->
            if (isZip) {
                var entries = 0
                ZipInputStream(input.buffered()).use { z ->
                    while (true) {
                        val e = z.nextEntry ?: break
                        if (e.isDirectory) continue
                        if (++entries > MAX_ENTRIES) { refused += "(zip)" to "the zip has too many files"; break }
                        val base = baseName(e.name)
                        if (!base.contains(".so", true)) continue   // readme, licence, other platforms: ignored without a word
                        val tmp = File(dir, "${pending.size + refused.size}.part")
                        val whole = tmp.outputStream().use { copyCapped(z, it) }
                        if (!whole) { tmp.delete(); refused += base to "the file is too large"; continue }
                        consider(base, tmp)
                    }
                }
            } else {
                val tmp = File(dir, "0.part")
                if (!tmp.outputStream().use { copyCapped(input, it) }) { tmp.delete(); refused += baseName(name) to "the file is too large" }
                else consider(baseName(name), tmp)
            }
        } ?: refused.add(baseName(name) to "the file could not be opened")
        if (isZip && pending.isEmpty() && refused.isEmpty()) refused += baseName(name) to "no .so library in the zip"
        return CodecImportState(pending, refused)
    }

    /** Moves the waiting files into the codecs folder (the old file of the same name is replaced). Returns the names installed. */
    fun install(ctx: Context, pending: List<PendingCodec>): List<String> {
        val dest = codecDir(ctx).apply { mkdirs() }
        val done = ArrayList<String>()
        for (p in pending) {
            val src = File(p.temp)
            val to = File(dest, p.file)
            if (to.canonicalFile.parentFile != dest.canonicalFile) continue   // a name that is not a plain file name never gets here; belt and braces
            val part = File(dest, p.file + ".part")
            part.delete()
            src.copyTo(part, overwrite = false)
            part.setReadOnly()   // Android 14 refuses to load a library that is writable
            if (to.exists()) { to.setWritable(true); to.delete() }
            check(part.renameTo(to)) { "the file could not be stored" }
            src.delete()
            done += p.file
        }
        stageDir(ctx).deleteRecursively()
        return done
    }

    fun discard(ctx: Context) { stageDir(ctx).deleteRecursively() }
}
