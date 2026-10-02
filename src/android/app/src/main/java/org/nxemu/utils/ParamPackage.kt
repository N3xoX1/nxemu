// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.utils

class ParamPackage(serialized: String = "") {
    private val keyValueSeparator = ":"
    private val paramSeparator = ","

    private val escapeCharacter = "$"
    private val keyValueSeparatorEscape = "$0"
    private val paramSeparatorEscape = "$1"
    private val escapeCharacterEscape = "$2"

    private val emptyPlaceholder = "[empty]"

    val data = mutableMapOf<String, String>()

    init {
        if (serialized.isNotEmpty() && serialized != emptyPlaceholder) {
            val pairs = serialized.split(paramSeparator)
            for (pair in pairs) {
                val keyValue = pair.split(keyValueSeparator).toMutableList()
                if (keyValue.size != 2) {
                    continue
                }
                keyValue.forEachIndexed { i: Int, _: String ->
                    keyValue[i] = keyValue[i].replace(keyValueSeparatorEscape, keyValueSeparator)
                    keyValue[i] = keyValue[i].replace(paramSeparatorEscape, paramSeparator)
                    keyValue[i] = keyValue[i].replace(escapeCharacterEscape, escapeCharacter)
                }
                set(keyValue[0], keyValue[1])
            }
        }
    }

    constructor(params: List<Pair<String, String>>) : this() {
        params.forEach {
            data[it.first] = it.second
        }
    }

    fun serialize(): String {
        if (data.isEmpty()) {
            return emptyPlaceholder
        }

        val result = StringBuilder()
        data.forEach {
            val keyValue = mutableListOf(it.key, it.value)
            keyValue.forEachIndexed { i, _ ->
                keyValue[i] = keyValue[i].replace(escapeCharacter, escapeCharacterEscape)
                keyValue[i] = keyValue[i].replace(paramSeparator, paramSeparatorEscape)
                keyValue[i] = keyValue[i].replace(keyValueSeparator, keyValueSeparatorEscape)
            }
            result.append("${keyValue[0]}$keyValueSeparator${keyValue[1]}$paramSeparator")
        }
        return result.removeSuffix(paramSeparator).toString()
    }

    fun get(key: String, defaultValue: String): String =
        if (has(key)) {
            data[key]!!
        } else {
            defaultValue
        }

    fun get(key: String, defaultValue: Int): Int =
        if (has(key)) {
            try {
                data[key]!!.toInt()
            } catch (_: NumberFormatException) {
                defaultValue
            }
        } else {
            defaultValue
        }

    fun get(key: String, defaultValue: Boolean): Boolean =
        if (has(key)) {
            when (data[key]) {
                "1" -> true
                "0" -> false
                else -> defaultValue
            }
        } else {
            defaultValue
        }

    fun get(key: String, defaultValue: Float): Float =
        if (has(key)) {
            try {
                data[key]!!.toFloat()
            } catch (_: NumberFormatException) {
                defaultValue
            }
        } else {
            defaultValue
        }

    fun set(key: String, value: String) {
        data[key] = value
    }

    fun set(key: String, value: Int) {
        data[key] = value.toString()
    }

    fun set(key: String, value: Boolean) {
        data[key] = if (value) "1" else "0"
    }

    fun set(key: String, value: Float) {
        data[key] = value.toString()
    }

    fun has(key: String): Boolean = data.containsKey(key)

    fun clear() = data.clear()
}
