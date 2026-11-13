// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

/** The waterfall colour maps of the desktop (app/app.h Waterfall::color): the same stops, the same Turbo polynomial. */
object Colormaps {
    val names = listOf("Classic", "Viridis", "Inferno", "Grey", "Turbo", "Cividis")
    private val cool = arrayOf(f(6, 10, 20), f(16, 38, 70), f(30, 100, 140), f(110, 190, 205), f(240, 250, 250))
    private val grey = arrayOf(f(0, 0, 0), f(34, 34, 33), f(96, 95, 92), f(170, 168, 162), f(244, 242, 236))
    private val viridis = arrayOf(f(68, 1, 84), f(71, 44, 122), f(59, 81, 139), f(44, 113, 142), f(33, 144, 141), f(39, 173, 129), f(92, 200, 99), f(170, 220, 50), f(253, 231, 37))
    private val inferno = arrayOf(f(0, 0, 4), f(31, 12, 72), f(85, 15, 109), f(136, 34, 106), f(186, 54, 85), f(227, 89, 51), f(249, 140, 10), f(249, 201, 50), f(252, 255, 164))
    private val cividis = arrayOf(f(0, 32, 77), f(26, 52, 107), f(66, 78, 108), f(97, 104, 111), f(124, 123, 120), f(154, 145, 119), f(187, 167, 108), f(221, 193, 88), f(255, 233, 69))
    private fun f(r: Int, g: Int, b: Int) = floatArrayOf(r.toFloat(), g.toFloat(), b.toFloat())

    private fun stops(s: Array<FloatArray>, v0: Float): Int {
        val v = v0 * (s.size - 1)
        val i = minOf(s.size - 2, v.toInt())
        val fr = v - i
        fun ch(k: Int) = (s[i][k] + (s[i + 1][k] - s[i][k]) * fr + 0.5f).toInt()
        return (0xFF shl 24) or (ch(0) shl 16) or (ch(1) shl 8) or ch(2)
    }

    private fun turbo(v: Float): Int {
        val t = 0.12f + 0.84f * v
        fun poly(a0: Float, a1: Float, a2: Float, a3: Float, a4: Float, a5: Float) =
            (a0 + t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5))))).coerceIn(0f, 1f)
        val r = poly(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f, -152.94239396f, 59.28637943f)
        val g = poly(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f, 4.27729857f, 2.82956604f)
        val b = poly(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f, -89.90310912f, 27.34824973f)
        val f = minOf(1f, v / 0.22f)
        val k = f * f * (3 - 2 * f)
        return (0xFF shl 24) or ((r * k * 255 + 0.5f).toInt() shl 16) or ((g * k * 255 + 0.5f).toInt() shl 8) or (b * k * 255 + 0.5f).toInt()
    }

    fun color(map: Int, v0: Float): Int {
        val v = v0.coerceIn(0f, 1f)
        return when (map) {
            1 -> stops(viridis, v)
            2 -> stops(inferno, v)
            3 -> stops(grey, v)
            4 -> turbo(v)
            5 -> stops(cividis, v)
            else -> stops(cool, v)
        }
    }

    fun lut(map: Int): IntArray = IntArray(256) { color(map, it / 255f) }
}
