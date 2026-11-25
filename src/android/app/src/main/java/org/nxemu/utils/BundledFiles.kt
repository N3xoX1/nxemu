package org.nxemu.utils

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import java.io.File

// The mods and per-game setting defaults that ship with NxEmu are in the APK's assets; the native code
// reads them from the app's storage, so they are copied there when the app is installed or updated.
object BundledFiles {
    private const val TAG = "NxEmu"
    private const val MODS_ASSETS_DIR = "mods"
    private const val MODS_TARGET_DIR = "bundled_mods"
    private const val GAME_DEFAULTS_FILE = "game_defaults.json"
    private const val STAMP_FILE = ".version"

    fun install(context: Context, appDirectory: String) {
        val mods = File(appDirectory, MODS_TARGET_DIR)
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        val stamp = "${info.longVersionCode} ${info.lastUpdateTime}"
        val stampFile = File(mods, STAMP_FILE)
        if (stampFile.isFile && stampFile.readText() == stamp) {
            return
        }
        val defaultsCopied = attempt("Copying the game defaults") {
            copy(context.assets, GAME_DEFAULTS_FILE, File(appDirectory, GAME_DEFAULTS_FILE))
        }
        val modsCopied = attempt("Copying the bundled mods") {
            mods.deleteRecursively()
            copy(context.assets, MODS_ASSETS_DIR, mods)
            mods.mkdirs()
        }
        if (defaultsCopied && modsCopied) {
            attempt("Writing the bundled files version") { stampFile.writeText(stamp) }
        }
    }

    private fun attempt(action: String, block: () -> Unit): Boolean =
        try {
            block()
            true
        } catch (e: Exception) {
            Log.e(TAG, "$action failed", e)
            false
        }

    private fun copy(assets: AssetManager, path: String, target: File) {
        val children = assets.list(path) ?: return
        if (children.isEmpty()) {
            target.parentFile?.mkdirs()
            assets.open(path).use { input -> target.outputStream().use { input.copyTo(it) } }
            return
        }
        target.mkdirs()
        for (child in children) {
            copy(assets, "$path/$child", File(target, child))
        }
    }
}
