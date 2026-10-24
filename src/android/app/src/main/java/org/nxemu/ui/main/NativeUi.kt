package org.nxemu.ui.main

import android.content.res.AssetManager
import android.view.Surface

object NativeUi {
    init {
        System.loadLibrary("nxemu-android")
    }

    external fun start(assets: AssetManager)
    external fun create(host: UiHost, settings: Boolean): Long
    external fun destroy(screen: Long)

    external fun event(screen: Long, name: String, payload: String)
}
