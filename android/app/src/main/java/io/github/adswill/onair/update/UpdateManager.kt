// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.update

import android.app.Application
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings as SysSettings
import io.github.adswill.onair.BuildConfig
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.settings.SettingsStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

enum class UpdatePhase { IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING, READY, INSTALLING, FAILED }

/**
 * @property needConfirm only the mirror vouches for the package's checksum (GitHub could not be reached): the user has to say yes
 * before it is downloaded. The signature check against the installed app still applies afterwards.
 */
data class UpdateState(
    val phase: UpdatePhase = UpdatePhase.IDLE,
    val release: ReleaseInfo? = null,
    val done: Long = 0,
    val total: Long = 0,
    val error: String = "",
    val viaMirror: Boolean = false,
    val needConfirm: Boolean = false,
    val needInstallPermission: Boolean = false,
)

/**
 * The in-app update of the phone (issue #183). Checks GitHub's latest release (or the user's mirror, the same setting as the desktop's
 * update mirror), downloads the APK with progress, verifies its SHA-256 against the release's SHA256SUMS and that it is signed with the
 * installed app's certificate, then hands it to Android's package installer: the system asks the user to confirm, nothing is installed
 * silently. The check can be switched off in Settings; nothing is sent to a mirror unless the user chose it.
 */
class UpdateManager(private val app: Application, private val settings: SettingsStore) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val _state = MutableStateFlow(UpdateState())
    val state: StateFlow<UpdateState> = _state
    private var job: Job? = null
    @Volatile private var cancelled = false
    private var startChecked = false
    @Volatile private var verifiedSha = ""   // the SHA-256 of the file that passed every check

    private fun set(s: UpdateState) { _state.value = s }

    /** The check at program start, once per process, when the setting is on (not for a debug build, which has another package name). */
    fun checkOnStart() {
        if (startChecked || BuildConfig.DEBUG) return
        startChecked = true
        scope.launch { if (settings.data.first().updateCheck) runCheck(manual = false) }
    }

    fun checkNow() {
        if (_state.value.phase == UpdatePhase.CHECKING || _state.value.phase == UpdatePhase.DOWNLOADING) return
        job = scope.launch { runCheck(manual = true) }
    }

    fun cancel() { cancelled = true }

    fun download(confirmMirrorOnly: Boolean) {
        val s = _state.value
        val r = s.release ?: return
        if (s.phase != UpdatePhase.AVAILABLE && s.phase != UpdatePhase.FAILED) return
        job = scope.launch { runDownload(r, confirmMirrorOnly) }
    }

    // ---- network ------------------------------------------------------------------------------------------------------------

    private class Cfg(val source: Int, val fallback: Boolean, val template: String)

    private fun cfgOf(s: Settings) = Cfg(s.updateSource, s.updateMirrorOn, s.updateMirror.trim())

    /** One https request, redirects followed by hand (at most 5, https only). Throws IOException. */
    private fun open(url: String, connectMs: Int, readMs: Int): HttpURLConnection {
        var u = url
        for (hop in 0..5) {
            if (!u.startsWith("https://")) throw IOException("not an https address")
            val c = URL(u).openConnection() as HttpURLConnection
            c.instanceFollowRedirects = false
            c.connectTimeout = connectMs; c.readTimeout = readMs
            c.setRequestProperty("User-Agent", "OnAir/" + BuildConfig.VERSION_NAME)
            c.setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream, text/plain")
            val code = c.responseCode
            if (code in 300..399) {
                val loc = c.getHeaderField("Location") ?: throw IOException("redirect without an address")
                c.disconnect()
                u = URL(URL(u), loc).toString()
                continue
            }
            if (code != 200) { c.disconnect(); throw IOException("HTTP $code") }
            return c
        }
        throw IOException("too many redirects")
    }

    private fun getText(url: String, maxBytes: Int): String {
        val c = open(url, 15000, 20000)
        try {
            c.inputStream.use { ins ->
                val out = java.io.ByteArrayOutputStream()
                val b = ByteArray(8192)
                while (true) {
                    val n = ins.read(b); if (n < 0) break
                    out.write(b, 0, n)
                    if (out.size() > maxBytes) throw IOException("answer too large")
                }
                return out.toString("UTF-8")
            }
        } finally { c.disconnect() }
    }

    private fun getFile(url: String, out: File, maxBytes: Long, onProgress: (Long, Long) -> Unit) {
        val c = open(url, 15000, 30000)
        try {
            val len = c.contentLengthLong
            if (len > maxBytes) throw IOException("file larger than the release lists")
            var done = 0L
            c.inputStream.use { ins ->
                out.outputStream().use { os ->
                    val b = ByteArray(65536)
                    var last = 0L
                    while (true) {
                        if (cancelled) throw IOException("cancelled")
                        val n = ins.read(b); if (n < 0) break
                        os.write(b, 0, n); done += n
                        if (done > maxBytes) throw IOException("file larger than the release lists")
                        val now = System.nanoTime()
                        if (now - last > 100_000_000L) { last = now; onProgress(done, if (len > 0) len else maxBytes) }
                    }
                }
            }
            onProgress(done, done)
        } finally { c.disconnect() }
    }

    /** GitHub first (unless mirror only); the mirror only when the user turned it on and GitHub failed. Returns the text and whether the mirror answered. */
    private fun textWithFallback(url: String, cfg: Cfg, max: Int): Pair<String, Boolean> {
        var gh: Exception? = null
        if (cfg.source != 2) {
            try { return getText(url, max) to false } catch (e: Exception) { gh = e; AppLog.w("update", "GitHub did not answer", e) }
        }
        if (!UpdateLogic.mirrorEnabled(cfg.source, cfg.fallback, cfg.template)) throw (gh ?: IOException("no source to ask"))
        val m = UpdateLogic.mirrorUrl(cfg.template, url)
        if (m.isEmpty()) throw IOException("the mirror address is not usable")
        return getText(m, max) to true
    }

    private fun fileWithFallback(url: String, cfg: Cfg, out: File, max: Long, onProgress: (Long, Long) -> Unit): Boolean {
        var gh: Exception? = null
        if (cfg.source != 2) {
            try { getFile(url, out, max, onProgress); return false } catch (e: Exception) {
                if (cancelled) throw e
                gh = e; AppLog.w("update", "GitHub download failed", e)
            }
        }
        if (!UpdateLogic.mirrorEnabled(cfg.source, cfg.fallback, cfg.template)) throw (gh ?: IOException("no source to ask"))
        val m = UpdateLogic.mirrorUrl(cfg.template, url)
        if (m.isEmpty()) throw IOException("the mirror address is not usable")
        out.delete()
        getFile(m, out, max, onProgress)
        return true
    }

    // ---- check ----------------------------------------------------------------------------------------------------------------

    private suspend fun runCheck(manual: Boolean) {
        set(UpdateState(UpdatePhase.CHECKING))
        try {
            val cfg = cfgOf(settings.data.first())
            val (body, viaMirror) = textWithFallback(UpdateLogic.API_LATEST, cfg, 4 shl 20)
            val r = UpdateLogic.parseLatest(body)
            if (r == null) { set(UpdateState(UpdatePhase.FAILED, error = "the release list has no Android package")); return }
            if (UpdateLogic.compareVersions(r.version, BuildConfig.VERSION_NAME) > 0) {
                AppLog.i("update", "version ${r.version} is available (this is ${BuildConfig.VERSION_NAME})")
                set(UpdateState(UpdatePhase.AVAILABLE, release = r, viaMirror = viaMirror))
            } else set(UpdateState(UpdatePhase.UP_TO_DATE, release = r, viaMirror = viaMirror))
        } catch (e: Exception) {
            AppLog.w("update", "update check failed", e)
            // a start-up check that fails stays quiet; a check the user asked for says why
            set(if (manual) UpdateState(UpdatePhase.FAILED, error = e.message ?: e.javaClass.simpleName) else UpdateState())
        }
    }

    // ---- download and verify --------------------------------------------------------------------------------------------------

    private fun updateDir(): File = File(app.cacheDir, "update").also { it.mkdirs() }

    private suspend fun runDownload(r: ReleaseInfo, confirmMirrorOnly: Boolean) {
        cancelled = false
        val dir = updateDir()
        dir.listFiles()?.forEach { it.delete() }
        try {
            val cfg = cfgOf(settings.data.first())
            // the checksum: the release's SHA256SUMS from github.com; only when GitHub cannot be reached, from the mirror (then the user confirms)
            set(UpdateState(UpdatePhase.DOWNLOADING, release = r))
            val sumsUrl = UpdateLogic.DOWNLOAD_BASE + r.tag + "/SHA256SUMS"
            var sums = ""
            var sumsFromMirror = false
            if (cfg.source != 2) {
                try { sums = getText(sumsUrl, 1 shl 16) } catch (e: Exception) { AppLog.w("update", "SHA256SUMS from GitHub failed", e) }
            }
            if (sums.isEmpty() && UpdateLogic.mirrorEnabled(cfg.source, cfg.fallback, cfg.template)) {
                val m = UpdateLogic.mirrorUrl(cfg.template, sumsUrl)
                if (m.isNotEmpty()) { sums = getText(m, 1 shl 16); sumsFromMirror = true }
            }
            val expected = UpdateLogic.expectedChecksum(sums, r.apkName, if (sumsFromMirror) "" else r.digest)
            if (expected == null) { fail(r, "the checksum GitHub lists differs from the one in SHA256SUMS: not installing it"); return }
            if (expected.isEmpty()) { fail(r, "no checksum is listed for the package: not installing it"); return }
            if (sumsFromMirror && !confirmMirrorOnly) { set(UpdateState(UpdatePhase.AVAILABLE, release = r, viaMirror = true, needConfirm = true)); return }

            val apk = File(dir, r.apkName)
            val cap = if (r.size > 0) r.size + (1 shl 20) else 400L shl 20
            val viaMirror = fileWithFallback(r.apkUrl, cfg, apk, cap) { d, t -> set(UpdateState(UpdatePhase.DOWNLOADING, release = r, done = d, total = t, viaMirror = false)) }
            if (r.size > 0 && apk.length() != r.size) { apk.delete(); fail(r, "the downloaded file has another size than the release lists"); return }
            if (UpdateLogic.sha256Hex(apk) != expected) { apk.delete(); fail(r, "the downloaded file does not match its checksum: not installing it"); return }
            val why = checkPackage(apk)
            if (why != null) { apk.delete(); fail(r, why); return }
            verifiedSha = expected
            AppLog.i("update", "${r.apkName} verified (SHA-256 and signature)")
            set(UpdateState(UpdatePhase.READY, release = r, done = apk.length(), total = apk.length(), viaMirror = viaMirror || sumsFromMirror))
        } catch (e: Exception) {
            AppLog.w("update", "download failed", e)
            fail(r, if (cancelled) "cancelled" else e.message ?: e.javaClass.simpleName)
        }
    }

    private fun fail(r: ReleaseInfo, why: String) = set(UpdateState(UpdatePhase.FAILED, release = r, error = why))

    private fun certsOf(info: android.content.pm.PackageInfo?): Set<String> {
        val si = info?.signingInfo ?: return emptySet()
        val out = HashSet<String>()
        for (sg in si.apkContentsSigners) out.add(sha256(sg.toByteArray()))   // the signer(s) of the APK's current contents
        return out
    }

    private fun sha256(b: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    /** null when the file is this app's package, newer, and signed with the installed app's certificate; else the reason it is not. */
    private fun checkPackage(apk: File): String? {
        val pm = app.packageManager
        val flags = PackageManager.GET_SIGNING_CERTIFICATES
        val cand = pm.getPackageArchiveInfo(apk.absolutePath, flags) ?: return "the downloaded file is not a readable Android package"
        if (cand.packageName != app.packageName) return "the package is for ${cand.packageName}, this app is ${app.packageName}"
        val mine = try { pm.getPackageInfo(app.packageName, flags) } catch (e: Exception) { return "the installed app could not be read" }
        val newer = if (Build.VERSION.SDK_INT >= 28) cand.longVersionCode > mine.longVersionCode else true
        if (!newer) return "the package is not newer than the installed app"
        if (!UpdateLogic.sameSigner(certsOf(mine), certsOf(cand))) return "the package is signed with another certificate than the installed app: not installing it"
        return null
    }

    // ---- install --------------------------------------------------------------------------------------------------------------

    /** Hands the verified file to the system's package installer; the system shows its own confirmation. */
    fun install(ctx: Context) {
        val s = _state.value
        val r = s.release ?: return
        if (s.phase != UpdatePhase.READY) return
        val pm = app.packageManager
        if (!pm.canRequestPackageInstalls()) {   // the user allows OnAir to install packages once, in the system's own screen
            set(s.copy(needInstallPermission = true))
            val i = Intent(SysSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + app.packageName)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            try { ctx.startActivity(i) } catch (e: Exception) { AppLog.w("update", "install permission screen", e) }
            return
        }
        val apk = File(updateDir(), r.apkName)
        if (!apk.isFile) { fail(r, "the downloaded file is gone"); return }
        scope.launch {
            try {
                // checked again right before the hand-over: the file sat in the cache
                if (UpdateLogic.sha256Hex(apk) != verifiedSha) { fail(r, "the downloaded file changed after it was checked"); return@launch }
                val inst = app.packageManager.packageInstaller
                val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
                params.setAppPackageName(app.packageName)
                if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
                val id = inst.createSession(params)
                inst.openSession(id).use { session ->
                    apk.inputStream().use { ins -> session.openWrite("onair.apk", 0, apk.length()).use { os -> ins.copyTo(os); session.fsync(os) } }
                    val intent = Intent(app, InstallResultReceiver::class.java).setAction(InstallResultReceiver.ACTION).setPackage(app.packageName)
                    val pi = PendingIntent.getBroadcast(app, id, intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)
                    set(UpdateState(UpdatePhase.INSTALLING, release = r))
                    session.commit(pi.intentSender)
                }
            } catch (e: Exception) {
                AppLog.w("update", "install hand-over failed", e)
                fail(r, e.message ?: e.javaClass.simpleName)
            }
        }
    }

    /** The system's answer to the install session (from [InstallResultReceiver]). */
    fun onInstallResult(status: Int, message: String?) {
        val r = _state.value.release
        when (status) {
            PackageInstaller.STATUS_SUCCESS -> set(UpdateState(UpdatePhase.UP_TO_DATE, release = r))
            PackageInstaller.STATUS_FAILURE_ABORTED -> set(UpdateState(UpdatePhase.READY, release = r))   // the user said no: the file is still there
            else -> set(UpdateState(UpdatePhase.FAILED, release = r, error = message ?: "install failed ($status)"))
        }
    }
}

/** Receives the package installer's progress for a session started by [UpdateManager.install]. */
class InstallResultReceiver : android.content.BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {   // the system's confirmation screen: the user decides
            val confirm: Intent? = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                                   else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
            if (confirm != null) { confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); try { context.startActivity(confirm) } catch (e: Exception) { AppLog.w("update", "confirm screen", e) } }
            return
        }
        io.github.adswill.onair.AppGraph.updates.onInstallResult(status, intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))
    }

    companion object { const val ACTION = "io.github.adswill.onair.UPDATE_INSTALL_RESULT" }
}
