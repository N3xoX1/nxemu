package org.nxemu.ui.main

import android.content.res.Configuration
import android.webkit.JavascriptInterface
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary

class NxEmuBridge(private val activity: MainActivity) {
    @JavascriptInterface
    fun addGameDirectory() {
        activity.runOnUiThread { activity.AddGameDirectory() }
    }

    @JavascriptInterface
    fun setSettingString(setting: String, value: String) {
        NativeLibrary.setSettingString(setting, value)
    }

    @JavascriptInterface
    fun getSettingString(setting: String): String {
        return NativeLibrary.getSettingString(setting)
    }

    @JavascriptInterface
    fun saveSettings() {
        NativeLibrary.saveSettings()
    }

    @JavascriptInterface
    fun isDarkTheme(): Boolean {
        val night = activity.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK
        return night == Configuration.UI_MODE_NIGHT_YES
    }

    @JavascriptInterface
    fun requestGameLibraryScan(gen: Int) {
        Thread({
            val json = GameLibraryScanner.scanRomUris(activity.applicationContext)
            activity.runOnUiThread {
                if (!activity.isDestroyed) {
                    activity.dispatchGameLibraryPaths(gen, json)
                }
            }
        }, "GameLibraryScan").start()
    }

    @JavascriptInterface
    fun queryRomMetadata(path: String): String {
        return NativeLibrary.queryRomMetadata(path)
    }
}
