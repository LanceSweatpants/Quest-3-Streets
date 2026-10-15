// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * StreetMR: the settings a fresh headset needs, bundled in assets/StreetMR (the Config .ini files,
 * street_tuning.txt and the StreetVR360 pictures). Public users have no adb, so these are installed
 * into the user folder at app start, before the native side loads its config.
 *
 * A settings file is only installed when it isn't there yet: a headset that already has tuned files
 * (pushed by Install-StreetXR.ps1) keeps them. The VR environment pictures are replaced whenever the
 * bundled one differs in size (a new release brings new scenery; other pictures there are left).
 */
object StreetDefaults {
    private const val TAG = "[StreetDefaults]"
    private const val ROOT = "StreetMR"

    /** Dolphin.ini holds this instead of the game folder path (it depends on the app id). */
    private const val GAMES_PLACEHOLDER = "@GAMES@"

    @JvmStatic
    fun install(context: Context, userDir: File) {
        val games = File(userDir, "Games")
        if (!games.isDirectory && !games.mkdirs()) {
            Log.warning("$TAG Could not create ${games.absolutePath}")
        }

        installFolder(context, "$ROOT/Config", File(userDir, "Config")) { name, bytes ->
            if (name == "Dolphin.ini") {
                String(bytes, Charsets.UTF_8)
                    .replace(GAMES_PLACEHOLDER, games.absolutePath)
                    .toByteArray(Charsets.UTF_8)
            } else {
                bytes
            }
        }
        installFile(context, "$ROOT/street_tuning.txt", File(userDir, "street_tuning.txt"), null)
        installFolder(context, "$ROOT/StreetVR360", File(userDir, "Load/StreetVR360"), true, null)
    }

    private fun installFolder(
        context: Context,
        assetFolder: String,
        outFolder: File,
        replaceChanged: Boolean = false,
        transform: ((String, ByteArray) -> ByteArray)?
    ) {
        val names = try {
            context.assets.list(assetFolder) ?: emptyArray()
        } catch (e: IOException) {
            Log.error("$TAG Can't list $assetFolder: ${e.message}")
            return
        }
        for (name in names) {
            installFile(context, "$assetFolder/$name", File(outFolder, name), transform, replaceChanged)
        }
    }

    private fun installFile(
        context: Context,
        asset: String,
        out: File,
        transform: ((String, ByteArray) -> ByteArray)?,
        replaceChanged: Boolean = false
    ) {
        if (out.exists()) {
            if (!replaceChanged) {
                return
            }
            // PNGs are stored uncompressed in the APK, so their length is known without reading.
            val bundled = try {
                context.assets.openFd(asset).use { it.length }
            } catch (e: IOException) {
                -1L
            }
            if (bundled < 0 || bundled == out.length()) {
                return
            }
        }

        val parent = out.parentFile
        if (parent != null && !parent.isDirectory && !parent.mkdirs()) {
            Log.error("$TAG Could not create ${parent.absolutePath}")
            return
        }

        // Write next to the target and rename, so an interrupted copy never leaves a half file
        // that would block the install on the next start.
        val temp = File(out.parentFile, out.name + ".installing")
        try {
            context.assets.open(asset).use { input ->
                FileOutputStream(temp).use { output ->
                    if (transform != null) {
                        output.write(transform(out.name, input.readBytes()))
                    } else {
                        input.copyTo(output, 64 * 1024)
                    }
                }
            }
            if (!temp.renameTo(out)) {
                throw IOException("rename to ${out.absolutePath} failed")
            }
            Log.info("$TAG Installed ${out.absolutePath}")
        } catch (e: IOException) {
            Log.error("$TAG Failed to install $asset: ${e.message}")
            temp.delete()
        }
    }
}
