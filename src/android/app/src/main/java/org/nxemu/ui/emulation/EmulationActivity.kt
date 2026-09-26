package org.nxemu.ui.emulation

import android.content.res.Configuration
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
    private lateinit var lockDrawerItem: TextView
    private lateinit var overlayAppVersion: String
    private lateinit var overlayPhoneModel: String
    private lateinit var overlaySoc: String

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
                findViewById<View>(R.id.in_game_menu).requestFocus()
            }

            override fun onDrawerClosed(drawerView: View) {
                drawerLayout.setDrawerLockMode(savedDrawerLockMode())
                if (loadingIndicator.visibility != View.VISIBLE) {
                    surfaceInputOverlay.visibility = View.VISIBLE
                }
            }
        })
        lockDrawerItem.setOnClickListener { toggleDrawerLock() }
        findViewById<View>(R.id.menu_exit).setOnClickListener {
            drawerLayout.closeDrawers()
            finish()
        }
        updateLockDrawerLabel()
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
        val mode = NativeLibrary.getSettingInt(NXUISetting.LockDrawer)
        return if (mode == DrawerLayout.LOCK_MODE_LOCKED_CLOSED) {
            DrawerLayout.LOCK_MODE_LOCKED_CLOSED
        } else {
            DrawerLayout.LOCK_MODE_UNLOCKED
        }
    }

    private fun toggleDrawerLock() {
        val mode = if (savedDrawerLockMode() == DrawerLayout.LOCK_MODE_UNLOCKED) {
            DrawerLayout.LOCK_MODE_LOCKED_CLOSED
        } else {
            DrawerLayout.LOCK_MODE_UNLOCKED
        }
        NativeLibrary.setSettingInt(NXUISetting.LockDrawer, mode)
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

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (!nativeSurfaceSessionOpen.compareAndSet(false, true)) {
            Log.w(TAG, "Ignoring extra surfaceCreated")
            return
        }
        val path = intent.getStringExtra(EXTRA_GAME_PATH)
        if (path.isNullOrEmpty()) {
            Log.e(TAG, "Missing EXTRA_GAME_PATH")
            nativeSurfaceSessionOpen.set(false)
            finish()
            return
        }
        val ratio = resources.displayMetrics.density
        val surface = holder.surface
        executor.execute {
            try {
                if (!NativeLibrary.emulationSurfaceReady(surface, ratio, path)) {
                    Log.e(TAG, "emulationSurfaceReady returned false")
                    nativeSurfaceSessionOpen.set(false)
                    runOnUiThread { finish() }
                }
            } catch (e: Throwable) {
                Log.e(TAG, "emulationSurfaceReady failed", e)
                nativeSurfaceSessionOpen.set(false)
                runOnUiThread { finish() }
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (!nativeSurfaceSessionOpen.get() || width <= 0 || height <= 0) {
            return
        }
        val surface = holder.surface
        executor.execute {
            try {
                NativeLibrary.surfaceChanged(surface)
            } catch (e: Throwable) {
                Log.e(TAG, "surfaceChanged failed", e)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        try {
            executor.submit {
                try {
                    NativeLibrary.emulationSurfaceDestroyed()
                } finally {
                    nativeSurfaceSessionOpen.set(false)
                }
            }.get(120, TimeUnit.SECONDS)
        } catch (e: Exception) {
            Log.e(TAG, "emulationSurfaceDestroyed failed", e)
            nativeSurfaceSessionOpen.set(false)
        }
    }

    override fun onDestroy() {
        NativeLibrary.removeSettingChangedListener(settingChangedListener)
        stopPerfOverlay()
        stopAnimatedDrawables()
        if (nativeSurfaceSessionOpen.get()) {
            try {
                executor.submit {
                    try {
                        NativeLibrary.emulationSurfaceDestroyed()
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

    companion object {
        const val EXTRA_GAME_PATH = "org.nxemu.EXTRA_GAME_PATH"
        private const val TAG = "NxEmu-Emulation"
    }
}
