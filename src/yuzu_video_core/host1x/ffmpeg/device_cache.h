// SPDX-FileCopyrightText: 2026 nxemu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace FFmpeg
{
// Native device creation precedes guest execution; decoder contexts remain per stream.
void WarmHardwareDecoderDevices();
void ReleaseHardwareDecoderDevices();
} // namespace FFmpeg
