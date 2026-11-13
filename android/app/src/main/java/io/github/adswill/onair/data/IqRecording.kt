// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.data

import android.content.Context
import android.content.Intent
import android.content.ContentValues
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.core.content.FileProvider
import java.io.File

/** The IQ recorder's state for the UI. [bytes] is the size so far, [capBytes] the size at which it stops by itself. */
data class IqRecording(
    val active: Boolean,
    val seconds: Double,
    val bytes: Long,
    val capBytes: Long,
    val nearCap: Boolean,
    /** A note for the user: why the last recording ended by itself (cap, disk full), "" when nothing. */
    val error: String,
    /** The file being written. */
    val file: File?,
    /** The last finished recording's data file. */
    val last: File?,
) {
    companion object {
        const val DEFAULT_CAP_MB = 1024
        val capChoicesMb = listOf(256, 512, 1024, 2048, 4096)
        val idle = IqRecording(false, 0.0, 0L, DEFAULT_CAP_MB * 1048576L, false, "", null, null)
        fun size(bytes: Long): String = if (bytes >= 1048576L * 1024) "%.2f GB".format(bytes / 1073741824.0) else "%.1f MB".format(bytes / 1048576.0)
        fun clock(seconds: Double): String { val s = seconds.toLong(); return "%d:%02d".format(s / 60, s % 60) }
    }
}

/** Where recordings live (the app's own external folder: scoped storage, no permission needed) and how they are shared. */
object IqFiles {
    fun dir(ctx: Context): File = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "iq").apply { mkdirs() }

    /** The newest recording's data file (not the .sigmf-meta). */
    fun newest(ctx: Context): File? = dir(ctx).listFiles { f -> f.isFile && !f.name.endsWith(".sigmf-meta") && !f.name.endsWith(".sigmf-data") }?.maxByOrNull { it.lastModified() }

    fun metaOf(data: File): File = File(data.parentFile, data.nameWithoutExtension + ".sigmf-meta")

    /** The Android share sheet with the recording and its SigMF metadata. Nothing leaves the phone unless the user picks a target. */
    fun shareIntent(ctx: Context, data: File): Intent {
        val auth = ctx.packageName + ".files"
        val files = listOf(data, metaOf(data)).filter { it.exists() }
        val uris = ArrayList(files.map { FileProvider.getUriForFile(ctx, auth, it) })
        val send = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
            type = "application/octet-stream"
            putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
            putExtra(Intent.EXTRA_SUBJECT, data.name)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        return Intent.createChooser(send, null).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }

    /** Copies the recording and its metadata into the public Downloads folder through MediaStore (scoped storage). Returns true on success. */
    fun saveToDownloads(ctx: Context, data: File): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        return try {
            for (f in listOf(data, metaOf(data)).filter { it.exists() }) {
                val v = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, f.name)
                    put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
                    put(MediaStore.Downloads.RELATIVE_PATH, "Download/OnAir")
                }
                val uri: Uri = ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v) ?: return false
                ctx.contentResolver.openOutputStream(uri)?.use { out -> f.inputStream().use { it.copyTo(out) } } ?: return false
            }
            true
        } catch (_: Exception) { false }
    }
}
