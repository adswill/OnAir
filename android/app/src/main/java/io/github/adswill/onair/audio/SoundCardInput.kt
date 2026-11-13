// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.audio

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import androidx.core.content.ContextCompat
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.usb.RadioNative

/**
 * The sound-card source on a phone: AudioRecord (the microphone, a wired headset, a USB audio interface or a radio's line output
 * through one) recording stereo float at [rate] Hz on a thread of its own, pushed into the core (audio_push.h) where the sound-card
 * source (source_audio.cpp, device "push") turns it into IQ exactly as the desktop does with a sound card.
 *
 * Privacy: the microphone is opened only by [start], which the repository calls when a session with the sound-card source starts,
 * and closed by [stop] when it ends. Nothing is recorded to a file or sent anywhere. RECORD_AUDIO must have been granted (the UI asks).
 */
class SoundCardInput(private val ctx: Context, val rate: Int = 48000) {
    @Volatile private var run = false
    private var thread: Thread? = null

    val permitted: Boolean get() = ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** "" when recording, else why not in plain words. Call it before the engine starts the source (the source wants the rate). */
    @SuppressLint("MissingPermission")   // checked by [permitted]
    fun start(source: Int = MediaRecorder.AudioSource.UNPROCESSED): String {
        if (run) return ""
        if (!permitted) return "OnAir needs the microphone permission for the sound-card input. Allow it when asked, or in the phone's app settings."
        if (!RadioNative.loaded) return "The receiver library is missing"
        val fmt = AudioFormat.ENCODING_PCM_FLOAT
        val ch = AudioFormat.CHANNEL_IN_STEREO
        val min = AudioRecord.getMinBufferSize(rate, ch, fmt)
        if (min <= 0) return "This phone cannot record stereo at $rate Hz"
        // UNPROCESSED (no noise suppression or AGC, which would ruin an IF) where the phone has it, else the plain microphone path
        val rec = listOf(source, MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC).firstNotNullOfOrNull { s ->
            try {
                val r = AudioRecord(s, rate, ch, fmt, maxOf(min * 4, rate / 5 * 8))
                if (r.state == AudioRecord.STATE_INITIALIZED) r else { r.release(); null }
            } catch (_: Exception) { null }
        } ?: return "The phone did not open its sound input (another app may be recording)"
        RadioNative.nativeAudioConfigure(rate, 1.0)
        run = true
        thread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            val buf = FloatArray(rate / 50 * 2)   // 20 ms of stereo
            try {
                rec.startRecording()
                while (run) {
                    val n = rec.read(buf, 0, buf.size, AudioRecord.READ_BLOCKING)
                    if (n < 0) { AppLog.w("audio", "AudioRecord.read: $n"); break }
                    if (n > 0) RadioNative.nativeAudioPush(buf, n / 2)
                }
            } catch (e: Exception) {
                AppLog.e("audio", "sound-card input stopped", e)
            } finally {
                try { rec.stop() } catch (_: Exception) {}
                rec.release()
            }
        }, "onair-audio-in").apply { start() }
        AppLog.i("audio", "sound-card input on, $rate Hz stereo")
        return ""
    }

    fun stop() {
        run = false
        thread?.join(500)
        thread = null
    }
}
