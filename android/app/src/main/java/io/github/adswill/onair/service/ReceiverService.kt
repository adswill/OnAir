// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.service

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import io.github.adswill.onair.AppGraph
import io.github.adswill.onair.MainActivity
import io.github.adswill.onair.R
import io.github.adswill.onair.data.Phase
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.SourceKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import java.util.Locale

/**
 * The foreground service of a receive session. It is started by ReceiverRepository.start() (from the UI, so the start is allowed)
 * and stops itself when the repository's phase is back to IDLE. While it runs:
 *  - the process stays alive with the screen off or the activity gone, so the receiver thread keeps reading and the audio plays;
 *  - a partial wake lock keeps the CPU awake and a Wi-Fi lock keeps rtl_tcp / SpyServer streams flowing;
 *  - a notification shows mode, frequency and state, with a Stop action.
 * The receiver itself (engine thread, native library) belongs to the repository (process singleton); the service is its keep-alive.
 *
 * Foreground service types: mediaPlayback (the receiver plays audio), plus connectedDevice for a USB radio and microphone for the
 * sound-card input ([foregroundType]). A USB radio pulled out mid-session stops the session (RadioRepository), and with it the service.
 */
class ReceiverService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var watch: Job? = null
    private var wake: PowerManager.WakeLock? = null
    private var wifi: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val repo = AppGraph.repository
        if (intent?.action == ACTION_STOP) {
            AppLog.i("service", "stop from the notification")
            repo.stop()
        }
        ensureChannel(this)
        val type = foregroundType(repo.settings.value.source, repo.radios.usbFor(repo.settings.value) != null, repo.radios.soundPermitted)
        val note = build(repo.state.value)
        try {
            ServiceCompat.startForeground(this, NOTIFICATION_ID, note, type)
        } catch (e: Exception) {
            // a type whose prerequisite is gone (USB permission lost, microphone denied) throws: plain media playback needs nothing more.
            // Leaving without any startForeground() would take the app down ("did not then call Service.startForeground").
            AppLog.w("service", "foreground type $type refused: ${e.message}")
            try {
                ServiceCompat.startForeground(this, NOTIFICATION_ID, note, foregroundType(SourceKind.TEST_SIGNAL, usbGranted = false, micGranted = false))
            } catch (e2: Exception) {
                // Android 12+ refuses a foreground start from the background; the receiver still runs while the app is visible
                AppLog.e("service", "could not go to the foreground", e2)
                stopSelf()
                return START_NOT_STICKY
            }
        }
        acquireLocks()
        if (watch == null) watch = scope.launch {
            repo.state.map { Triple(it.phase, it.modeIndex, it.freqMhz) to it }.distinctUntilChanged { a, b -> a.first == b.first }.collect { (_, st) ->
                if (st.phase == Phase.IDLE) { AppLog.i("service", "receiver idle, service ends"); stopSelf() }
                else getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, build(st))
            }
        }
        // not sticky: after the system killed the process there is no session to resume (the native receiver is gone with it)
        return START_NOT_STICKY
    }

    private fun build(st: ReceiverState): Notification {
        val names = resources.getStringArray(R.array.mode_names)
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, ReceiverService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        val stateText = getString(
            when (st.phase) { Phase.STARTING -> R.string.state_starting; Phase.STOPPING -> R.string.state_stopping; else -> R.string.state_receiving },
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(names.getOrElse(st.modeIndex) { getString(R.string.app_name) })
            .setContentText(String.format(Locale.US, "%.4f MHz  ·  %s", st.freqMhz, stateText))
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .addAction(0, getString(R.string.stop), stop)
            .build()
    }

    @SuppressLint("WakelockTimeout")   // held for exactly as long as the receive session; released in onDestroy
    private fun acquireLocks() {
        if (wake == null) wake = getSystemService(PowerManager::class.java)?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "OnAir:receiver")?.apply {
            setReferenceCounted(false); acquire()
        }
        if (wifi == null) {
            @Suppress("DEPRECATION")
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
            wifi = (applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager)?.createWifiLock(mode, "OnAir:stream")?.apply {
                setReferenceCounted(false); acquire()
            }
        }
    }

    override fun onDestroy() {
        scope.cancel()
        try { wake?.release() } catch (_: Exception) {}
        try { wifi?.release() } catch (_: Exception) {}
        wake = null; wifi = null
        super.onDestroy()
    }

    companion object {
        const val CHANNEL_ID = "receiver"
        const val NOTIFICATION_ID = 1
        const val ACTION_STOP = "io.github.adswill.onair.STOP"

        /**
         * The foreground-service types of a session: mediaPlayback always (the decoded audio); connectedDevice while a USB radio is the
         * source (Android 14 allows it only once the user granted the USB permission, which [usbGranted] says); microphone while the
         * sound-card input records ([micGranted]: RECORD_AUDIO). Asking for a type without its prerequisite throws, so they are added
         * only when they hold.
         */
        fun foregroundType(source: SourceKind, usbGranted: Boolean, micGranted: Boolean): Int {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return 0
            var t = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            if (source == SourceKind.USB && usbGranted) t = t or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            if (source == SourceKind.SOUND_CARD && micGranted && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) t = t or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            return t
        }

        /** Called by the repository when a session starts (the app is in the foreground then). */
        fun start(ctx: Context) {
            try {
                ContextCompat.startForegroundService(ctx, Intent(ctx, ReceiverService::class.java))
            } catch (e: Exception) {
                AppLog.e("service", "foreground service not started", e)
            }
        }

        fun ensureChannel(ctx: Context) {
            val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
            if (nm.getNotificationChannel(CHANNEL_ID) != null) return
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, ctx.getString(R.string.channel_receiver), NotificationManager.IMPORTANCE_LOW).apply {
                    description = ctx.getString(R.string.channel_receiver_desc)
                    setShowBadge(false)
                },
            )
        }
    }
}
