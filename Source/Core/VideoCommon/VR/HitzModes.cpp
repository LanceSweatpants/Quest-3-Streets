// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/HitzModes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VR/HitzWorld.h"

namespace HitzModes
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto DOUBLE_TAP_WINDOW = std::chrono::milliseconds(400);

// A tap = press and release within TAP_MAX; a longer press is a hold (holding the left stick in
// grabs the table, since the grips are game buttons now - Lance, r3).
constexpr auto TAP_MAX = std::chrono::milliseconds(350);

struct TapState
{
  bool was_down = false;
  Clock::time_point down_at{};
  Clock::time_point last_tap{};
};
std::array<TapState, 2> s_taps;  // left stick, right stick

// True on the release of the second quick tap of a double tap.
bool DoubleTapped(TapState& tap, bool down)
{
  const auto now = Clock::now();
  const bool rising = down && !tap.was_down;
  const bool falling = !down && tap.was_down;
  tap.was_down = down;
  if (rising)
    tap.down_at = now;
  if (!falling || now - tap.down_at > TAP_MAX)
    return false;

  if (tap.last_tap != Clock::time_point{} && now - tap.last_tap < DOUBLE_TAP_WINDOW)
  {
    tap.last_tap = {};
    return true;
  }
  tap.last_tap = now;
  return false;
}

// True once a tap has stood alone for the double-tap window (no second tap followed). Call every
// frame after DoubleTapped on the same TapState.
bool SingleTapped(TapState& tap)
{
  if (tap.last_tap == Clock::time_point{} || Clock::now() - tap.last_tap < DOUBLE_TAP_WINDOW)
    return false;
  tap.last_tap = {};
  return true;
}

// Jumbotron screen heights (metres), cycled with a single right-stick tap; the distance grows with
// the size so it always fills about the same view.
constexpr std::array<float, 3> SCREEN_SIZES = {2.0f, 3.0f, 4.5f};
int s_screen_size = 0;

// StreetMR VR 360: MR (0) or VR (1), Lance's choice.
// r30 (Lance: "try starting the game in jumbotron VR mode as opposed to the MR mode"): every
// launch starts in VR (the big screen inside the court); a left-stick double tap goes to MR. VR
// held 60 fps / 100% speed through his whole r28 session while MR fell to 73-88% at match start.
// The Passthrough setting stays on either way (WantPassthroughSetting), so no reload.
std::atomic<int> s_stadium{1};
std::atomic<bool> s_passthrough_job_pending{false};

bool Stadium()
{
  return s_stadium.load(std::memory_order_acquire) == 1;
}

// The Passthrough setting (EFB coverage alpha + the camera feed in MR) for the MR/VR choice: on
// in MR; in VR on for Tabletop whenever a backdrop cube can show (the table's alpha lets the
// court's calm sky through - no court drawn around you, Lance r23). Only without cube layers, or
// while capturing a court's pictures, does Tabletop VR draw the court life-size around you. The
// flat Jumbotron's VR backdrop is a layer of its own.
// StreetMR r26: with a backdrop the setting simply stays on in every view, MR or VR. Lance's r25
// log: the Jumbotron's MR -> VR switch turned it off, which recreated the EFB and reloaded the
// shaders (19 fps for 5 s). The flat Jumbotron doesn't use the coverage alpha, and the camera feed
// follows the MR/VR choice on its own (OpenXRManager::EndFrameDetached), so on is right in both.
bool WantPassthroughSetting(HitzWorld::View view)
{
  if (!Stadium())
    return true;
  if (HitzWorld::Vr360Capture())
    return false;
  return HitzWorld::BackdropSupported() || view == HitzWorld::View::Jumbotron;
}

void ShowMode()
{
  const bool mr = !Stadium();
  const HitzWorld::View view = HitzWorld::GetView();
  // "Living Room" = in your room (MR); "Stadium" = VR (Jumbotron: the court you're on all around
  // you; Tabletop: the court's calm sky).
  // StreetMR r18: the flat Jumbotron shows the room behind it too (OpenGL ES).
  const char* suffix = mr ? " - Living Room" : " - Stadium";
  const std::string name = std::string(HitzWorld::ViewName(view)) + suffix;
  OSD::AddMessage(name, OSD::Duration::NORMAL);
  NOTICE_LOG_FMT(VIDEO, "HitzModes: {}", name);
}

// Keeps the Passthrough setting in line with the MR/VR choice when what it should be changes (cube
// layers becoming available, a capture starting or ending). Edge-triggered: it only acts when its
// own wish changes and never fights another writer - every change recreates the EFB and reloads
// the shaders, a stutter (Baseball's review of r23). Each change is logged and counted.
std::atomic<int> s_last_wanted{-1};
std::atomic<u32> s_passthrough_changes{0};

void SetPassthroughSetting(bool on, const char* why)
{
  s_last_wanted.store(on ? 1 : 0, std::memory_order_release);
  if (on == Config::Get(Config::GFX_VR_PASSTHROUGH))
    return;
  s_passthrough_changes.fetch_add(1, std::memory_order_relaxed);
  NOTICE_LOG_FMT(VIDEO, "HitzModes: Passthrough setting {} ({})", on ? "on" : "off", why);
  Config::SetCurrent(Config::GFX_VR_PASSTHROUGH, on);
}

void SyncPassthroughSetting()
{
  if (s_passthrough_job_pending.load(std::memory_order_acquire))
    return;
  const int want = WantPassthroughSetting(HitzWorld::GetView()) ? 1 : 0;
  if (want == s_last_wanted.load(std::memory_order_acquire))
    return;
  s_passthrough_job_pending.store(true, std::memory_order_release);
  Core::QueueHostJob([](Core::System&) {
    SetPassthroughSetting(WantPassthroughSetting(HitzWorld::GetView()), "sync");
    s_passthrough_job_pending.store(false, std::memory_order_release);
  });
}

// Applies a view. Everything runs in stereo (StreetMR launches in VR): Jumbotron draws the whole
// game onto the fork's world-fixed virtual screen (VertexManagerBase forces the Screen route), so
// passthrough works around it - the flat OpenXR quad crashed with passthrough in Hitz r6.
// Tabletop/Rinkside use the world lock (HitzWorld); their HUD stays head-locked (no virtual
// screen). Config goes through the host thread like a settings-menu change.
void ApplyView(HitzWorld::View view, int mr)
{
  HitzWorld::SetView(view);
  if (mr >= 0)
  {
    Stadium();  // read the launch setting first if this is the first change
    s_stadium.store(mr != 0 ? 0 : 1, std::memory_order_release);
  }
  Core::QueueHostJob([view](Core::System&) {
    // r4: Jumbotron is the flat OpenXR quad again (sharp, and stable). The stereo virtual-screen
    // Jumbotron (r2/r3) scrambled the game's own EFB-copy effects at match load-in ("short
    // circuit", Lance r3) and looked blurry in the eye buffers.
    Config::SetCurrent(Config::GFX_VR_FLAT_SCREEN, view == HitzWorld::View::Jumbotron);
    Config::SetCurrent(Config::GFX_VR_VIRTUAL_SCREEN, false);
    // StreetMR r27 (Lance: "get it clearer"): the table renders at a higher internal resolution
    // ("tune tableir", default 3; the Jumbotron keeps GFX.ini's 2). Measured on Uptown in Tabletop
    // MR: IR 3 = 60 fps at GPU 63% busy (IR 2: 52%; IR 3 + MSAA 2: 88%, too close for recording).
    const int table_ir = HitzWorld::TableInternalResolution();
    const int ir = view == HitzWorld::View::Tabletop && table_ir > 0 ?
                       table_ir :
                       Config::GetBase(Config::GFX_EFB_SCALE);
    if (Config::Get(Config::GFX_EFB_SCALE) != ir)
      Config::SetCurrent(Config::GFX_EFB_SCALE, ir);
    SetPassthroughSetting(WantPassthroughSetting(view), "view");
    ShowMode();
  });
}
}  // namespace

bool StreetPublic()
{
  static const bool is_public = [] {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string name;
    std::getline(cmdline, name, '\0');
    const bool result = name.rfind("io.github.lancesweatpants.street", 0) == 0;
    NOTICE_LOG_FMT(VIDEO, "HitzModes: {} ({})", result ? "public build" : "developer build", name);
    return result;
  }();
  return is_public;
}

void OnControllers(const std::array<Common::VR::OpenXRControllerState, 2>& controllers)
{
  const bool toggle_mr = DoubleTapped(s_taps[0], controllers[0].thumbstick_button);
  const bool toggle_view = DoubleTapped(s_taps[1], controllers[1].thumbstick_button);

  // StreetMR r33: both sticks held in for a second = a snapshot of this moment for Claude (a
  // census of every draw + a screenshot). Once per hold.
  if (!StreetPublic())
  {
    static Clock::time_point both_since{};
    static bool taken = false;
    if (controllers[0].thumbstick_button && controllers[1].thumbstick_button)
    {
      if (both_since == Clock::time_point{})
        both_since = Clock::now();
      if (!taken && Clock::now() - both_since > std::chrono::seconds(1))
      {
        taken = true;
        HitzWorld::RequestSnapshot();
        OSD::AddMessage("Snapshot saved", OSD::Duration::SHORT);
      }
    }
    else
    {
      both_since = {};
      taken = false;
    }
  }

  // Single taps (like Strikers): left stick = bring the table back in front of you (Tabletop),
  // right stick = next screen size (Jumbotron).
  if (SingleTapped(s_taps[0]))
  {
    if (HitzWorld::GetView() == HitzWorld::View::Tabletop)
    {
      HitzWorld::BringTableHere();
      OSD::AddMessage("Table: here", OSD::Duration::SHORT);
    }
    else if (HitzWorld::GetView() == HitzWorld::View::Jumbotron)
    {
      HitzWorld::BringScreenHere();  // StreetMR r27, as in Charged
      OSD::AddMessage("Screen: here", OSD::Duration::SHORT);
    }
  }
  if (SingleTapped(s_taps[1]) && HitzWorld::GetView() == HitzWorld::View::Jumbotron)
    CycleScreenSize();

  // Tabletop (Lance's rule for every MR project): a grip that closes on the table holds it - one
  // hand moves it, two hands turn and resize it. Anywhere else the grips are game buttons.
  HitzWorld::OnControllers(controllers);

  if (!toggle_mr && !toggle_view)
  {
    SyncPassthroughSetting();
    return;
  }

  // Right stick: Jumbotron <-> Tabletop (no Courtside: Lance, 2026-10-10). Left stick: MR <-> VR.
  // The public build (r35, Lance: "JUST ship the Jumbotron version tonight, with the tabletop
  // coming soon") has no Tabletop: the right-stick double tap does nothing there.
  HitzWorld::View view = HitzWorld::GetView();
  if (toggle_view && !StreetPublic())
  {
    view = view == HitzWorld::View::Jumbotron ? HitzWorld::View::Tabletop :
                                                HitzWorld::View::Jumbotron;
  }
  // StreetMR r18: the left-stick double tap also switches the flat Jumbotron between the room
  // behind the screen (MR) and VR (OpenGL ES: no device loss with passthrough behind the quad).
  const int mr = toggle_mr ? (Stadium() ? 1 : 0) : -1;
  ApplyView(view, mr);
}

bool StadiumWanted()
{
  return Stadium();
}

u32 PassthroughSettingChanges()
{
  return s_passthrough_changes.load(std::memory_order_relaxed);
}

void CycleScreenSize()
{
  s_screen_size = (s_screen_size + 1) % static_cast<int>(SCREEN_SIZES.size());
  const float size = SCREEN_SIZES[s_screen_size];
  NOTICE_LOG_FMT(VIDEO, "HitzModes: screen size {:.1f} m", size);
  // StreetMR r27: the hand-placed screen takes the size, re-placed in front of you.
  HitzWorld::SetScreenPreset(size, size * 1.5f);
  OSD::AddMessage(fmt::format("Screen: {:.1f} m", size), OSD::Duration::SHORT);
}

void SetView(int view, int mr)
{
  HitzWorld::View v = view >= 0 ? static_cast<HitzWorld::View>(view) : HitzWorld::GetView();
  if (StreetPublic() && v == HitzWorld::View::Tabletop)
    v = HitzWorld::View::Jumbotron;
  ApplyView(v, mr);
}

namespace
{
std::atomic<int> s_weather{static_cast<int>(Weather::Off)};
std::atomic<OverlayLayerProvider> s_overlay_provider{nullptr};
}  // namespace

Weather GetWeather()
{
  return static_cast<Weather>(s_weather.load(std::memory_order_relaxed));
}

void SetWeather(Weather weather)
{
  if (s_weather.exchange(static_cast<int>(weather)) != static_cast<int>(weather))
    NOTICE_LOG_FMT(VIDEO, "HitzModes: weather {}", static_cast<int>(weather));
}

void SetOverlayLayerProvider(OverlayLayerProvider provider)
{
  s_overlay_provider.store(provider);
}

const XrCompositionLayerBaseHeader* GetOverlayLayer(XrTime display_time)
{
  const OverlayLayerProvider provider = s_overlay_provider.load();
  return provider ? provider(display_time) : nullptr;
}
}  // namespace HitzModes
