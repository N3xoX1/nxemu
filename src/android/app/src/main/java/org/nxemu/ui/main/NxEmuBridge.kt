package org.nxemu.ui.main

import android.webkit.JavascriptInterface
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary
import org.nxemu.utils.ThemeHelper

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
    fun getSettingInt(setting: String): Int {
        return NativeLibrary.getSettingInt(setting)
    }

    @JavascriptInterface
    fun setSettingInt(setting: String, value: Int) {
        NativeLibrary.setSettingInt(setting, value)
    }

    @JavascriptInterface
    fun isDarkTheme(): Boolean {
        return ThemeHelper.isDark(activity)
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

    @JavascriptInterface
    fun launchGame(path: String) {
        activity.runOnUiThread { activity.launchGame(path) }
    }
}
