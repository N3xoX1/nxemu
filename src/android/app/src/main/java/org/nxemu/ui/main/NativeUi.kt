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

    external fun setSurface(screen: Long, surface: Surface?, width: Int, height: Int)
    external fun setMetrics(screen: Long, density: Float, left: Int, top: Int, right: Int, bottom: Int)
    external fun event(screen: Long, name: String, payload: String)
}
