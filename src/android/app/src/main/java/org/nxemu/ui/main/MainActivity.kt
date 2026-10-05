package org.nxemu.ui.main

import android.content.Context
import android.content.Intent
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.webkit.ConsoleMessage
import android.webkit.JsResult
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.NXUISetting
import org.nxemu.NativeLibrary
import org.nxemu.features.input.NativeInput
import org.nxemu.features.input.model.NativeButton
import org.nxemu.ui.emulation.EmulationActivity
import org.nxemu.ui.settings.ControllerSettings
import org.nxemu.ui.settings.InputMappingSession
import org.nxemu.ui.settings.OverlayLayoutActivity
import org.nxemu.utils.InputHandler
import org.nxemu.utils.ThemeHelper

open class MainActivity : ComponentActivity() {
    private lateinit var webView: WebView
    private val inputMapping = InputMappingSession(this) { refreshControllerPages() }
    private var emulationLaunchPending = false
    private var stickX = 0
    private var stickY = 0
    private val stickRepeat = object : Runnable {
        override fun run() {
            if (stickX == 0 && stickY == 0) return
            nudgeBrowserStick()
            if (::webView.isInitialized && !isDestroyed) {
                webView.postDelayed(this, STICK_REPEAT_MS)
            }
        }
    }
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshConnectedControllers()
        override fun onInputDeviceRemoved(deviceId: Int) = refreshConnectedControllers()
        override fun onInputDeviceChanged(deviceId: Int) = refreshConnectedControllers()
    }
    private val settingChangedForwarder: (String) -> Unit = { setting ->
        runOnUiThread {
            webView.evaluateJavascript(
                "onSettingChanged('${setting.replace("'", "\\'")}')",
                null
            )
        }
    }

    private val addGameDirectory = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            contentResolver.takePersistableUriPermission(
                it,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
            Log.d("NxEmu", "Folder selected: $it")

            val path = it.toString()
            val existing = NativeLibrary.getSettingString(NXUISetting.GameDirectories)
            val dirs = try {
                JSONArray(existing)
            } catch (e: Exception) {
                JSONArray()
            }
            val paths = (0 until dirs.length()).map { i -> dirs.getString(i) }
            if (!paths.contains(path)) {
                dirs.put(path)
                NativeLibrary.setSettingString(NXUISetting.GameDirectories, dirs.toString())
                NativeLibrary.saveSettings()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        if (this !is SettingsActivity) {
            installSplashScreen()
        }
        super.onCreate(savedInstanceState)
        webView = WebView(this).apply {
            setBackgroundColor(ThemeHelper.backgroundColor(this@MainActivity))
            settings.javaScriptEnabled = true
            webChromeClient = object : WebChromeClient() {
                override fun onConsoleMessage(msg: ConsoleMessage): Boolean {
                    Log.d("NxEmu-JS", "${msg.message()} [${msg.sourceId()}:${msg.lineNumber()}]")
                    return true
                }

                override fun onJsAlert(
                    view: WebView?,
                    url: String?,
                    message: String?,
                    result: JsResult,
                ): Boolean {
                    android.app.AlertDialog.Builder(this@MainActivity)
                        .setMessage(message)
                        .setPositiveButton(android.R.string.ok) { _, _ -> result.confirm() }
                        .setOnCancelListener { result.confirm() }
                        .show()
                    return true
                }

                override fun onJsConfirm(
                    view: WebView?,
                    url: String?,
                    message: String?,
                    result: JsResult,
                ): Boolean {
                    android.app.AlertDialog.Builder(this@MainActivity)
                        .setMessage(message)
                        .setPositiveButton(android.R.string.ok) { _, _ -> result.confirm() }
                        .setNegativeButton(android.R.string.cancel) { _, _ -> result.cancel() }
                        .setOnCancelListener { result.cancel() }
                        .show()
                    return true
                }
            }
            addJavascriptInterface(NxEmuBridge(this@MainActivity), "NxEmu")
        }
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    if (inputMapping.isShowing()) {
                        inputMapping.dismiss()
                        return
                    }
                    webView.evaluateJavascript("handleAndroidBack()") { result ->
                        if (result != "true" && result != "\"true\"") {
                            finish()
                        }
                    }
                }
            },
        )
        NativeLibrary.addSettingChangedListener(settingChangedForwarder)
        bootstrapControllerOnFirstRun()
        val page = if (this is SettingsActivity) "settings.html" else "index.html"
        webView.loadUrl("file:///android_asset/$page")
        setContentView(webView)
        ThemeHelper.applySystemBars(this)
    }

    private fun bootstrapControllerOnFirstRun() {
        if (InputHandler.getDevices().isEmpty()) {
            return
        }
        if (NativeInput.getButtonParam(0, NativeButton.A).get("engine", "") != "keyboard") {
            return
        }
        InputHandler.updateControllerData()
        ControllerSettings.autoMap(this, playerIndex = 0, index = 0)
        if (NativeInput.getButtonParam(0, NativeButton.A).get("engine", "") == "keyboard") {
            return
        }
        NativeLibrary.setSettingBool(NXUISetting.ShowInputOverlay, false)
        NativeLibrary.saveSettings()
    }

    override fun onResume() {
        super.onResume()
        emulationLaunchPending = false
        InputHandler.updateControllerData()
        inputManager()?.registerInputDeviceListener(inputDeviceListener, null)
        refreshConnectedControllers()
        resumeRunningGame()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        resumeRunningGame()
    }

    private fun resumeRunningGame() {
        if (this is SettingsActivity) {
            return
        }
        val path = EmulationActivity.activeGamePath ?: return
        startActivity(
            Intent(this, EmulationActivity::class.java).apply {
                putExtra(EmulationActivity.EXTRA_GAME_PATH, path)
                addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
            }
        )
    }

    override fun onPause() {
        inputMapping.dismiss()
        inputManager()?.unregisterInputDeviceListener(inputDeviceListener)
        releaseBrowserStick()
        super.onPause()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (inputMapping.onKey(event)) {
            return true
        }
        if (handleBrowserPad(event)) {
            return true
        }
        if (InputHandler.dispatchKeyEvent(event)) {
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun handleBrowserPad(event: KeyEvent): Boolean {
        val script = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_Y ->
                browserPadScript(event, repeat = false, "typeof openSettingsFromPad==='function'&&openSettingsFromPad()")
            KeyEvent.KEYCODE_BUTTON_B ->
                browserPadScript(event, repeat = false, "typeof goBackFromPad==='function'&&goBackFromPad()")
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER ->
                browserPadScript(event, repeat = false, "typeof launchSelectedGame==='function'&&launchSelectedGame()")
            KeyEvent.KEYCODE_DPAD_LEFT ->
                browserPadScript(event, repeat = true, "typeof moveGameSelection==='function'&&moveGameSelection('left')")
            KeyEvent.KEYCODE_DPAD_RIGHT ->
                browserPadScript(event, repeat = true, "typeof moveGameSelection==='function'&&moveGameSelection('right')")
            KeyEvent.KEYCODE_DPAD_UP ->
                browserPadScript(event, repeat = true, "typeof moveGameSelection==='function'&&moveGameSelection('up')")
            KeyEvent.KEYCODE_DPAD_DOWN ->
                browserPadScript(event, repeat = true, "typeof moveGameSelection==='function'&&moveGameSelection('down')")
            else -> return false
        }
        if (script.isNotEmpty() && ::webView.isInitialized && !isDestroyed) {
            webView.evaluateJavascript(script, null)
        }
        return true
    }

    private fun browserPadScript(event: KeyEvent, repeat: Boolean, script: String): String {
        if (event.action != KeyEvent.ACTION_DOWN) return ""
        if (!repeat && event.repeatCount != 0) return ""
        return script
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (inputMapping.onMotion(event)) {
            return true
        }
        if (handleBrowserStick(event)) {
            return true
        }
        if (InputHandler.dispatchGenericMotionEvent(event)) {
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    private fun handleBrowserStick(event: MotionEvent): Boolean {
        val source = event.source
        val fromStick = source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
        if (!fromStick) {
            return false
        }
        val x = stickDirection(event.getAxisValue(MotionEvent.AXIS_X))
        val y = stickDirection(event.getAxisValue(MotionEvent.AXIS_Y))
        if (x == stickX && y == stickY) {
            return x != 0 || y != 0
        }
        stickX = x
        stickY = y
        if (::webView.isInitialized) {
            webView.removeCallbacks(stickRepeat)
        }
        if (x == 0 && y == 0) {
            return false
        }
        nudgeBrowserStick()
        if (::webView.isInitialized && !isDestroyed) {
            webView.postDelayed(stickRepeat, STICK_REPEAT_MS)
        }
        return true
    }

    private fun stickDirection(value: Float): Int {
        return when {
            value > STICK_DEADZONE -> 1
            value < -STICK_DEADZONE -> -1
            else -> 0
        }
    }

    private fun nudgeBrowserStick() {
        if (!::webView.isInitialized || isDestroyed) {
            return
        }
        val moves = buildString {
            if (stickX < 0) append("moveGameSelection('left');")
            if (stickX > 0) append("moveGameSelection('right');")
            if (stickY < 0) append("moveGameSelection('up');")
            if (stickY > 0) append("moveGameSelection('down');")
        }
        if (moves.isEmpty()) {
            return
        }
        webView.evaluateJavascript("if(typeof moveGameSelection==='function'){$moves}", null)
    }
    private fun releaseBrowserStick() {
        stickX = 0
        stickY = 0
        if (::webView.isInitialized) {
            webView.removeCallbacks(stickRepeat)
        }
    }

    fun beginControllerMap(playerIndex: Int, mapId: String, title: String, filterIndex: Int) {
        runOnUiThread {
            inputMapping.begin(playerIndex, mapId, title, filterIndex)
        }
    }

    fun refreshControllerPages() {
        if (!::webView.isInitialized || isDestroyed) {
            return
        }
        webView.post {
            if (!isDestroyed) {
                webView.evaluateJavascript(
                    "typeof renderControllerPages==='function'&&renderControllerPages()",
                    null,
                )
            }
        }
    }

    private fun inputManager(): InputManager? {
        return getSystemService(Context.INPUT_SERVICE) as? InputManager
    }

    private fun refreshConnectedControllers() {
        if (!::webView.isInitialized || isDestroyed) {
            return
        }
        webView.post {
            if (!isDestroyed) {
                webView.evaluateJavascript(
                    "typeof renderControllersSummary==='function'&&renderControllersSummary()",
                    null,
                )
            }
        }
    }

    fun editOverlayLayout() {
        startActivity(Intent(this, OverlayLayoutActivity::class.java))
    }

    override fun onDestroy() {
        NativeLibrary.removeSettingChangedListener(settingChangedForwarder)
        super.onDestroy()
    }

    fun AddGameDirectory() {
        addGameDirectory.launch(null)
    }

    fun launchGame(path: String) {
        if (path.isEmpty() || emulationLaunchPending) {
            return
        }
        emulationLaunchPending = true
        startActivity(
            Intent(this, EmulationActivity::class.java).apply {
                putExtra(EmulationActivity.EXTRA_GAME_PATH, path)
            }
        )
    }

    fun dispatchGameLibraryPaths(gen: Int, json: String) {
        webView.evaluateJavascript(
            "onGameLibraryPaths($gen, ${JSONObject.quote(json)})",
            null,
        )
    }

    companion object {
        private const val STICK_REPEAT_MS = 220L
        private const val STICK_DEADZONE = 0.5f
    }
}

class SettingsActivity : MainActivity()
