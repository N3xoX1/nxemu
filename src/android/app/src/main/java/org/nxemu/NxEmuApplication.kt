package org.nxemu

import android.app.Application
import org.nxemu.utils.AndroidDevice
import org.nxemu.utils.GpuDriverHelper

class NxEmuApplication : Application() {
    companion object {
        lateinit var appContext: Application
            private set
    }

    override fun onCreate() {
        super.onCreate()
        appContext = this

        NativeLibrary.appInit(
            AndroidDevice.PACKAGE_DIRECTORY,
            AndroidDevice.nativeModuleLibsDirectory(this),
        )
        GpuDriverHelper.initializeDriverParameters()
        NativeLibrary.restorePersistedGameDirectoryAccess()
    }
}
