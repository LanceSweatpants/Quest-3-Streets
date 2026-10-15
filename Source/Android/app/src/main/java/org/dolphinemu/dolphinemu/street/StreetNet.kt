// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.street

import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.utils.Log

/**
 * StreetMR online 1v1: the start screen's way into NativeLibrary.StreetNet* / StreetVoice*
 * (jni/StreetOnline.cpp). If the native side doesn't have them (UnsatisfiedLinkError), online play
 * just reports itself as unavailable instead of crashing the app.
 */
object StreetNet {
    const val IDLE = "IDLE"
    const val WORKING = "WORKING"
    const val WAITING = "WAITING"
    const val READY = "READY"
    const val BOOT = "BOOT"
    const val PLAYING = "PLAYING"
    const val ENDED = "ENDED"
    const val ERROR = "ERROR"

    class Status(val state: String, val code: String, val message: String)

    @Volatile
    var available = true
        private set

    /** NativeLibrary.StreetNetStatus(), split into STATE|CODE|MESSAGE. */
    fun status(): Status = parse(call(null) { NativeLibrary.StreetNetStatus() })

    fun host(gamePath: String, nickname: String): Boolean =
        call(false) { NativeLibrary.StreetNetHost(gamePath, nickname) }

    fun join(hostCode: String, gamePath: String, nickname: String): Boolean =
        call(false) { NativeLibrary.StreetNetJoin(hostCode, gamePath, nickname) }

    fun quit() = call(Unit) { NativeLibrary.StreetNetQuit() }

    fun setVoice(enabled: Boolean, micAllowed: Boolean) = call(Unit) {
        NativeLibrary.StreetVoiceSetEnabled(enabled)
        NativeLibrary.StreetVoiceSetMicAllowed(micAllowed)
    }

    /** Splits on the first two '|' only: MESSAGE may contain '|' (e.g. in a player's name). */
    fun parse(raw: String?): Status {
        val s = raw ?: return Status(IDLE, "", "")
        val first = s.indexOf('|')
        if (first < 0) {
            return Status(s.trim().ifEmpty { IDLE }, "", "")
        }
        val state = s.substring(0, first).trim().ifEmpty { IDLE }
        val second = s.indexOf('|', first + 1)
        if (second < 0) {
            return Status(state, s.substring(first + 1).trim(), "")
        }
        return Status(state, s.substring(first + 1, second).trim(), s.substring(second + 1).trim())
    }

    private inline fun <T> call(fallback: T, block: () -> T): T {
        if (!available) return fallback
        return try {
            block()
        } catch (e: UnsatisfiedLinkError) {
            Log.error("[StreetNet] Online play is missing from the native library: $e")
            available = false
            fallback
        }
    }
}
