package org.nxemu.ui.main

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.drawable.ColorDrawable
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import android.view.HapticFeedbackConstants
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
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
import org.nxemu.utils.GpuDriverHelper
import org.nxemu.utils.InputHandler
import org.nxemu.utils.ThemeHelper
import kotlin.math.abs

open class MainActivity : ComponentActivity() {
    private lateinit var surfaceView: SurfaceView
    private lateinit var textInput: EditText
    private var settingTextInput = false
    private var screen = 0L
    private val inputMapping = InputMappingSession(this) { refreshControllerPages() }
    private var emulationLaunchPending = false
    private val firmware = FirmwareInstallFlow(this) { dispatchUiEvent("firmware", "") }
    val gameData = GameDataActions(this) { dispatchUiEvent("gameDataChanged", "") }
    private var touchPointer = MotionEvent.INVALID_POINTER_ID
    private var touchDownX = 0f
    private var touchDownY = 0f
    private var longPressed = false
    private val longPress = Runnable {
        longPressed = true
        NativeUi.touch(screen, NativeUi.TOUCH_CANCEL, touchDownX, touchDownY)
        surfaceView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        dispatchUiEvent("longPress", "$touchDownX $touchDownY")
    }
    private var stickX = 0
    private var stickY = 0

    private val stickRepeat = object : Runnable {
        override fun run() {
            if (stickX == 0 && stickY == 0) return
            nudgeBrowserStick()
            if (!isDestroyed) {
                surfaceView.postDelayed(this, STICK_REPEAT_MS)
            }
        }
    }

    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshConnectedControllers()
        override fun onInputDeviceRemoved(deviceId: Int) = refreshConnectedControllers()
        override fun onInputDeviceChanged(deviceId: Int) = refreshConnectedControllers()
    }

    private val settingChangedForwarder: (String) -> Unit = { setting ->
        if (setting == NXUISetting.ThemeMode) {
            runOnUiThread { applyWindowTheme() }
        }
        dispatchUiEvent("setting", setting)
    }

    private val addGameDirectory = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            contentResolver.takePersistableUriPermission(
                it,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
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

    private val importGpuDriver = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri == null) {
            return@registerForActivityResult
        }
        Thread({
            val result = GpuDriverHelper.importAndSelect(this, uri)
            runOnUiThread {
                if (isDestroyed) {
                    return@runOnUiThread
                }
                showGpuDriverResult(result)
                refreshGpuDrivers()
            }
        }, "GpuDriverImport").start()
    }

    private val installFirmwareFile = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        uri?.let { firmware.install(it.toString()) }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        if (this !is SettingsActivity) {
            installSplashScreen()
        }
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        bootstrapControllerOnFirstRun()

        NativeUi.start(applicationContext.assets)
        val mode = when {
            this !is SettingsActivity -> NativeUi.MODE_LIBRARY
            intent.getBooleanExtra(SettingsActivity.EXTRA_HUB, false) -> NativeUi.MODE_HUB
            else -> NativeUi.MODE_SETTINGS
        }
        screen = NativeUi.create(UiHost(this), mode)
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {}

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                NativeUi.setSurface(screen, holder.surface, width, height)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                NativeUi.setSurface(screen, null, 0, 0)
            }
        })
        surfaceView.setOnTouchListener { _, event ->
            forwardTouch(event)
            true
        }
        ViewCompat.setOnApplyWindowInsetsListener(surfaceView) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            NativeUi.setMetrics(screen, resources.displayMetrics.density, bars.left, bars.top, bars.right, bars.bottom)
            insets
        }

        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    if (inputMapping.isShowing()) {
                        inputMapping.dismiss()
                        return
                    }
                    dispatchUiEvent("back", "")
                }
            },
        )
        NativeLibrary.addSettingChangedListener(settingChangedForwarder)
        textInput = EditText(this).apply {
            alpha = 0f
            isSingleLine = true
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            imeOptions = EditorInfo.IME_ACTION_SEARCH or EditorInfo.IME_FLAG_NO_EXTRACT_UI
            setOnEditorActionListener { _, _, _ ->
                hideKeyboard()
                true
            }
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
                override fun afterTextChanged(s: Editable?) {
                    if (!settingTextInput) {
                        dispatchUiEvent("searchText", s?.toString() ?: "")
                    }
                }
            })
        }
        setContentView(FrameLayout(this).apply {
            addView(textInput, FrameLayout.LayoutParams(1, 1))
            addView(surfaceView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        })
        applyWindowTheme()
        hideNavigationBar()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideNavigationBar()
        }
    }

    private fun hideNavigationBar() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.navigationBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
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
        applyWindowTheme()
        dispatchUiEvent("resume", "")
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
                putExtra(EmulationActivity.EXTRA_PROGRAM_ID, EmulationActivity.activeProgramId)
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

    override fun onDestroy() {
        firmware.dismiss()
        NativeLibrary.removeSettingChangedListener(settingChangedForwarder)
        surfaceView.removeCallbacks(stickRepeat)
        surfaceView.removeCallbacks(longPress)
        NativeUi.destroy(screen)
        super.onDestroy()
    }

    private fun forwardTouch(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                touchPointer = event.getPointerId(0)
                touchDownX = event.x
                touchDownY = event.y
                longPressed = false
                surfaceView.postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
                NativeUi.touch(screen, NativeUi.TOUCH_DOWN, event.x, event.y)
            }
            MotionEvent.ACTION_MOVE -> {
                val index = event.findPointerIndex(touchPointer)
                if (index >= 0 && !longPressed) {
                    val slop = ViewConfiguration.get(this).scaledTouchSlop
                    if (abs(event.getX(index) - touchDownX) > slop || abs(event.getY(index) - touchDownY) > slop) {
                        surfaceView.removeCallbacks(longPress)
                    }
                    NativeUi.touch(screen, NativeUi.TOUCH_MOVE, event.getX(index), event.getY(index))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) == touchPointer) {
                    surfaceView.removeCallbacks(longPress)
                    if (!longPressed) {
                        NativeUi.touch(screen, NativeUi.TOUCH_UP, event.getX(event.actionIndex), event.getY(event.actionIndex))
                    }
                    touchPointer = MotionEvent.INVALID_POINTER_ID
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                surfaceView.removeCallbacks(longPress)
                if (!longPressed) {
                    NativeUi.touch(screen, NativeUi.TOUCH_CANCEL, event.x, event.y)
                }
                touchPointer = MotionEvent.INVALID_POINTER_ID
            }
        }
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
        val (button, repeats) = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_Y -> "y" to false
            KeyEvent.KEYCODE_BUTTON_B -> "b" to false
            KeyEvent.KEYCODE_BUTTON_X -> "x" to false
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> "a" to false
            KeyEvent.KEYCODE_DPAD_LEFT -> "left" to true
            KeyEvent.KEYCODE_DPAD_RIGHT -> "right" to true
            KeyEvent.KEYCODE_DPAD_UP -> "up" to true
            KeyEvent.KEYCODE_DPAD_DOWN -> "down" to true
            else -> return false
        }
        if (event.action == KeyEvent.ACTION_DOWN && (repeats || event.repeatCount == 0)) {
            dispatchUiEvent("pad", button)
        }
        return true
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
        surfaceView.removeCallbacks(stickRepeat)
        if (x == 0 && y == 0) {
            return false
        }
        nudgeBrowserStick()
        if (!isDestroyed) {
            surfaceView.postDelayed(stickRepeat, STICK_REPEAT_MS)
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
        if (isDestroyed) {
            return
        }
        if (stickX < 0) dispatchUiEvent("pad", "left")
        if (stickX > 0) dispatchUiEvent("pad", "right")
        if (stickY < 0) dispatchUiEvent("pad", "up")
        if (stickY > 0) dispatchUiEvent("pad", "down")
    }

    private fun releaseBrowserStick() {
        stickX = 0
        stickY = 0
        surfaceView.removeCallbacks(stickRepeat)
    }

    fun dispatchUiEvent(name: String, payload: String) {
        if (screen != 0L) {
            NativeUi.event(screen, name, payload)
        }
    }

    fun beginControllerMap(playerIndex: Int, mapId: String, title: String, filterIndex: Int) {
        runOnUiThread {
            inputMapping.begin(playerIndex, mapId, title, filterIndex)
        }
    }

    fun refreshControllerPages() = dispatchUiEvent("controllerPages", "")

    private fun refreshConnectedControllers() = dispatchUiEvent("controllers", "")

    fun refreshGpuDrivers() = dispatchUiEvent("gpuDrivers", "")

    private fun inputManager(): InputManager? {
        return getSystemService(Context.INPUT_SERVICE) as? InputManager
    }

    fun confirm(id: Int, message: String) {
        if (isDestroyed) {
            return
        }
        val answer = { ok: Boolean ->
            dispatchUiEvent("confirm", JSONObject().put("id", id).put("ok", ok).toString())
        }
        AlertDialog.Builder(this)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { _, _ -> answer(true) }
            .setNegativeButton(android.R.string.cancel) { _, _ -> answer(false) }
            .setOnCancelListener { answer(false) }
            .show()
            .window?.setGravity(Gravity.CENTER)
    }

    fun alert(message: String) {
        if (isDestroyed) {
            return
        }
        AlertDialog.Builder(this)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
            .window?.setGravity(Gravity.CENTER)
    }

    fun editOverlayLayout() {
        startActivity(Intent(this, OverlayLayoutActivity::class.java))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(Activity.OVERRIDE_TRANSITION_OPEN, 0, 0)
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    fun installGpuDriver() {
        importGpuDriver.launch(arrayOf("application/zip", "application/octet-stream"))
    }

    fun showGpuDriverResult(result: String) {
        if (isDestroyed) {
            return
        }
        val message = when (result) {
            "ok" -> "GPU driver installed. It applies the next time a game starts."
            "unsupported" -> "Custom GPU drivers need an Adreno GPU."
            "api" -> "This driver needs a newer Android version."
            "invalid" -> "That file is not a GPU driver."
            "network" -> "Download failed. Check your connection and try again."
            "busy" -> "A driver is already downloading."
            "missing" -> "That driver is no longer listed. Refresh and try again."
            else -> "Couldn't read that driver."
        }
        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
    }

    fun beginTextInput(text: String) {
        settingTextInput = true
        textInput.setText(text)
        textInput.setSelection(text.length)
        settingTextInput = false
        textInput.requestFocus()
        (getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).showSoftInput(textInput, InputMethodManager.SHOW_IMPLICIT)
    }

    fun endTextInput() {
        hideKeyboard()
        textInput.clearFocus()
    }

    private fun hideKeyboard() {
        (getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(textInput.windowToken, 0)
    }

    fun addGameDirectory() {
        addGameDirectory.launch(null)
    }

    fun installFirmware() {
        if (!firmware.busy) {
            installFirmwareFile.launch(arrayOf("*/*"))
        }
    }

    fun firmwareVersion(): String =
        if (firmware.busy) "" else NativeLibrary.getFirmwareVersion().let { if (it == "N/A") "" else it }

    fun launchGame(path: String, programId: Long) {
        if (path.isEmpty() || emulationLaunchPending || firmware.busy) {
            return
        }
        emulationLaunchPending = true
        firmware.offerBeforeLaunch(
            path,
            launch = { startEmulation(path, programId) },
            cancel = { emulationLaunchPending = false },
        )
    }

    private fun startEmulation(path: String, programId: Long) {
        emulationLaunchPending = true
        startActivity(
            Intent(this, EmulationActivity::class.java).apply {
                putExtra(EmulationActivity.EXTRA_GAME_PATH, path)
                putExtra(EmulationActivity.EXTRA_PROGRAM_ID, programId)
            }
        )
    }

    fun dispatchGameLibraryPaths(gen: Int, json: String) {
        val paths = try {
            JSONArray(json)
        } catch (e: Exception) {
            JSONArray()
        }
        dispatchUiEvent("libraryPaths", JSONObject().put("gen", gen).put("paths", paths).toString())
    }

    private fun applyWindowTheme() {
        window.setBackgroundDrawable(ColorDrawable(ThemeHelper.backgroundColor(this)))
        ThemeHelper.applySystemBars(this)
    }

    companion object {
        private const val STICK_REPEAT_MS = 220L
        private const val STICK_DEADZONE = 0.5f
    }
}

class SettingsActivity : MainActivity() {
    companion object {
        const val EXTRA_HUB = "hub"
    }
}
