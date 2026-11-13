// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import io.github.adswill.onair.OnAirNative

/**
 * The TV part of libonair.so (native/src/jni_tv.cpp; tools/check_jni.py compares the two): the transport stream of the TV modes cut
 * down to the packets of one service, and the service list of the core's demultiplexer. No decoding happens in the library.
 *
 * THREADING: an exception to the "receiver thread only" rule (docs/ARCHITECTURE.md): these touch only the packet ring of jni_tv.cpp,
 * which has its own lock, and the engine's packet tap and service snapshot, which the engine guards itself. [TvPlayer]'s thread calls
 * them; nothing else in the app does.
 */
object TvNative {
    val loaded: Boolean get() = OnAirNative.loaded

    /** JSON: {"packets":N,"network":..,"services":[{"id","name","provider","type","pmt","pcr","ca","scrambled","caState","lcn","now","next","streams":[...]}]} */
    external fun nativeTvServices(): String
    /** Keep the packets of these PIDs ("256,257"); "" stops. True when at least one PID is kept. Empties the ring. */
    external fun nativeTvSelect(pids: String): Boolean
    /** Whole 188-byte packets into [buf], waiting up to [timeoutMs] for the first; the bytes copied, 0 none yet, -1 nothing selected. */
    external fun nativeTvRead(buf: ByteArray, timeoutMs: Int): Int
    /** "seen\tkept\tdropped\tqueued\tstreamSeconds" */
    external fun nativeTvStats(): String
    /**
     * The newest picture of an analogue TV mode ([AnalogSource]: 0 Analog TV, 1 MAC, 2 FPV video), made by the core's receiver as on the desktop.
     * meta (6): in meta[0] the picture number shown last; out {seq, width, height, colour, untuned, field rate}. RGBA (alpha 255) into [out].
     * The bytes copied, 0 when nothing is newer, -(bytes needed) when [out] is too small.
     */
    external fun nativeAnalogFrame(which: Int, meta: DoubleArray, out: ByteArray): Int
}
