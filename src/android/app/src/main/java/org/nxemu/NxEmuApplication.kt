package org.nxemu

import android.app.Application
import org.nxemu.utils.BundledMods
import org.nxemu.utils.GpuDriverHelper
import org.nxemu.utils.Translation

class NxEmuApplication : Application() {
    companion object {
        lateinit var appContext: Application
            private set
    }

    override fun onCreate() {
        super.onCreate()
        appContext = this

        BundledMods.install(this, AndroidDevice.PACKAGE_DIRECTORY)
        NativeLibrary.appInit(
            AndroidDevice.PACKAGE_DIRECTORY,
            AndroidDevice.nativeModuleLibsDirectory(this),
        )
        Translation.start(assets)
        GpuDriverHelper.initializeDriverParameters()
        NativeLibrary.restorePersistedGameDirectoryAccess()
        org.nxemu.utils.InputHandler.updateControllerData()
    }
}
