// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.street

import android.content.Context
import android.os.Build
import android.os.Environment
import androidx.preference.PreferenceManager
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.utils.Log
import java.io.File
import java.util.Locale

/**
 * StreetMR: the NBA Street Vol. 2 (USA) game file. r35c (Lance: "choose the file like how we do for
 * strikers, and let rvz files and iso files be accepted"): like Quest 3 Strikers' chooser, every
 * GameCube disc image on the headset is listed and checked, and the one tapped is played from
 * where it is (no copy). That needs Android's "All files access". Headsets set up before r35c may
 * have a copy in <filesDir>/user/Games/street.<ext>, which still counts.
 */
object StreetGameFile {
    private const val TAG = "[StreetGameFile]"
    private const val PREF_PATH = "streetGamePath"

    const val GAME_ID = "GNZE69"

    /** Disc images Dolphin reads. */
    @JvmField
    val EXTENSIONS = listOf("rvz", "iso", "gcm", "ciso", "gcz", "wia")
    private val ARCHIVES = listOf("zip", "7z", "rar")
    private const val ARCHIVE_MIN = 100L * 1024 * 1024
    private const val MAX_DEPTH = 4

    @JvmStatic
    fun gamesDir(context: Context): File = File(context.filesDir, "user/Games")

    /** The current game file, or null if there is none yet. Use this everywhere. */
    @JvmStatic
    fun find(context: Context): File? {
        val chosen = PreferenceManager.getDefaultSharedPreferences(context).getString(PREF_PATH, null)
        if (chosen != null) {
            val f = File(chosen)
            if (f.isFile && f.canRead()) return f
        }
        return EXTENSIONS
            .map { File(gamesDir(context), "street.$it") }
            .firstOrNull { it.isFile && it.length() > 0 }
    }

    /** The current game file's path, or null if there is none yet. */
    @JvmStatic
    fun findPath(context: Context): String? = find(context)?.absolutePath

    fun choose(context: Context, file: File) {
        PreferenceManager.getDefaultSharedPreferences(context).edit()
            .putString(PREF_PATH, file.absolutePath).apply()
        Log.info("$TAG Chosen: ${file.absolutePath}")
    }

    fun isChosen(context: Context, file: File): Boolean =
        find(context)?.absolutePath == file.absolutePath

    /** Android 11+: "All files access" lets the app see the headset's own folders. */
    fun hasAllFilesAccess(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    /** One file found on the headset: usable, or what is wrong with it. */
    class Found(val file: File, val usable: Boolean, val label: String)

    /** Every disc image (and big zipped file) on the headset, playable ones first. Slow: off the UI thread. */
    fun scan(): List<Found> {
        val found = ArrayList<Found>()
        walk(Environment.getExternalStorageDirectory(), 0, found)
        return found.filter { it.usable } + found.filter { !it.usable }
    }

    private fun walk(dir: File, depth: Int, out: MutableList<Found>) {
        val entries = dir.listFiles() ?: return
        for (entry in entries.sortedBy { it.name.lowercase(Locale.ROOT) }) {
            val name = entry.name
            if (entry.isDirectory) {
                if (name.startsWith(".")) continue
                if (depth == 0 && name.equals("Android", ignoreCase = true)) continue
                if (depth < MAX_DEPTH) walk(entry, depth + 1, out)
                continue
            }
            val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
            when {
                ext in EXTENSIONS -> out.add(check(entry))
                ext in ARCHIVES && entry.length() >= ARCHIVE_MIN ->
                    out.add(Found(entry, false, "Zipped. Unzip it on your PC first, then copy the game file in."))
            }
        }
    }

    /** Opens the file with Dolphin's own reader and checks it is NBA Street Vol. 2 (USA). */
    fun check(file: File): Found {
        val game = try {
            GameFile.parse(file.absolutePath)
        } catch (e: Exception) {
            null
        }
        if (game == null) {
            return Found(file, false, "Can't read this file (damaged, or not a GameCube game).")
        }
        val id = game.getGameId()
        if (id == GAME_ID) {
            return Found(file, true, "NBA Street Vol. 2 (USA)")
        }
        val title = game.getTitle().ifEmpty { id }
        val why = if (id.startsWith("GNZ")) "a different region's NBA Street Vol. 2 ($id). This app needs the USA version."
        else "$title ($id), not NBA Street Vol. 2."
        return Found(file, false, "This is $why")
    }

    fun shortPath(file: File): String {
        val root = Environment.getExternalStorageDirectory().absolutePath
        val parent = file.parentFile?.absolutePath ?: return file.name
        return if (parent == root) "Main folder" else parent.removePrefix("$root/")
    }

    fun size(bytes: Long): String =
        if (bytes >= 1L shl 30) String.format(Locale.ROOT, "%.2f GB", bytes / (1024.0 * 1024 * 1024))
        else String.format(Locale.ROOT, "%d MB", bytes / (1024 * 1024))
}
