// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.street

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.preference.PreferenceManager
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.Log
import org.dolphinemu.dolphinemu.utils.PermissionsHandler
import org.dolphinemu.dolphinemu.utils.StartupHandler
import java.io.File
import kotlin.math.roundToInt

/**
 * StreetMR start screen (replaces booting the game as soon as the app opens). A few big dialogs on
 * the Quest's 2D panel, used with the controller pointer:
 *
 * - no game file yet: "Choose your NBA Street Vol. 2 (USA) game file" -> file picker -> copy + check
 * - menu: Play / Play online / Change game file
 * - Play online: name + voice chat switch -> Host a game / Join a game (8-character code)
 *   -> status dialog polling NativeLibrary.StreetNetStatus() until the game boots.
 *
 * Shown whenever MainActivity comes to the front without a game running. Owned by MainActivity
 * (created as a field, so the file picker and permission launchers are registered in time).
 */
class StreetStartScreen(private val activity: FragmentActivity) : DefaultLifecycleObserver {
    private val handler = Handler(Looper.getMainLooper())
    private var dialog: AlertDialog? = null
    private var pollTask: Runnable? = null

    /** The microphone question is open, or the game is being started. */
    private var busy = false

    /** Sent to Android's "All files access" setting; the chooser opens again on the way back. */
    private var awaitingFileAccess = false
    private var chooserToken = 0

    private val askMicrophone =
        activity.registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            onMicrophoneAnswer(granted)
        }

    init {
        activity.lifecycle.addObserver(this)
    }

    override fun onResume(owner: LifecycleOwner) {
        AfterDirectoryInitializationRunner().runWithLifecycle(activity) {
            applyBackdrop()
            if (awaitingFileAccess && !busy) {
                awaitingFileAccess = false
                showChooser()
                return@runWithLifecycle
            }
            refresh()
        }
    }

    override fun onDestroy(owner: LifecycleOwner) {
        stopPolling()
        handler.removeCallbacksAndMessages(null)
        dialog?.dismiss()
        dialog = null
    }

    /** Shows the right screen, unless one is already up or something else is in progress. */
    private fun refresh() {
        if (!isAlive() || busy || dialog?.isShowing == true) {
            return
        }
        showHome()
    }

    /** Picks the screen from where things stand: a copy, an online game, the game file. */
    private fun showHome() {
        stopPolling()
        if (!isAlive()) {
            return
        }

        val status = StreetNet.status()
        when (status.state) {
            StreetNet.WORKING, StreetNet.WAITING, StreetNet.READY -> {
                showOnlineStatus()
                return
            }

            StreetNet.BOOT -> {
                bootOnlineGame()
                return
            }

            StreetNet.ENDED, StreetNet.ERROR -> {
                showOnlineOver(status)
                return
            }

            StreetNet.PLAYING -> {
                // Still booting or running (the panel came up on top of it): leave it alone.
                if (NativeLibrary.IsRunning()) {
                    return
                }
                // We're back at the menu, so that online game is over.
                StreetNet.quit()
            }
        }

        showMenuOrChooseGame()
    }

    private fun showMenuOrChooseGame() {
        if (StreetGameFile.find(activity) == null) {
            showChooseGame()
        } else {
            showMenu()
        }
    }

    // ---- Game file -----------------------------------------------------------------------------

    private fun showChooseGame() {
        val ui = Ui()
        ui.title(appName())
        ui.text("First, pick your NBA Street Vol. 2 (USA) game file (.rvz or .iso).")
        ui.button("Choose game file") { showChooser() }
        show(ui)
    }

    private fun pickGameFile() = showChooser()

    /**
     * r35c, like Quest 3 Strikers: every GameCube disc image on the headset, checked, to tap the
     * one to play. Needs "All files access" first.
     */
    private fun showChooser() {
        if (!StreetGameFile.hasAllFilesAccess()) {
            val ui = Ui()
            ui.title("Let the app find your game")
            ui.text("Turn on \"Allow access to manage all files\" for this app, then come back here.")
            ui.button("Open the setting") { openAllFilesAccess() }
            if (StreetGameFile.find(activity) != null) {
                ui.button("Back") { showMenu() }
            }
            show(ui)
            return
        }
        val ui = Ui()
        ui.title("Choose your game file")
        val status = ui.text("Looking for game files on this Quest...")
        val list = ui.column()
        ui.button("Look again") { showChooser() }
        if (StreetGameFile.find(activity) != null) {
            ui.button("Back") { showMenu() }
        }
        show(ui)
        val token = ++chooserToken
        Thread({
            val found = StreetGameFile.scan()
            handler.post {
                if (token != chooserToken || !isAlive()) return@post
                if (found.isEmpty()) {
                    status.text = "No game file found on this Quest.\n\nOn your PC, open SideQuest, " +
                            "click the folder icon, open Download and drag your NBA Street Vol. 2 " +
                            "(USA) file in (.rvz or .iso). Then press Look again."
                    return@post
                }
                status.text = "Tap your game file:"
                for (f in found) {
                    val chosen = StreetGameFile.isChosen(activity, f.file)
                    val text = f.file.name + (if (chosen) "   (chosen)" else "") + "\n" + f.label +
                            "\n" + StreetGameFile.shortPath(f.file) + " · " +
                            StreetGameFile.size(f.file.length())
                    ui.listButton(list, text, f.usable) {
                        StreetGameFile.choose(activity, f.file)
                        showHome()
                    }
                }
            }
        }, "StreetGameScan").start()
    }

    private fun openAllFilesAccess() {
        awaitingFileAccess = true
        try {
            activity.startActivity(
                Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + activity.packageName)
                )
            )
        } catch (e: Exception) {
            try {
                activity.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            } catch (e2: ActivityNotFoundException) {
                awaitingFileAccess = false
                showMessage("This headset has no file access setting to open.") { showHome() }
            }
        }
    }

    // ---- VR background -------------------------------------------------------------------------

    private fun showBackdrops() {
        val ui = Ui()
        ui.title("VR background")
        ui.text("What surrounds the big screen in VR.")
        val current = savedBackdrop()
        for ((key, label) in BACKDROPS) {
            ui.button(if (key == current) "$label   ✓" else label) {
                prefs().edit().putString(PREF_BACKDROP, key).apply()
                applyBackdrop()
                showMenu()
            }
        }
        ui.button("Back") { showMenu() }
        show(ui)
    }

    private fun savedBackdrop(): String = prefs().getString(PREF_BACKDROP, "vegas") ?: "vegas"

    /** Tells the native side (it keeps it for the game's VR background). */
    private fun applyBackdrop() {
        try {
            NativeLibrary.StreetSetBackdrop(savedBackdrop())
        } catch (e: UnsatisfiedLinkError) {
            Log.warning("[StreetStartScreen] No StreetSetBackdrop in this build")
        }
    }

    // ---- Menu / Play ---------------------------------------------------------------------------

    private fun showMenu() {
        val ui = Ui()
        ui.title(appName())
        ui.button("Play") { play() }
        // Lance 2026-10-15: online stays in the dev build until it works ("online tomorrow").
        if (!BuildConfig.STREET_PUBLIC) {
            ui.button("Play online") { showPlayOnline() }
        }
        ui.button("VR background") { showBackdrops() }
        ui.button("Change game file") { pickGameFile() }
        show(ui)
    }

    private fun play() {
        val game = StreetGameFile.find(activity)
        if (game == null) {
            showHome()
            return
        }
        launchGame(game)
    }

    /** Boots the game like "Play" always did (stops a leftover game first). Finishes this activity. */
    private fun launchGame(game: File) {
        closeDialog()
        applyBackdrop()
        busy = true
        StartupHandler.playStreet(activity, game)

        // The launch finishes this activity. If for some reason it is still up later, don't leave
        // the player looking at an empty panel.
        handler.postDelayed({
            busy = false
            refresh()
        }, 15000)
    }

    // ---- Play online ---------------------------------------------------------------------------

    private fun showPlayOnline() {
        val ui = Ui()
        ui.title("Play online")
        val nameField = ui.input("Your name", savedName(), MAX_NAME_LENGTH, false)
        val voice = ui.switch("Voice chat", savedVoiceChat())

        fun save() {
            prefs().edit()
                .putString(PREF_NAME, cleanName(nameField.editText?.text))
                .putBoolean(PREF_VOICE_CHAT, voice.isChecked)
                .apply()
        }

        ui.button("Host a game") {
            save()
            startOnline(null)
        }
        ui.button("Join a game") {
            save()
            showJoin()
        }
        ui.button("Back") { showMenuOrChooseGame() }
        show(ui)
    }

    private fun showJoin() {
        val ui = Ui()
        ui.title("Join a game")
        ui.text("Type your friend's code.")
        val codeField = ui.input("Code", "", CODE_LENGTH, true)
        ui.button("Join") {
            val code = codeField.editText?.text?.toString()?.trim()?.lowercase() ?: ""
            if (!CODE_PATTERN.matches(code)) {
                codeField.error = "The code is 8 characters: 0-9 and a-f."
            } else {
                startOnline(code)
            }
        }
        ui.button("Back") { showPlayOnline() }
        show(ui)
    }

    /** [joinCode] null = host. Asks for the microphone first when voice chat is on. */
    private fun startOnline(joinCode: String?) {
        if (savedVoiceChat() && !PermissionsHandler.hasRecordAudioPermission(activity)) {
            pendingOnline = OnlineRequest(joinCode)
            closeDialog()
            busy = true
            askMicrophone.launch(Manifest.permission.RECORD_AUDIO)
            return
        }
        beginOnline(joinCode, PermissionsHandler.hasRecordAudioPermission(activity), false)
    }

    private fun onMicrophoneAnswer(granted: Boolean) {
        busy = false
        val request = pendingOnline
        pendingOnline = null
        if (request == null) {
            refresh()
            return
        }
        beginOnline(request.joinCode, granted, false)
    }

    private fun beginOnline(joinCode: String?, micAllowed: Boolean, leftoverStopped: Boolean) {
        if (!isAlive()) {
            return
        }
        val game = StreetGameFile.find(activity)
        if (game == null) {
            showHome()
            return
        }

        if (!leftoverStopped && NativeLibrary.IsRunning()) {
            // A game left running would get in the way: stop it first (blocks, so not here).
            val ui = Ui()
            ui.title("Play online")
            ui.text("Starting...")
            show(ui)
            busy = true
            Thread({
                StartupHandler.stopLeftoverGame()
                handler.post {
                    busy = false
                    beginOnline(joinCode, micAllowed, true)
                }
            }, "StreetOnlineStop").start()
            return
        }

        val name = savedName()
        onlineGamePath = game.absolutePath
        StreetNet.setVoice(savedVoiceChat(), micAllowed)
        val started = if (joinCode == null) {
            StreetNet.host(game.absolutePath, name)
        } else {
            StreetNet.join(joinCode, game.absolutePath, name)
        }

        if (!started) {
            val status = StreetNet.status()
            StreetNet.quit()
            val message = when {
                !StreetNet.available -> "Online play isn't in this version."
                status.message.isNotEmpty() -> status.message
                else -> "Couldn't start online play. Check your internet and try again."
            }
            showMessage(message) { showMenuOrChooseGame() }
            return
        }

        showOnlineStatus()
    }

    private fun showOnlineStatus() {
        val ui = Ui()
        ui.title("Play online")
        val code = ui.code()
        val message = ui.text("Connecting...")
        ui.button("Cancel") {
            StreetNet.quit()
            showMenuOrChooseGame()
        }
        show(ui)

        startPolling(object : Runnable {
            override fun run() {
                if (pollTask !== this) return
                val status = StreetNet.status()
                when (status.state) {
                    StreetNet.WAITING -> {
                        code.text = "Your code: ${status.code}"
                        code.visibility = View.VISIBLE
                        message.text = status.message.ifEmpty {
                            "Tell your friend this code. Waiting for them to join..."
                        }
                    }

                    StreetNet.BOOT -> {
                        pollTask = null
                        bootOnlineGame()
                        return
                    }

                    StreetNet.PLAYING -> {
                        pollTask = null
                        closeDialog()
                        return
                    }

                    StreetNet.ENDED, StreetNet.ERROR -> {
                        pollTask = null
                        showOnlineOver(status)
                        return
                    }

                    StreetNet.IDLE -> {
                        pollTask = null
                        showMenuOrChooseGame()
                        return
                    }

                    else -> {
                        // WORKING, READY (and anything new): just show what's happening.
                        code.visibility = View.GONE
                        if (status.message.isNotEmpty()) {
                            message.text = status.message
                        }
                    }
                }
                handler.postDelayed(this, POLL_MS)
            }
        })
    }

    /** Both players are in: start the game exactly like "Play", with the same game file. */
    private fun bootOnlineGame() {
        stopPolling()
        val game = onlineGamePath?.let { File(it) }?.takeIf { it.isFile }
            ?: StreetGameFile.find(activity)
        if (game == null) {
            StreetNet.quit()
            showMenuOrChooseGame()
            return
        }
        launchGame(game)
    }

    /** ENDED / ERROR: say so once, reset the online side, back to the menu. */
    private fun showOnlineOver(status: StreetNet.Status) {
        StreetNet.quit()
        val message = status.message.ifEmpty {
            if (status.state == StreetNet.ERROR) "Something went wrong online." else "The online game is over."
        }
        showMessage(message) { showMenuOrChooseGame() }
    }

    // ---- Helpers -------------------------------------------------------------------------------

    private fun showMessage(message: String, onOk: () -> Unit) {
        val ui = Ui()
        ui.text(message)
        ui.button("OK") { onOk() }
        show(ui)
    }

    private fun show(ui: Ui) {
        closeDialog()
        if (!isAlive()) {
            return
        }
        dialog = ui.builder
            .setView(ScrollView(ui.context).apply { addView(ui.root) })
            .setCancelable(false)
            .show()
    }

    private fun closeDialog() {
        stopPolling()
        dialog?.dismiss()
        dialog = null
    }

    private fun startPolling(task: Runnable) {
        stopPolling()
        pollTask = task
        handler.post(task)
    }

    private fun stopPolling() {
        pollTask?.let { handler.removeCallbacks(it) }
        pollTask = null
    }

    private fun isAlive(): Boolean = !activity.isFinishing && !activity.isDestroyed

    private fun appName(): String = activity.getString(R.string.app_name_suffixed)

    private fun prefs() = PreferenceManager.getDefaultSharedPreferences(activity)

    private fun savedName(): String = cleanName(prefs().getString(PREF_NAME, DEFAULT_NAME))

    private fun savedVoiceChat(): Boolean = prefs().getBoolean(PREF_VOICE_CHAT, true)

    private fun cleanName(raw: CharSequence?): String {
        val name = raw?.toString()?.filter { it >= ' ' }?.trim()?.take(MAX_NAME_LENGTH) ?: ""
        return name.ifEmpty { DEFAULT_NAME }
    }

    /** Builds one screen: a column of big text and big buttons (easy to hit with the pointer). */
    private inner class Ui {
        val builder = MaterialAlertDialogBuilder(activity)
        val context: Context = builder.context
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(32), dp(28), dp(32), dp(20))
        }
        private val textColor = MaterialColors.getColor(
            context, com.google.android.material.R.attr.colorOnSurface, "colorOnSurface"
        )

        fun title(text: String): TextView = add(label(text, 30f).apply {
            setTypeface(typeface, Typeface.BOLD)
        }, 20)

        fun text(text: String): TextView = add(label(text, 24f), 16)

        /** The online code, big and hidden until there is one. */
        fun code(): TextView = add(label("", 36f).apply {
            typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
            visibility = View.GONE
        }, 12)

        fun button(text: String, onClick: () -> Unit): MaterialButton =
            add(MaterialButton(context).apply {
                this.text = text
                textSize = 24f
                isAllCaps = false
                minHeight = dp(76)
                setOnClickListener { onClick() }
            }, 8)

        fun input(hint: String, value: String, maxLength: Int, code: Boolean): TextInputLayout {
            val layout = TextInputLayout(context)
            layout.hint = hint
            val edit = TextInputEditText(layout.context)
            edit.textSize = 24f
            edit.setSingleLine(true)
            edit.filters = arrayOf(InputFilter.LengthFilter(maxLength))
            if (code) {
                edit.inputType = InputType.TYPE_CLASS_TEXT or
                        InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
                        InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                edit.typeface = Typeface.MONOSPACE
            } else {
                edit.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
            }
            edit.setText(value)
            layout.addView(edit)
            return add(layout, 16)
        }

        fun switch(text: String, checked: Boolean): MaterialSwitch =
            add(MaterialSwitch(context).apply {
                this.text = text
                textSize = 24f
                isChecked = checked
                minHeight = dp(64)
            }, 8)

        /** A column filled in later (the chooser's list). */
        fun column(): LinearLayout = add(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
        }, 8)

        /** A wide, left-aligned, several-line button in [parent]; greyed out when not usable. */
        fun listButton(parent: LinearLayout, text: String, enabled: Boolean, onClick: () -> Unit) {
            val b = MaterialButton(context).apply {
                this.text = text
                textSize = 18f
                isAllCaps = false
                gravity = Gravity.START or Gravity.CENTER_VERTICAL
                minHeight = dp(76)
                isEnabled = enabled
                setOnClickListener { onClick() }
            }
            val params = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            )
            params.bottomMargin = dp(8)
            parent.addView(b, params)
        }

        fun progress(determinate: Boolean): LinearProgressIndicator =
            add(LinearProgressIndicator(context).apply {
                isIndeterminate = !determinate
                max = 100
            }, 16)

        private fun label(text: String, size: Float) = TextView(context).apply {
            this.text = text
            textSize = size
            gravity = Gravity.CENTER_HORIZONTAL
            setTextColor(textColor)
        }

        private fun <T : View> add(view: T, marginBottomDp: Int): T {
            val params = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            )
            params.bottomMargin = dp(marginBottomDp)
            root.addView(view, params)
            return view
        }

        private fun dp(value: Int): Int =
            (value * context.resources.displayMetrics.density).roundToInt()
    }

    private class OnlineRequest(val joinCode: String?)

    companion object {
        private const val POLL_MS = 250L
        private const val PREF_NAME = "streetOnlineName"
        private const val PREF_VOICE_CHAT = "streetOnlineVoiceChat"
        private const val DEFAULT_NAME = "Player"
        private const val MAX_NAME_LENGTH = 20
        private const val CODE_LENGTH = 8
        private val CODE_PATTERN = Regex("^[0-9a-f]{8}$")
        private const val PREF_BACKDROP = "streetBackdrop"

        // r35c (Lance: plain gradients for people who don't want the Vegas scenery). Keys match
        // OGLOpenXR's MakeGradientCube palettes.
        private val BACKDROPS = listOf(
            "vegas" to "Hoop City (Las Vegas)",
            "sunset" to "Sunset gradient",
            "ocean" to "Ocean gradient",
            "purple" to "Purple gradient",
            "midnight" to "Midnight gradient",
            "mint" to "Mint gradient",
            "fire" to "Fire gradient",
            "gray" to "Gray gradient",
        )

        // Static, so they survive the panel being recreated while the permission question is up or
        // an online game is starting.
        private var pendingOnline: OnlineRequest? = null
        private var onlineGamePath: String? = null
    }
}
