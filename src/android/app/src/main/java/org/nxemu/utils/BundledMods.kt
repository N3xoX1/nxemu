package org.nxemu.utils

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import java.io.File

// The mods that ship with NxEmu are in the APK's assets (mods/); the loader reads them from
// <app storage>/bundled_mods, so they are copied there when the app is installed or updated.
object BundledMods {
    private const val TAG = "NxEmu"
    private const val ASSETS_DIR = "mods"
    private const val TARGET_DIR = "bundled_mods"
    private const val STAMP_FILE = ".version"

    fun install(context: Context, appDirectory: String) {
        val target = File(appDirectory, TARGET_DIR)
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        val stamp = "${info.longVersionCode} ${info.lastUpdateTime}"
        val stampFile = File(target, STAMP_FILE)
        if (stampFile.isFile && stampFile.readText() == stamp) {
            return
        }
        try {
            target.deleteRecursively()
            copy(context.assets, ASSETS_DIR, target)
            target.mkdirs()
            stampFile.writeText(stamp)
        } catch (e: Exception) {
            Log.e(TAG, "Copying the bundled mods failed", e)
        }
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
