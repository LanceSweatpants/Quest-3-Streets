// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.content.Context
import org.dolphinemu.dolphinemu.BuildConfig
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
        removeRetiredScenery(context, File(userDir, "Load/StreetVR360"))
        if (BuildConfig.STREET_PUBLIC) {
            // r35l (Lance: max fps): no HD texture pack ships, so looking one up for every texture
            // the game loads is only CPU time. Also fixes headsets that got the earlier GFX.ini.
            setIniValue(File(userDir, "Config/GFX.ini"), "Settings", "HiresTextures", "False")
            // r35m: the big screen's image is the eye swapchain (1680x1760 at 1.0) with the game
            // picture (1280 wide at 2x) stretched over it every frame. 0.8 = 1344x1408: still
            // wider than the picture, 36% fewer pixels to fill and for the headset to sample.
            setIniValue(File(userDir, "Config/GFX.ini"), "VR", "ResolutionScale", "0.8")
            // r35m: GL now runs the XR pacing thread; fill every display refresh with the newest
            // game frame (the default paces to the game: ~57 XR frames/s on a 72 Hz display).
            setIniValue(File(userDir, "Config/GFX.ini"), "VR", "EagerHeartbeat", "True")
        }
    }

    /** Sets [key] = [value] in [section] of an ini file, adding the section or key when missing. */
    private fun setIniValue(file: File, section: String, key: String, value: String) {
        val lines = if (file.exists()) file.readLines().toMutableList() else mutableListOf()
        val header = "[$section]"
        var start = lines.indexOfFirst { it.trim().equals(header, ignoreCase = true) }
        if (start < 0) {
            lines.add(header)
            start = lines.size - 1
        }
        var end = start + 1
        while (end < lines.size && !lines[end].trim().startsWith("[")) {
            end++
        }
        val wanted = "$key = $value"
        val at = (start + 1 until end).firstOrNull {
            lines[it].substringBefore('=').trim().equals(key, ignoreCase = true)
        }
        if (at != null) {
            if (lines[at].trim() == wanted) {
                return
            }
            lines[at] = wanted
        } else {
            lines.add(end, wanted)
        }
        try {
            file.parentFile?.mkdirs()
            file.writeText(lines.joinToString("\n", postfix = "\n"))
            Log.info("$TAG $section/$key set to $value in ${file.name}")
        } catch (e: IOException) {
            Log.error("$TAG Couldn't update ${file.name}: ${e.message}")
        }
    }

    /**
     * r35l (Lance: "delete the whole damn blender render"): the app's own scenery pictures
     * ("streetmr_*") that this release no longer bundles (Hoop City, the old rooftop's sprites and
     * clouds) are deleted from the headset. Other pictures in the folder are left alone.
     */
    private fun removeRetiredScenery(context: Context, folder: File) {
        val bundled = try {
            context.assets.list("$ROOT/StreetVR360")?.toSet() ?: emptySet()
        } catch (e: IOException) {
            return
        }
        val files = folder.listFiles() ?: return
        for (f in files) {
            if (f.isFile && f.name.startsWith("streetmr_") && f.name !in bundled) {
                if (f.delete()) {
                    Log.info("$TAG Removed retired scenery ${f.name}")
                }
            }
        }
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
