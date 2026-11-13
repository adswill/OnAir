// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

class TsDemuxTest {
    private val rnd = Random(7)
    private fun payload(n: Int) = ByteArray(n) { rnd.nextInt(256).toByte() }

    @Test fun ptsRoundTripAllBits() {
        for (v in longArrayOf(0, 1, 90_000, 0x1_0000_0000L, PTS_MODULUS - 1, 0x1_2345_6789L)) {
            val pes = TsBuild.pes(0xE0, v, null, ByteArray(4))
            assertEquals(v, readPts(pes, 9))
        }
    }

    @Test fun reassemblesBoundedAndUnboundedPesAcrossPackets() {
        val got = ArrayList<Pes>()
        val d = TsDemux { got += it }
        d.setPids(listOf(0x100, 0x101))
        val v = TsBuild.Packetizer(0x100)
        val a = TsBuild.Packetizer(0x101)
        val vPayloads = listOf(payload(5000), payload(183), payload(184), payload(1))
        val aPayloads = listOf(payload(700), payload(13))
        val parts = ArrayList<ByteArray>()
        vPayloads.forEachIndexed { i, p -> parts += v.packets(TsBuild.pes(0xE0, 1000L + i * 3600, 900L + i * 3600, p, bounded = false), randomAccess = i == 0) }
        aPayloads.forEachIndexed { i, p -> parts += a.packets(TsBuild.pes(0xC0, 2000L + i * 2160, null, p)) }
        // one more video start so the last unbounded PES is handed out
        parts += v.packets(TsBuild.pes(0xE0, 99999, null, payload(10), bounded = false))
        // interleave audio and video packets as a multiplexer would, and feed in odd-sized chunks
        val all = TsBuild.join(parts.shuffledKeepingOrderPerPid())
        var i = 0
        while (i < all.size) { val n = minOf(all.size - i, 1 + rnd.nextInt(700)); d.feed(all.copyOfRange(i, i + n)); i += n }
        val video = got.filter { it.pid == 0x100 }
        val audio = got.filter { it.pid == 0x101 }
        assertEquals(vPayloads.size, video.size)
        vPayloads.forEachIndexed { k, p -> assertArrayEquals(p, video[k].data); assertEquals(1000L + k * 3600, video[k].pts); assertEquals(900L + k * 3600, video[k].dts) }
        assertTrue(video[0].randomAccess)
        assertFalse(video[1].randomAccess)
        assertEquals(aPayloads.size, audio.size)
        aPayloads.forEachIndexed { k, p -> assertArrayEquals(p, audio[k].data); assertEquals(2000L + k * 2160, audio[k].pts) }
        assertTrue(got.none { it.damaged })
        assertEquals(0, d.ccErrors())
    }

    /** Keeps each PID's packet order but mixes the PIDs (round robin on the PID bytes). */
    private fun List<ByteArray>.shuffledKeepingOrderPerPid(): List<ByteArray> {
        val byPid = groupBy { ((it[1].toInt() and 0x1F) shl 8) or (it[2].toInt() and 0xFF) }.mapValues { ArrayDeque(it.value) }
        val out = ArrayList<ByteArray>()
        while (byPid.values.any { it.isNotEmpty() }) for (q in byPid.values) if (q.isNotEmpty() && rnd.nextBoolean()) out += q.removeFirst()
        return out
    }

    @Test fun lostPacketMarksDamagedAndDuplicateIsIgnored() {
        val got = ArrayList<Pes>()
        val d = TsDemux { got += it }
        d.setPids(listOf(0x200))
        val pk = TsBuild.Packetizer(0x200)
        val p1 = pk.packets(TsBuild.pes(0xC0, 10, null, payload(600)))
        val p2 = pk.packets(TsBuild.pes(0xC0, 20, null, payload(600)))
        val p3 = pk.packets(TsBuild.pes(0xC0, 30, null, payload(100)))
        // p1 with its second packet sent twice (a legal duplicate), p2 with a packet lost, p3 intact
        d.feed(TsBuild.join(listOf(p1[0], p1[1], p1[1]) + p1.drop(2) + listOf(p2[0]) + p2.drop(2) + p3))
        assertEquals(3, got.size)
        assertFalse("duplicate is not a loss", got[0].damaged)
        assertTrue("lost packet: damaged (cut short)", got[1].damaged)
        assertFalse(got[2].damaged)
        assertEquals(1, d.ccErrors())
    }

    @Test fun resyncsAfterGarbageAndJoinsMidPes() {
        val got = ArrayList<Pes>()
        val d = TsDemux { got += it }
        d.setPids(listOf(0x300))
        val pk = TsBuild.Packetizer(0x300)
        val first = pk.packets(TsBuild.pes(0xE0, 1, null, payload(1000), bounded = false))
        val second = pk.packets(TsBuild.pes(0xE0, 2, null, payload(50), bounded = false))
        val third = pk.packets(TsBuild.pes(0xE0, 3, null, payload(50), bounded = false))
        // start in the middle of the first PES (no start seen: dropped), then 77 zero bytes, then the rest
        val stream = TsBuild.join(first.drop(2)) + ByteArray(77) + TsBuild.join(second + third)
        d.feed(stream)
        d.flush()
        assertTrue(d.syncLosses >= 1)
        assertEquals(listOf(2L, 3L), got.map { it.pts })
    }

    @Test fun teiMarksDamaged() {
        val got = ArrayList<Pes>()
        val d = TsDemux { got += it }
        d.setPids(listOf(0x40))
        val pk = TsBuild.Packetizer(0x40)
        val a = pk.packets(TsBuild.pes(0xC0, 5, null, payload(400)))
        a[1][1] = (a[1][1].toInt() or 0x80).toByte()   // transport_error_indicator
        d.feed(TsBuild.join(a + pk.packets(TsBuild.pes(0xC0, 6, null, payload(10)))))
        assertTrue(got[0].damaged)
        assertFalse(got[1].damaged)
    }

    @Test fun unwrapsAcrossThe33BitWrapAndSmallStepsBack() {
        val u = PtsUnwrapper()
        val start = PTS_MODULUS - 90_000L * 2   // two seconds before the wrap
        var last = u.unwrap(start)
        assertEquals(start, last)
        for (k in 1..200) {
            val ext = start + k * 3600L
            val got = u.unwrap(ext and (PTS_MODULUS - 1))
            assertEquals(ext, got)
            assertTrue(got > last); last = got
        }
        // a B-frame PTS a little behind the last one stays behind, not 26 hours ahead
        val back = u.unwrap((last - 7200) and (PTS_MODULUS - 1))
        assertEquals(last - 7200, back)
        assertEquals(1_000_000L, ptsToUs(90_000))
    }
}
