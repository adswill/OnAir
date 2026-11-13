// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.diag

import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.content.FileProvider
import io.github.adswill.onair.BuildConfig
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Crash guard and crash log. Uncaught exceptions (any thread) and failures of JNI calls are appended to files/crash.log
 * (capped at 64 KB), with a header that says which build and phone it was. The next start shows a banner when there is a crash the
 * user has not seen yet; the Log tab can share the crash log and the app log through [FileProvider] (authority
 * `${applicationId}.files`, res/xml/file_paths.xml: only files copied into cache/share/ are ever exposed).
 */
object CrashLog {
    private const val MAX = 64 * 1024
    fun file(ctx: Context) = File(ctx.filesDir, "crash.log")
    private fun seenMarker(ctx: Context) = File(ctx.filesDir, "crash.seen")

    fun header(): String =
        "OnAir ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE}, ${BuildConfig.BUILD_TYPE}) on ${Build.MANUFACTURER} ${Build.MODEL}, " +
            "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), ABI ${Build.SUPPORTED_ABIS.firstOrNull() ?: "?"}"

    @Synchronized
    fun record(ctx: Context, where: String, t: Throwable) = write(ctx, where, t.stackTraceToString())

    /** A failure that is not an exception on this side, such as a native call that returned an error. */
    @Synchronized
    fun recordNative(ctx: Context, where: String, message: String) = write(ctx, where, message)

    private fun write(ctx: Context, where: String, body: String) {
        try {
            val f = file(ctx)
            if (f.length() > MAX) f.delete()
            val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
            if (!f.exists()) f.writeText(header() + "\n")
            f.appendText("[$stamp] $where: $body\n")
        } catch (_: Throwable) {}
        try { AppLog.e("crash", "$where: ${body.lineSequence().firstOrNull() ?: ""}") } catch (_: Throwable) {}
    }

    /** The crash guard: records every uncaught exception, then lets the previous handler end the process as usual. */
    fun install(ctx: Context) {
        val app = ctx.applicationContext
        val old = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { th, e ->
            record(app, "uncaught in ${th.name}", e)
            old?.uncaughtException(th, e)
        }
    }

    fun last(ctx: Context): String = try { file(ctx).takeIf { it.exists() }?.readText()?.takeLast(4000) ?: "" } catch (_: Throwable) { "" }

    /** True when crash.log grew since the user last saw it (the start-up banner). */
    fun hasUnseen(ctx: Context): Boolean = try {
        val f = file(ctx)
        f.exists() && f.length() != (seenMarker(ctx).takeIf { it.exists() }?.readText()?.trim()?.toLongOrNull() ?: -1L)
    } catch (_: Throwable) { false }

    fun markSeen(ctx: Context) { try { seenMarker(ctx).writeText(file(ctx).length().toString()) } catch (_: Throwable) {} }

    fun clear(ctx: Context) { try { file(ctx).delete(); seenMarker(ctx).delete() } catch (_: Throwable) {} }

    /**
     * A share sheet with the crash log (if any) and the app log as text attachments. The files are copied into cache/share/ first,
     * the only folder the FileProvider exposes.
     */
    fun shareIntent(ctx: Context): Intent {
        val dir = File(ctx.cacheDir, "share").apply { mkdirs() }
        val logFile = File(dir, "onair-log.txt").apply { writeText(header() + "\n\n" + AppLog.dump() + "\n") }
        val files = mutableListOf(logFile)
        file(ctx).takeIf { it.exists() }?.let { files += it.copyTo(File(dir, "onair-crash.txt"), overwrite = true) }
        val auth = ctx.packageName + ".files"
        val uris = ArrayList(files.map { FileProvider.getUriForFile(ctx, auth, it) })
        val send = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
            type = "text/plain"
            putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
            putExtra(Intent.EXTRA_SUBJECT, "OnAir ${BuildConfig.VERSION_NAME} log")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        return Intent.createChooser(send, null).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
}
