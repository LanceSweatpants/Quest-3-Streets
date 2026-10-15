// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

// HitzMR weather overlay for the Vulkan OpenXR backend: rain streaks or snowflakes simulated
// around the player's head in the room's reference space and drawn into their own OpenXR
// projection layer (premultiplied alpha), composited on top of the game in every mode.

#pragma once

#ifdef ENABLE_VR

namespace Vulkan::HitzWeather
{
// Registers the overlay provider with HitzModes. Rendering resources are created lazily on the
// OpenXR pacing thread the first time weather is switched on.
void Initialize();

// Destroys the swapchain and Vulkan objects. Call after the pacing thread has stopped and
// before the XrSession is destroyed.
void Shutdown();
}  // namespace Vulkan::HitzWeather

#endif  // ENABLE_VR
