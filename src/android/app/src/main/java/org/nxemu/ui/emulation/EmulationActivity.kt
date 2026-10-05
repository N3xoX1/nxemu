package org.nxemu.ui.emulation

import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.graphics.PixelFormat
import android.graphics.drawable.AnimatedImageDrawable
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Base64
import android.util.Log
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.drawerlayout.widget.DrawerLayout
import org.nxemu.NXCoreSetting
import org.nxemu.NXUISetting
import org.nxemu.NativeLibrary
import org.nxemu.R
import org.nxemu.overlay.InputOverlay
import org.nxemu.overlay.model.OverlayLayout
import org.nxemu.utils.InputHandler
import org.nxemu.utils.NativeConfig
import org.json.JSONObject
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class EmulationActivity : ComponentActivity(), SurfaceHolder.Callback {
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "nxemu-emu").apply { isDaemon = true }
    }
    private val nativeSurfaceSessionOpen = AtomicBoolean(false)
    private var explicitExit = false
    private var leavingApp = false
    private lateinit var pausedFrameImage: ImageView
    private val captureInFlight = AtomicBoolean(false)
    private val pausedFrameListener = { updatePauseLabel() }
    private val perfStatsHandler = Handler(Looper.getMainLooper())
    private var perfStatsUpdater: Runnable? = null
    private lateinit var loadingIndicator: View
    private lateinit var loadingCornerLogo: ImageView
    private lateinit var loadingCornerBanner: ImageView
    private lateinit var loadingImage: ImageView
    private lateinit var loadingTitle: TextView
    private lateinit var showFpsText: TextView
    private lateinit var showDeviceText: TextView
    private lateinit var surfaceInputOverlay: InputOverlay
    private lateinit var drawerLayout: DrawerLayout
    private lateinit var pauseItem: TextView
    private lateinit var pausedIcon: ImageView
    private lateinit var lockDrawerItem: TextView
    private lateinit var overlayAppVersion: String
    private lateinit var overlayPhoneModel: String
    private lateinit var overlaySoc: String
    private var drawerStickY = 0
    private val drawerStickRepeat = object : Runnable {
        override fun run() {
            if (drawerStickY == 0 || !::drawerLayout.isInitialized || !drawerLayout.isDrawerOpen(Gravity.START)) {
                return
            }
            moveDrawerFocus(drawerStickY)
            perfStatsHandler.postDelayed(this, DRAWER_REPEAT_MS)
        }
    }

    private val settingChangedListener: (String) -> Unit = { setting ->
        if (setting == NXCoreSetting.DisplayedFrames) {
            runOnUiThread { hideLoadingIfFirstFrame() }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_emulation)

        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        loadingIndicator = findViewById(R.id.loading_indicator)
        loadingCornerLogo = findViewById(R.id.loading_corner_logo)
        loadingCornerBanner = findViewById(R.id.loading_corner_banner)
        loadingImage = findViewById(R.id.loading_image)
        loadingTitle = findViewById(R.id.loading_title)
        loadingTitle.text = getString(R.string.app_name)
        showFpsText = findViewById(R.id.show_fps_text)
        showDeviceText = findViewById(R.id.show_device_text)
        surfaceInputOverlay = findViewById(R.id.surface_input_overlay)
        surfaceInputOverlay.setZOrderMediaOverlay(true)
        surfaceInputOverlay.holder.setFormat(PixelFormat.TRANSLUCENT)
        drawerLayout = findViewById(R.id.drawer_layout)
        pauseItem = findViewById(R.id.menu_pause)
        pausedIcon = findViewById(R.id.paused_icon)
        pausedFrameImage = findViewById(R.id.paused_frame_image)
        frameListener = pausedFrameListener
        pausedIcon.setOnClickListener {
            if (NativeLibrary.isPaused()) {
                resumeAfterBackground.set(false)
                NativeLibrary.unpauseEmulation()
                clearPausedFrame()
                updatePauseLabel()
            }
        }
        lockDrawerItem = findViewById(R.id.menu_lock_drawer)
        setupInGameDrawer()
        updateInputOverlayLayout()
        cacheDeviceOverlayInfo()

        NativeLibrary.addSettingChangedListener(settingChangedListener)
        hideLoadingIfFirstFrame()

        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    if (drawerLayout.isDrawerOpen(Gravity.START)) {
                        drawerLayout.closeDrawer(Gravity.START)
                    } else {
                        drawerLayout.openDrawer(Gravity.START)
                    }
                }
            },
        )

        val path = intent.getStringExtra(EXTRA_GAME_PATH)
        if (path.isNullOrEmpty()) {
            Log.e(TAG, "Missing EXTRA_GAME_PATH")
            finish()
            return
        }
        executor.execute { loadRomInfo(path) }

        findViewById<SurfaceView>(R.id.emulation_surface).holder.addCallback(this)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemBars()
        }
    }

    override fun onUserLeaveHint() {
        leavingApp = true
        super.onUserLeaveHint()
    }

    override fun onResume() {
        super.onResume()
        leavingApp = false
        val surface = findViewById<SurfaceView>(R.id.emulation_surface).holder.surface
        if (surface != null && surface.isValid) {
            unpauseIfBackgrounded()
        }
        updatePauseLabel()
        if (NativeLibrary.isPaused() && pausedFrame == null) {
            capturePausedFrame()
        }
        InputHandler.updateControllerData()
        if (::surfaceInputOverlay.isInitialized) {
            updateInputOverlayLayout()
            surfaceInputOverlay.refreshControls()
        }
    }

    override fun onPause() {
        releaseDrawerStick()
        val leftApp = leavingApp
        if (!NativeLibrary.isPaused()) {
            NativeLibrary.pauseEmulation()
            if (!leftApp) {
                resumeAfterBackground.set(true)
            }
        }
        if (leftApp) {
            capturePausedFrame()
        }
        leavingApp = false
        super.onPause()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            return super.dispatchKeyEvent(event)
        }
        val source = event.source
        val fromGamepad = source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (!fromGamepad) {
            return super.dispatchKeyEvent(event)
        }
        if (::drawerLayout.isInitialized && drawerLayout.isDrawerOpen(Gravity.START)) {
            handleDrawerPad(event)
            return true
        }
        if (InputHandler.dispatchKeyEvent(event)) {
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val fromStick = event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
        if (fromStick && ::drawerLayout.isInitialized && drawerLayout.isDrawerOpen(Gravity.START)) {
            handleDrawerStick(event)
            return true
        }
        if (InputHandler.dispatchGenericMotionEvent(event)) {
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    private fun setupInGameDrawer() {
        drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED)
        drawerLayout.addDrawerListener(object : DrawerLayout.SimpleDrawerListener() {
            override fun onDrawerSlide(drawerView: View, slideOffset: Float) {
                releaseOverlayTouches()
                if (slideOffset > 0f) {
                    surfaceInputOverlay.visibility = View.INVISIBLE
                }
            }

            override fun onDrawerOpened(drawerView: View) {
                drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_UNLOCKED)
                updatePauseLabel()
                drawerMenuItems().first().requestFocus()
            }

            override fun onDrawerClosed(drawerView: View) {
                releaseDrawerStick()
                drawerLayout.setDrawerLockMode(savedDrawerLockMode())
                if (loadingIndicator.visibility != View.VISIBLE) {
                    surfaceInputOverlay.visibility = View.VISIBLE
                }
            }
        })
        pauseItem.setOnClickListener { togglePause() }
        lockDrawerItem.setOnClickListener { toggleDrawerLock() }
        findViewById<View>(R.id.menu_exit).setOnClickListener {
            drawerLayout.closeDrawers()
            explicitExit = true
            activeGamePath = null
            clearPausedFrame()
            finish()
        }
        updatePauseLabel()
        updateLockDrawerLabel()
    }

    private fun togglePause() {
        if (NativeLibrary.isPaused()) {
            NativeLibrary.unpauseEmulation()
            clearPausedFrame()
        } else {
            NativeLibrary.pauseEmulation()
            capturePausedFrame()
        }
        updatePauseLabel()
    }

    private fun updatePauseLabel() {
        val paused = NativeLibrary.isPaused()
        pauseItem.text = getString(
            if (paused) R.string.emulation_unpause else R.string.emulation_pause,
        )
        if (::pausedIcon.isInitialized) {
            pausedIcon.visibility = if (paused) View.VISIBLE else View.GONE
        }
        if (::pausedFrameImage.isInitialized) {
            val frame = if (paused) pausedFrame else null
            pausedFrameImage.setImageBitmap(frame)
            pausedFrameImage.visibility = if (frame != null) View.VISIBLE else View.GONE
        }
    }

    private fun capturePausedFrame() {
        if (!captureInFlight.compareAndSet(false, true)) {
            return
        }
        executor.execute {
            try {
                val width = NativeLibrary.getAppletCaptureWidth()
                val height = NativeLibrary.getAppletCaptureHeight()
                val frame = NativeLibrary.getAppletCaptureBuffer()
                val expected = width * height * 4
                if (width <= 0 || height <= 0 || frame.size < expected) {
                    return@execute
                }
                val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(frame, 0, expected))
                val previous = pausedFrame
                pausedFrame = bitmap
                Handler(Looper.getMainLooper()).post {
                    frameListener?.invoke()
                    if (previous != null && previous !== pausedFrame) {
                        previous.recycle()
                    }
                }
            } finally {
                captureInFlight.set(false)
            }
        }
    }

    private fun clearPausedFrame() {
        val previous = pausedFrame
        pausedFrame = null
        if (::pausedFrameImage.isInitialized) {
            pausedFrameImage.setImageDrawable(null)
            pausedFrameImage.visibility = View.GONE
        }
        previous?.recycle()
    }

    private fun releaseOverlayTouches() {
        val event = MotionEvent.obtain(
            SystemClock.uptimeMillis(),
            SystemClock.uptimeMillis(),
            MotionEvent.ACTION_UP,
            0f,
            0f,
            0,
        )
        surfaceInputOverlay.dispatchTouchEvent(event)
        event.recycle()
    }

    private fun savedDrawerLockMode(): Int {
        return if (NativeLibrary.getSettingBool(NXUISetting.LockDrawer)) {
            DrawerLayout.LOCK_MODE_LOCKED_CLOSED
        } else {
            DrawerLayout.LOCK_MODE_UNLOCKED
        }
    }

    private fun toggleDrawerLock() {
        NativeLibrary.setSettingBool(NXUISetting.LockDrawer, !NativeLibrary.getSettingBool(NXUISetting.LockDrawer))
        updateLockDrawerLabel()
        NativeConfig.saveGlobalConfig()
    }

    private fun updateLockDrawerLabel() {
        lockDrawerItem.text = getString(
            if (savedDrawerLockMode() == DrawerLayout.LOCK_MODE_LOCKED_CLOSED) {
                R.string.unlock_drawer
            } else {
                R.string.lock_drawer
            },
        )
    }

    private fun drawerMenuItems(): List<View> {
        return listOf(
            pauseItem,
            lockDrawerItem,
            findViewById(R.id.menu_exit),
        )
    }

    private fun handleDrawerPad(event: KeyEvent) {
        val down = event.action == KeyEvent.ACTION_DOWN
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_B -> {
                if (down && event.repeatCount == 0) {
                    drawerLayout.closeDrawer(Gravity.START)
                }
            }
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                if (down && event.repeatCount == 0) {
                    currentDrawerItem().performClick()
                }
            }
            KeyEvent.KEYCODE_DPAD_UP -> if (down) moveDrawerFocus(-1)
            KeyEvent.KEYCODE_DPAD_DOWN -> if (down) moveDrawerFocus(1)
        }
    }

    private fun handleDrawerStick(event: MotionEvent) {
        val y = when {
            event.getAxisValue(MotionEvent.AXIS_Y) > DRAWER_STICK_DEADZONE -> 1
            event.getAxisValue(MotionEvent.AXIS_Y) < -DRAWER_STICK_DEADZONE -> -1
            else -> 0
        }
        if (y == drawerStickY) {
            return
        }
        drawerStickY = y
        perfStatsHandler.removeCallbacks(drawerStickRepeat)
        if (y == 0) {
            return
        }
        moveDrawerFocus(y)
        perfStatsHandler.postDelayed(drawerStickRepeat, DRAWER_REPEAT_MS)
    }

    private fun releaseDrawerStick() {
        drawerStickY = 0
        perfStatsHandler.removeCallbacks(drawerStickRepeat)
    }

    private fun moveDrawerFocus(delta: Int) {
        val items = drawerMenuItems()
        if (items.isEmpty()) {
            return
        }
        val current = items.indexOfFirst { it.hasFocus() }.let { if (it < 0) 0 else it }
        items[(current + delta).coerceIn(0, items.lastIndex)].requestFocus()
    }

    private fun currentDrawerItem(): View {
        val items = drawerMenuItems()
        return items.firstOrNull { it.hasFocus() } ?: items.first().also { it.requestFocus() }
    }

    private fun hideSystemBars() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun hideLoadingIfFirstFrame() {
        if (NativeLibrary.getSettingBool(NXCoreSetting.DisplayedFrames)) {
            stopAnimatedDrawables()
            loadingIndicator.visibility = View.GONE
            startPerfOverlay()
            if (!drawerLayout.isDrawerOpen(Gravity.START)) {
                surfaceInputOverlay.visibility = View.VISIBLE
            }
            drawerLayout.setDrawerLockMode(savedDrawerLockMode())
            updateInputOverlayLayout()
            surfaceInputOverlay.refreshControls()
        }
    }

    private fun startPerfOverlay() {
        if (perfStatsUpdater != null) {
            return
        }
        showFpsText.visibility = View.VISIBLE
        showDeviceText.visibility = View.VISIBLE
        val updater = object : Runnable {
            override fun run() {
                if (isDestroyed) {
                    return
                }
                val stats = NativeLibrary.getPerfStats()
                val fps = if (stats.size > 1) stats[1] else 0.0
                val shaders = NativeLibrary.getShadersBuilding()
                var fpsLine = String.format(Locale.US, "FPS: %.1f", fps)
                if (shaders > 0) {
                    val shaderLabel = if (shaders == 1) "shader" else "shaders"
                    fpsLine += String.format(Locale.US, " | Building: %d %s", shaders, shaderLabel)
                }
                showFpsText.text = fpsLine
                showDeviceText.text = deviceOverlayLine()
                perfStatsHandler.postDelayed(this, 800)
            }
        }
        perfStatsUpdater = updater
        perfStatsHandler.post(updater)
    }

    private fun stopPerfOverlay() {
        perfStatsUpdater?.let { perfStatsHandler.removeCallbacks(it) }
        perfStatsUpdater = null
    }

    private fun updateInputOverlayLayout() {
        surfaceInputOverlay.layout =
            if (resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT) {
                OverlayLayout.Portrait
            } else {
                OverlayLayout.Landscape
            }
    }

    private fun cacheDeviceOverlayInfo() {
        overlayAppVersion = NativeLibrary.getAppVersion()
        overlayPhoneModel = Build.MODEL.ifBlank { "N/A" }
        overlaySoc = if (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL.isNotBlank()) {
            Build.SOC_MODEL
        } else {
            Build.HARDWARE.ifBlank { "N/A" }
        }
    }

    private fun deviceOverlayLine(): String {
        val firmware = NativeLibrary.getFirmwareVersion().ifBlank { "N/A" }
        return listOf(
            overlayAppVersion,
            overlayPhoneModel,
            overlaySoc,
            firmware,
        ).joinToString(" | ")
    }

    private fun loadRomInfo(path: String) {
        try {
            val json = JSONObject(NativeLibrary.queryRomInfo(path))
            val title = json.optString("title")
            val icon = json.optString("icon")
            val logo = json.optString("logo")
            val banner = json.optString("banner")
            runOnUiThread {
                if (isDestroyed) {
                    return@runOnUiThread
                }
                if (title.isNotEmpty()) {
                    loadingTitle.text = title
                }
                applyLoadingArtwork(logo, banner, icon)
            }
        } catch (e: Exception) {
            Log.w(TAG, "queryRomInfo failed", e)
        }
    }

    private fun applyLoadingArtwork(logo: String, banner: String, icon: String) {
        applyImage(loadingCornerLogo, logo, hideIfEmpty = true)
        applyImage(loadingCornerBanner, banner, hideIfEmpty = true)
        if (icon.isNotEmpty()) {
            applyImage(loadingImage, icon, hideIfEmpty = false)
        }
    }

    private fun applyImage(imageView: ImageView, base64: String?, hideIfEmpty: Boolean) {
        if (base64.isNullOrEmpty()) {
            if (hideIfEmpty) {
                imageView.visibility = View.GONE
            }
            return
        }
        val drawable = decodeDrawable(base64)
        if (drawable == null) {
            if (hideIfEmpty) {
                imageView.visibility = View.GONE
            }
            return
        }
        (imageView.drawable as? AnimatedImageDrawable)?.stop()
        imageView.setImageDrawable(drawable)
        (drawable as? AnimatedImageDrawable)?.start()
        imageView.visibility = View.VISIBLE
    }

    private fun decodeDrawable(base64: String): Drawable? {
        return try {
            val bytes = Base64.decode(base64, Base64.DEFAULT)
            try {
                ImageDecoder.decodeDrawable(ImageDecoder.createSource(ByteBuffer.wrap(bytes))) { decoder, _, _ ->
                    decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                }
            } catch (_: Exception) {
                val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return null
                BitmapDrawable(resources, bitmap)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to decode loading image", e)
            null
        }
    }

    private fun stopAnimatedDrawables() {
        listOf(loadingCornerLogo, loadingCornerBanner, loadingImage).forEach { view ->
            (view.drawable as? AnimatedImageDrawable)?.stop()
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (width <= 0 || height <= 0) {
            return
        }
        val path = intent.getStringExtra(EXTRA_GAME_PATH)
        if (path.isNullOrEmpty()) {
            Log.e(TAG, "Missing EXTRA_GAME_PATH")
            finish()
            return
        }
        val surface = holder.surface
        val ratio = resources.displayMetrics.density
        executor.execute {
            try {
                if (!nativeSurfaceSessionOpen.get()) {
                    if (!NativeLibrary.emulationSurfaceReady(surface, ratio, path)) {
                        Log.e(TAG, "emulationSurfaceReady returned false")
                        runOnUiThread { finish() }
                    } else {
                        nativeSurfaceSessionOpen.set(true)
                        activeGamePath = path
                        InputHandler.updateControllerData()
                        unpauseIfBackgrounded()
                        runOnUiThread { updatePauseLabel() }
                    }
                } else {
                    NativeLibrary.surfaceChanged(surface)
                    unpauseIfBackgrounded()
                    runOnUiThread { updatePauseLabel() }
                }
            } catch (e: Throwable) {
                Log.e(TAG, "surfaceChanged failed", e)
                if (!nativeSurfaceSessionOpen.get()) {
                    runOnUiThread { finish() }
                }
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        try {
            executor.submit {
                NativeLibrary.emulationSurfaceDestroyed()
            }.get(120, TimeUnit.SECONDS)
        } catch (e: Exception) {
            Log.e(TAG, "emulationSurfaceDestroyed failed", e)
        }
    }

    override fun onDestroy() {
        if (frameListener === pausedFrameListener) {
            frameListener = null
        }
        NativeLibrary.removeSettingChangedListener(settingChangedListener)
        stopPerfOverlay()
        stopAnimatedDrawables()
        if (explicitExit && nativeSurfaceSessionOpen.get()) {
            try {
                executor.submit {
                    try {
                        NativeLibrary.emulationStopped()
                    } finally {
                        nativeSurfaceSessionOpen.set(false)
                    }
                }.get(60, TimeUnit.SECONDS)
            } catch (e: Exception) {
                Log.e(TAG, "onDestroy native teardown failed", e)
                nativeSurfaceSessionOpen.set(false)
            }
        }
        executor.shutdown()
        try {
            if (!executor.awaitTermination(30, TimeUnit.SECONDS)) {
                executor.shutdownNow()
            }
        } catch (_: InterruptedException) {
            executor.shutdownNow()
        }
        super.onDestroy()
    }

    private fun unpauseIfBackgrounded() {
        if (resumeAfterBackground.compareAndSet(true, false)) {
            NativeLibrary.unpauseEmulation()
            runOnUiThread {
                clearPausedFrame()
                updatePauseLabel()
            }
        }
    }

    companion object {
        private val resumeAfterBackground = AtomicBoolean(false)
        var activeGamePath: String? = null
        var pausedFrame: Bitmap? = null
        var frameListener: (() -> Unit)? = null
        const val EXTRA_GAME_PATH = "org.nxemu.EXTRA_GAME_PATH"
        private const val TAG = "NxEmu-Emulation"
        private const val DRAWER_REPEAT_MS = 220L
        private const val DRAWER_STICK_DEADZONE = 0.5f
    }
}
