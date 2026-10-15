// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

// HitzMR viewing modes, switched in the headset with the same gestures as Strikers MR:
//   double-tap left stick  = MR <-> VR (passthrough on/off)
//   double-tap right stick = Jumbotron (big flat screen) <-> 3D (stereo game world)
// Plus the weather overlay: rain or snow falling all around the player in every mode.

#pragma once

#include <array>

#include <openxr/openxr.h>

#include "Common/CommonTypes.h"
#include "Common/VR/OpenXRInputState.h"

namespace HitzModes
{
// StreetMR r35: the public GitHub build (app id io.github.lancesweatpants.street, Gradle
// -PstreetPublic=true) is Jumbotron only - no Tabletop, no dev snapshots. Lance's own build keeps
// everything. Told apart at run time by the app's process name, so both share one native build.
bool StreetPublic();

// Called by the OpenXR pacing thread after every input sync.
void OnControllers(const std::array<Common::VR::OpenXRControllerState, 2>& controllers);

// Sets the view directly (dev commands): view = HitzWorld::View (0 Jumbotron, 1 Tabletop,
// 2 Rinkside), mr = passthrough. -1 leaves that part unchanged.
void SetView(int view, int mr);

// Next Jumbotron screen size (the single right-stick tap; also the dev "screensize" command).
void CycleScreenSize();

// StreetMR VR 360: Lance's MR/VR choice (the left-stick double tap): true = VR ("Stadium"). In VR
// the court you're on wraps around you (OpenXRManager's cube layer); the Passthrough setting then
// only stays on for Tabletop over a loaded court picture (its EFB alpha lets the picture through).
bool StadiumWanted();
// How many times HitzModes changed the Passthrough setting (each one recreates the EFB and
// reloads the shaders); in HitzDev's perf line.
u32 PassthroughSettingChanges();

enum class Weather : int
{
  Off = 0,
  Rain = 1,
  Snow = 2,
};
Weather GetWeather();
void SetWeather(Weather weather);

// The graphics backend registers a renderer for the weather layer. The pacing thread calls it
// once per display frame; it returns a layer to composite on top, or nullptr for none.
using OverlayLayerProvider = const XrCompositionLayerBaseHeader* (*)(XrTime display_time);
void SetOverlayLayerProvider(OverlayLayerProvider provider);
const XrCompositionLayerBaseHeader* GetOverlayLayer(XrTime display_time);
}  // namespace HitzModes
