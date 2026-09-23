package org.nxemu.utils

import android.content.Context
import android.content.res.Configuration
import android.graphics.Color
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import org.nxemu.NXUISetting
import org.nxemu.NativeLibrary

object ThemeHelper {
    const val FOLLOW_SYSTEM = 0
    const val LIGHT = 1
    const val DARK = 2

    fun mode(): Int = NativeLibrary.getSettingInt(NXUISetting.ThemeMode)

    fun isDark(context: Context): Boolean {
        return when (mode()) {
            LIGHT -> false
            DARK -> true
            else -> {
                val night = context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK
                night == Configuration.UI_MODE_NIGHT_YES
            }
        }
    }

    fun backgroundColor(context: Context): Int =
        if (isDark(context)) Color.parseColor("#121212") else Color.WHITE

    fun applySystemBars(activity: ComponentActivity) {
        val dark = isDark(activity)
        val controller = WindowCompat.getInsetsController(activity.window, activity.window.decorView)
        controller.isAppearanceLightStatusBars = !dark
        controller.isAppearanceLightNavigationBars = !dark
    }
}
