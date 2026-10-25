package org.nxemu.ui.main

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
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

open class MainActivity : ComponentActivity() {
    private lateinit var surfaceView: SurfaceView
    private var screen = 0L
    private val inputMapping = InputMappingSession(this) { refreshControllerPages() }
    private var emulationLaunchPending = false
    private var stickX = 0
    private var stickY = 0
    private val settingChangedForwarder: (String) -> Unit = { setting ->
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

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        if (this !is SettingsActivity) {
            installSplashScreen()
        }
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        bootstrapControllerOnFirstRun()

        NativeUi.start(applicationContext.assets)
        screen = NativeUi.create(UiHost(this), this is SettingsActivity)
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
        setContentView(surfaceView)
    }

    private fun bootstrapControllerOnFirstRun() {
    }

    private fun resumeRunningGame() {
    }

    override fun onDestroy() {
        NativeUi.destroy(screen)
        super.onDestroy()
    }

    private fun forwardTouch(event: MotionEvent) {
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        return super.dispatchKeyEvent(event)
    }

    private fun handleBrowserPad(event: KeyEvent): Boolean {
        return true
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

    fun refreshGpuDrivers() = dispatchUiEvent("gpuDrivers", "")

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
    }

    fun alert(message: String) {
        if (isDestroyed) {
            return
        }
        AlertDialog.Builder(this)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
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

    fun addGameDirectory() {
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
        val paths = try {
            JSONArray(json)
        } catch (e: Exception) {
            JSONArray()
        }
        dispatchUiEvent("libraryPaths", JSONObject().put("gen", gen).put("paths", paths).toString())
    }
}

class SettingsActivity : MainActivity()
