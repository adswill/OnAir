package io.github.adswill.onair

/**
 * The JNI library "onair": the desktop receiver library (dect2::Engine) behind a handful of calls. The matching C++ is
 * native/src/jni_onair.cpp; tools/check_jni.py compares the two. Receive only.
 *
 * Source kinds: 0 test signal, 1 IQ file, 2 rtl_tcp, 3 SpyServer. File formats: 0 cs8, 1 cu8, 2 cf32, 3 ci16, 4 cu16.
 */
object OnAirNative {
    val loaded: Boolean = try {
        System.loadLibrary("onair")
        true
    } catch (_: UnsatisfiedLinkError) {
        false
    }

    const val SRC_SYNTH = 0
    const val SRC_FILE = 1
    const val SRC_RTLTCP = 2
    const val SRC_SPY = 3

    external fun nativeInit(resultsDir: String): Boolean
    external fun nativeModeTable(): String
    external fun nativeSetMode(family: Int, subId: String): String
    external fun nativeDeviceInfo(kind: Int, host: String, port: Int): String
    external fun nativeStart(
        kind: Int, host: String, port: Int, path: String, fmt: Int, fileRate: Double, freqHz: Double,
        rate: Double, gainDb: Double, agc: Boolean, bias: Boolean, radio: String,
    ): String
    external fun nativeStop()
    external fun nativeRunning(): Boolean
    external fun nativeNote(): String
    external fun nativeRetune(hz: Double)
    external fun nativeSetGain(db: Double, agc: Boolean)
    external fun nativeSetRadio(radio: String, bias: Boolean)
    external fun nativeSpectrum(out: FloatArray, meta: DoubleArray): Boolean
    external fun nativeGainNow(): Double
    external fun nativeTelemetry(force: Boolean): String
    external fun nativeLog(since: Long): String
    external fun nativeClearResults()
    external fun nativeSetVolume(v: Float, mute: Boolean)
    external fun nativeGeneral(demod: Int, widthHz: Double, sqOn: Boolean, sqAuto: Boolean, sqDb: Float)
    external fun nativeFmDeemphasis(us: Double)
    external fun nativeSetVerbose(on: Boolean)
    external fun nativeVersion(): String
}
