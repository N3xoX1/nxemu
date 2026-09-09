package org.nxemu.ui.emulation

import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.nxemu.NativeLibrary
import org.nxemu.R
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class EmulationActivity : ComponentActivity(), SurfaceHolder.Callback {
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "nxemu-emu").apply { isDaemon = true }
    }
    private val nativeSurfaceSessionOpen = AtomicBoolean(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_emulation)

        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        findViewById<SurfaceView>(R.id.emulation_surface).holder.addCallback(this)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemBars()
        }
    }

    private fun hideSystemBars() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
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
