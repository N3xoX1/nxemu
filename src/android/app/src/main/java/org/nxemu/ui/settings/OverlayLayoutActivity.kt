package org.nxemu.ui.settings

import android.app.Activity
import android.content.res.Configuration
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.View
import android.widget.Button
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.nxemu.R
import org.nxemu.overlay.InputOverlay
import org.nxemu.overlay.model.OverlayLayout
import org.nxemu.utils.NativeConfig

class OverlayLayoutActivity : ComponentActivity() {
    private lateinit var surfaceInputOverlay: InputOverlay

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_overlay_layout)

        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        surfaceInputOverlay = findViewById(R.id.surface_input_overlay)
        surfaceInputOverlay.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                paintOpaqueBackground(holder)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                paintOpaqueBackground(holder)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
        })
        surfaceInputOverlay.layout =
            if (resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT) {
                OverlayLayout.Portrait
            } else {
                OverlayLayout.Landscape
            }
        surfaceInputOverlay.visibility = View.VISIBLE
        surfaceInputOverlay.requestFocus()
        surfaceInputOverlay.setIsInEditMode(true)

        findViewById<View>(R.id.overlay_edit_actions).bringToFront()
        findViewById<Button>(R.id.reset_control_config).setOnClickListener {
            surfaceInputOverlay.resetLayoutVisibilityAndPlacement()
        }
        findViewById<Button>(R.id.done_control_config).setOnClickListener { finishEditing() }
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    finishEditing()
                }
            },
        )
    }

    override fun finish() {
        super.finish()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(Activity.OVERRIDE_TRANSITION_CLOSE, 0, 0)
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    private fun finishEditing() {
        if (::surfaceInputOverlay.isInitialized) {
            surfaceInputOverlay.setIsInEditMode(false)
        }
        NativeConfig.saveGlobalConfig()
        finish()
    }

    private fun paintOpaqueBackground(holder: SurfaceHolder) {
        val canvas = holder.lockCanvas() ?: return
        canvas.drawColor(Color.BLACK)
        holder.unlockCanvasAndPost(canvas)
    }
}
