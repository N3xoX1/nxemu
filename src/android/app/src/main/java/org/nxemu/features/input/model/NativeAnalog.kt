// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input.model

enum class NativeAnalog(val int: Int) {
    LStick(0),
    RStick(1);

    companion object {
        fun from(int: Int): NativeAnalog = entries.firstOrNull { it.int == int } ?: LStick
    }
}
