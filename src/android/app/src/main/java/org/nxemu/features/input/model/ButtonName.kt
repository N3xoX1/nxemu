// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input.model

enum class ButtonName(val int: Int) {
    Invalid(1),
    Engine(2),
    Value(3);

    companion object {
        fun from(int: Int): ButtonName = entries.firstOrNull { it.int == int } ?: Invalid
    }
}
