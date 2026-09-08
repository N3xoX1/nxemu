package org.nxemu

import android.content.Context
import java.io.File

object AndroidDevice {
    val PACKAGE_DIRECTORY: String = NxEmuApplication.appContext.getExternalFilesDir(null)!!.absolutePath

    fun nativeModuleLibsDirectory(context: Context): String {
        val fromInfo = context.applicationInfo.nativeLibraryDir
        if (!fromInfo.isNullOrEmpty()) {
            return fromInfo
        }
        val parent = context.filesDir.parentFile?.absolutePath ?: return ""
        val legacyLib = File(parent, "lib")
        return if (legacyLib.isDirectory) legacyLib.absolutePath else File(parent, "lib").absolutePath
    }
}