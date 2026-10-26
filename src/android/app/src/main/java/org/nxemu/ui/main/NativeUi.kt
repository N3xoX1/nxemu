package org.nxemu.ui.main

import android.content.res.AssetManager
import android.view.Surface

object NativeUi {
    const val TOUCH_DOWN = 0
    const val TOUCH_MOVE = 1
    const val TOUCH_UP = 2
    const val TOUCH_CANCEL = 3

    const val MODE_LIBRARY = 0
    const val MODE_SETTINGS = 1
    const val MODE_HUB = 2

    init {
        System.loadLibrary("nxemu-android")
    }

    external fun start(assets: AssetManager)
    external fun create(host: UiHost, mode: Int): Long
    external fun destroy(screen: Long)

    external fun setSurface(screen: Long, surface: Surface?, width: Int, height: Int)
    external fun setMetrics(screen: Long, density: Float, left: Int, top: Int, right: Int, bottom: Int)
    external fun touch(screen: Long, action: Int, x: Float, y: Float)
    external fun event(screen: Long, name: String, payload: String)
}
