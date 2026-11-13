// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class UpdateLogicTest {
    private val apiUrl = "https://api.github.com/repos/adswill/OnAir/releases/latest"
    private val dlUrl = "https://github.com/adswill/OnAir/releases/download/v3.8.0/SHA256SUMS"

    @Test fun mirrorTemplates() {
        assertEquals("https://m.example/https://api.github.com/repos/adswill/OnAir/releases/latest", UpdateLogic.mirrorUrl("https://m.example/{url}", apiUrl))
        assertEquals("https://m.example/gh/" + dlUrl, UpdateLogic.mirrorUrl("https://m.example/gh", dlUrl))
        assertEquals("https://m.example/gh/" + dlUrl, UpdateLogic.mirrorUrl("https://m.example/gh/", dlUrl))
        assertTrue(UpdateLogic.mirrorUrl("https://m.example/?u={urlenc}", dlUrl).endsWith("https%3A%2F%2Fgithub.com%2Fadswill%2FOnAir%2Freleases%2Fdownload%2Fv3.8.0%2FSHA256SUMS"))
    }

    @Test fun mirrorRefusals() {
        assertEquals("", UpdateLogic.mirrorUrl("http://m.example/{url}", apiUrl))            // not https
        assertEquals("", UpdateLogic.mirrorUrl("https://m.example/a b/{url}", apiUrl))        // a space
        assertEquals("", UpdateLogic.mirrorUrl("{url}", apiUrl))                              // does not begin with https
        assertEquals("", UpdateLogic.mirrorUrl("https://m.example/{url}", "https://evil.example/x"))   // only the project's own addresses
        assertEquals("", UpdateLogic.mirrorUrl("https://m.example/{url}", "https://github.com/other/repo/releases/x"))
        assertEquals("", UpdateLogic.mirrorUrl("https://m.example/" + "a".repeat(400), apiUrl))
        assertEquals("", UpdateLogic.mirrorUrl("", apiUrl))
    }

    @Test fun mirrorChoice() {
        assertFalse(UpdateLogic.mirrorEnabled(0, false, UpdateLogic.DEFAULT_MIRROR))   // automatic without the fallback: GitHub only
        assertTrue(UpdateLogic.mirrorEnabled(0, true, UpdateLogic.DEFAULT_MIRROR))
        assertFalse(UpdateLogic.mirrorEnabled(1, true, UpdateLogic.DEFAULT_MIRROR))    // GitHub only: never the mirror
        assertTrue(UpdateLogic.mirrorEnabled(2, false, UpdateLogic.DEFAULT_MIRROR))
        assertFalse(UpdateLogic.mirrorEnabled(2, false, " "))                          // no address
    }

    @Test fun versions() {
        assertTrue(UpdateLogic.compareVersions("3.8.0", "3.7.0") > 0)
        assertTrue(UpdateLogic.compareVersions("3.10.0", "3.9.5") > 0)
        assertTrue(UpdateLogic.compareVersions("v3.8.0", "3.8.0") == 0)
        assertTrue(UpdateLogic.compareVersions("3.8.0", "3.8.0-beta.2") > 0)           // a beta is older than its release
        assertTrue(UpdateLogic.compareVersions("3.8.0-beta.2", "3.8.0-beta.1") > 0)
        assertTrue(UpdateLogic.compareVersions("3.8.0", "3.8.0-debug") == 0)           // a debug suffix is ignored
        assertTrue(UpdateLogic.compareVersions("3.7.0", "garbage") > 0)
    }

    @Test fun checksumFile() {
        val h1 = "a".repeat(64); val h2 = "B".repeat(64)
        val sums = "$h1  OnAir-3.8.0-android.apk\r\n$h2 *other.zip\nnot a line\n${"c".repeat(63)}  short.bin\n"
        assertEquals(h1, UpdateLogic.checksumFor(sums, "OnAir-3.8.0-android.apk"))
        assertEquals("b".repeat(64), UpdateLogic.checksumFor(sums, "other.zip"))
        assertEquals("", UpdateLogic.checksumFor(sums, "short.bin"))
        assertEquals("", UpdateLogic.checksumFor(sums, "missing.apk"))
        assertEquals("", UpdateLogic.checksumFor(sums, "android.apk"))                // a name is matched whole
    }

    private fun releaseJson(assets: String) = """{"tag_name":"v3.8.0","draft":false,"html_url":"https://github.com/adswill/OnAir/releases/tag/v3.8.0","body":"notes","assets":[$assets]}"""
    private fun asset(name: String, url: String = "https://github.com/adswill/OnAir/releases/download/v3.8.0/$name", digest: String = "") =
        """{"name":"$name","browser_download_url":"$url","size":12345678${if (digest.isEmpty()) "" else ",\"digest\":\"$digest\""}}"""

    @Test fun parsesTheAndroidPackage() {
        val d = "sha256:" + "d".repeat(64)
        val json = releaseJson(listOf(asset("OnAir-3.8.0-windows-x64.zip"), asset("OnAir-3.8.0-android-unsigned.apk"), asset("OnAir-3.8.0-android.apk", digest = d)).joinToString(","))
        val r = UpdateLogic.parseLatest(json)
        assertNotNull(r)
        assertEquals("v3.8.0", r!!.tag); assertEquals("3.8.0", r.version)
        assertEquals("OnAir-3.8.0-android.apk", r.apkName); assertEquals(12345678L, r.size)
        assertEquals("d".repeat(64), r.digest)
    }

    @Test fun jsonReader() {
        val v = UpdateLogic.parseJson("""{"a":[1,2.5,-3e2,true,false,null,"x\"y\u0041\n"],"b":{"c":{}},"d":[]}""") as Map<*, *>
        assertEquals(listOf(1.0, 2.5, -300.0, true, false, null, "x\"yA\n"), v["a"])
        assertEquals(emptyList<Any?>(), v["d"])
        for (bad in listOf("", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "[1] x", "\"abc", "[".repeat(100)))
            assertNull("bad json accepted: $bad", UpdateLogic.parseJson(bad))
    }

    @Test fun refusesWhatIsNotTheProjectsPackage() {
        assertNull(UpdateLogic.parseLatest("not json"))
        assertNull(UpdateLogic.parseLatest(releaseJson(asset("OnAir-3.8.0-windows-x64.zip"))))                       // no apk
        assertNull(UpdateLogic.parseLatest(releaseJson(asset("OnAir-3.8.0-android-unsigned.apk"))))                  // never the unsigned one
        assertNull(UpdateLogic.parseLatest(releaseJson(asset("OnAir-3.8.0-android.apk", url = "https://evil.example/OnAir-3.8.0-android.apk"))))
        assertNull(UpdateLogic.parseLatest("""{"tag_name":"v3.8.0","draft":true,"assets":[${asset("OnAir-3.8.0-android.apk")}]}"""))
    }

    @Test fun expectedChecksum() {
        val a = "a".repeat(64); val b = "b".repeat(64)
        val sums = "$a  OnAir-3.8.0-android.apk\n"
        assertEquals(a, UpdateLogic.expectedChecksum(sums, "OnAir-3.8.0-android.apk", ""))
        assertEquals(a, UpdateLogic.expectedChecksum(sums, "OnAir-3.8.0-android.apk", a))
        assertNull(UpdateLogic.expectedChecksum(sums, "OnAir-3.8.0-android.apk", b))                  // the two lists disagree: refused
        assertEquals(b, UpdateLogic.expectedChecksum("", "OnAir-3.8.0-android.apk", b))               // only GitHub's digest
        assertEquals("", UpdateLogic.expectedChecksum("", "OnAir-3.8.0-android.apk", ""))             // none: the caller refuses the package
    }

    @Test fun signerMustMatchExactly() {
        val mine = setOf("aa", "bb")
        assertTrue(UpdateLogic.sameSigner(mine, setOf("bb", "aa")))
        assertFalse(UpdateLogic.sameSigner(mine, setOf("aa")))
        assertFalse(UpdateLogic.sameSigner(mine, setOf("aa", "bb", "cc")))
        assertFalse(UpdateLogic.sameSigner(mine, emptySet()))                                          // unsigned
        assertFalse(UpdateLogic.sameSigner(emptySet(), emptySet()))
    }

    @Test fun sha256() {
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", UpdateLogic.sha256Hex("abc".toByteArray()))
        val f = File.createTempFile("onair", ".bin")
        try { f.writeBytes("abc".toByteArray()); assertEquals(UpdateLogic.sha256Hex("abc".toByteArray()), UpdateLogic.sha256Hex(f)) } finally { f.delete() }
    }
}
