package org.nxemu.utils

import android.content.Context
import java.io.IOException
import org.nxemu.NxEmuApplication

object AndroidDevice {
    val PACKAGE_DIRECTORY: String
        get() {
            val filesDir = NxEmuApplication.appContext.filesDir
            return try {
                filesDir.canonicalPath
            } catch (_: IOException) {
                filesDir.absolutePath
            }
        }

    fun nativeModuleLibsDirectory(context: Context): String {
        return context.applicationInfo.nativeLibraryDir
    }
}
