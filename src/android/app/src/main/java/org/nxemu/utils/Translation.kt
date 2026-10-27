package org.nxemu.utils

import android.content.res.AssetManager
import android.os.LocaleList

object Translation {
    init {
        System.loadLibrary("nxemu-android")
    }

    // Must run after the settings are loaded.
    fun start(assets: AssetManager) {
        val locales = LocaleList.getDefault()
        start(assets, (0 until locales.size()).joinToString(",") { "${locales[it].language}_${locales[it].country}" })
    }

    fun tr(text: String, vararg values: String): String = translate(text, values)

    external fun trCount(one: String, other: String, count: Long): String

    private external fun start(assets: AssetManager, deviceLocales: String)
    private external fun translate(text: String, values: Array<out String>): String
}
