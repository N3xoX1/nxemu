// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "nxemu-module-spec/video.h"

#include <memory>
#include "yuzu_video_core/renderer_opengl/present/window_adapt_pass.h"

namespace OpenGL {

std::unique_ptr<WindowAdaptPass> MakeNearestNeighbor(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeArea(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeBilinear(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeCatmullRom(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeCas(const Device& device, ScalingFilter filter);
std::unique_ptr<WindowAdaptPass> MakeFsrEasu(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeFsrRcas(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeBicubic(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeGaussian(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeScaleForce(const Device& device);
std::unique_ptr<WindowAdaptPass> MakeScaleForceFast(const Device& device);

} // namespace OpenGL
