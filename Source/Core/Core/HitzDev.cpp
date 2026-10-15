// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HitzDev.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef ANDROID
#include <android/log.h>
#endif

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Common/GekkoDisassembler.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Thread.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/Memmap.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/XFMemory.h"
#ifdef ENABLE_VR
#include "VideoCommon/VR/HitzModes.h"
#include "VideoCommon/VR/HitzWorld.h"
#endif

namespace HitzDev
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Injection
{
  u16 buttons = 0;
  bool stick = false;
  float stick_x = 0, stick_y = 0;
  bool cstick = false;
  float cstick_x = 0, cstick_y = 0;
};

std::mutex s_mutex;
Injection s_injection;
std::once_flag s_started;

// "hold <addr> <hex>": bytes rewritten every frame (from the pad poll on the CPU thread) until
// "hold off" - for trying RAM values the game itself rewrites each frame (camera FOV etc.).
struct HeldBytes
{
  u32 addr;
  std::vector<u8> bytes;
};
std::vector<HeldBytes> s_holds;  // guarded by s_mutex

// "code <addr> <hex>": original instructions, for "code restore".
std::map<u32, u32> s_code_patches;

void Out(const std::string& line)
{
#ifdef ANDROID
  __android_log_print(ANDROID_LOG_INFO, "HitzDev", "%s", line.c_str());
#endif
  std::ofstream out(File::GetUserPath(D_USER_IDX) + "hitz_out.txt", std::ios::app);
  out << line << '\n';
}

u16 ButtonBit(const std::string& name)
{
  static const std::pair<const char*, u16> names[] = {
      {"A", PAD_BUTTON_A},       {"B", PAD_BUTTON_B},         {"X", PAD_BUTTON_X},
      {"Y", PAD_BUTTON_Y},       {"Z", PAD_TRIGGER_Z},        {"L", PAD_TRIGGER_L},
      {"R", PAD_TRIGGER_R},      {"START", PAD_BUTTON_START}, {"UP", PAD_BUTTON_UP},
      {"DOWN", PAD_BUTTON_DOWN}, {"LEFT", PAD_BUTTON_LEFT},   {"RIGHT", PAD_BUTTON_RIGHT},
  };
  for (const auto& [n, bit] : names)
  {
    if (name == n)
      return bit;
  }
  return 0;
}

// Dolphin builds without exceptions, so parse with strto* instead of std::sto*.
bool ParseInt(const std::string& s, int* out)
{
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0')
    return false;
  *out = static_cast<int>(v);
  return true;
}

bool ParseFloat(const std::string& s, float* out)
{
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0')
    return false;
  *out = v;
  return true;
}

bool ParseHex(const std::string& s, u32* out)
{
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (end == s.c_str() || *end != '\0')
    return false;
  *out = static_cast<u32>(v);
  return true;
}

void Sleep(int ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(std::max(ms, 0)));
}

void Hold(const Injection& injection, int ms)
{
  {
    std::lock_guard lk(s_mutex);
    s_injection = injection;
  }
  Sleep(ms);
  {
    std::lock_guard lk(s_mutex);
    s_injection = {};
  }
  Sleep(50);  // a released frame between presses
}

void RunCommand(const std::string& line)
{
  std::istringstream in(line);
  std::vector<std::string> words;
  for (std::string w; in >> w;)
    words.push_back(w);
  if (words.empty() || words[0][0] == '#')
    return;

  const std::string& cmd = words[0];
  auto& system = Core::System::GetInstance();
  int ms = 0, slot = 0, count_arg = 0;
  float x = 0, y = 0;
  u32 addr = 0;

  if (cmd == "press" && words.size() >= 3 && ParseInt(words.back(), &ms))
  {
    Injection injection;
    for (size_t i = 1; i + 1 < words.size(); ++i)
      injection.buttons |= ButtonBit(words[i]);
    Hold(injection, ms);
  }
  else if ((cmd == "stick" || cmd == "cstick") && words.size() == 4 &&
           ParseFloat(words[1], &x) && ParseFloat(words[2], &y) && ParseInt(words[3], &ms))
  {
    Injection injection;
    if (cmd == "stick")
      injection.stick = true, injection.stick_x = x, injection.stick_y = y;
    else
      injection.cstick = true, injection.cstick_x = x, injection.cstick_y = y;
    Hold(injection, ms);
  }
  else if (cmd == "wait" && words.size() == 2 && ParseInt(words[1], &ms))
  {
    Sleep(ms);
  }
  else if (cmd == "shot" && words.size() == 2)
  {
    const std::string name = words[1];
    Core::QueueHostJob([name](Core::System&) { Core::SaveScreenShot(name); });
  }
  else if ((cmd == "save" || cmd == "load") && words.size() == 2 && ParseInt(words[1], &slot))
  {
    const bool save = cmd == "save";
    Core::QueueHostJob([slot, save](Core::System& s) {
      if (save)
        State::Save(s, slot);
      else
        State::Load(s, slot);
    });
  }
  else if (cmd == "dump" && words.size() == 2)
  {
    auto& memory = system.GetMemory();
    const std::string path = File::GetUserPath(D_DUMP_IDX) + words[1] + ".bin";
    File::CreateFullPath(path);
    File::IOFile f(path, "wb");
    f.WriteBytes(memory.GetRAM(), memory.GetRamSizeReal());
    Out(fmt::format("dump {} {} bytes", path, memory.GetRamSizeReal()));
  }
  else if (cmd == "peek" && words.size() == 3 && ParseHex(words[1], &addr) &&
           ParseInt(words[2], &count_arg))
  {
    auto& memory = system.GetMemory();
    addr &= 0x01FFFFFF;
    if (addr >= memory.GetRamSizeReal())
      return Out("bad address: " + line);
    const u32 count = std::min<u32>(std::max(count_arg, 0), memory.GetRamSizeReal() - addr);
    std::string hex;
    for (u32 i = 0; i < count; ++i)
      hex += fmt::format("{:02x}", memory.GetRAM()[addr + i]);
    Out(fmt::format("peek {:08x} {}", 0x80000000 | addr, hex));
  }
  else if (cmd == "poke" && words.size() == 3 && ParseHex(words[1], &addr))
  {
    auto& memory = system.GetMemory();
    addr &= 0x01FFFFFF;
    const std::string& hex = words[2];
    for (size_t i = 0; i + 1 < hex.size() && addr + i / 2 < memory.GetRamSizeReal(); i += 2)
    {
      u32 byte = 0;
      if (!ParseHex(hex.substr(i, 2), &byte))
        return Out("bad bytes: " + line);
      memory.GetRAM()[addr + i / 2] = static_cast<u8>(byte);
    }
    Out(fmt::format("poke {:08x} {}", 0x80000000 | addr, hex));
  }
#ifdef ENABLE_VR
  else if (cmd == "weather" && words.size() == 2)
  {
    const std::string& kind = words[1];
    HitzModes::SetWeather(kind == "rain" ? HitzModes::Weather::Rain :
                          kind == "snow" ? HitzModes::Weather::Snow :
                                           HitzModes::Weather::Off);
  }
  else if (cmd == "screensize" && words.size() == 1)
  {
    // Dev: the Jumbotron's single right-stick tap (next screen size), for unworn repros.
    HitzModes::CycleScreenSize();
  }
  else if (cmd == "mode" && words.size() == 2)
  {
    const std::string& m = words[1];
    const int view = (m == "jumbotron" || m == "flat")     ? 0 :
                     (m == "tabletop" || m == "3d")        ? 1 :
                     (m == "courtside" || m == "rinkside") ? 2 :
                                                             -1;
    HitzModes::SetView(view, m == "mr" ? 1 : m == "vr" ? 0 : -1);
  }
  else if (cmd == "tune" && words.size() == 3 && ParseFloat(words[2], &x))
  {
    HitzWorld::SetTuning(words[1], x);
  }
  else if (cmd == "census" && words.size() == 1)
  {
    HitzWorld::RequestCensus();
  }
  else if (cmd == "ir" && words.size() == 2 && ParseInt(words[1], &count_arg))
  {
    // Dev (StreetMR VR 360 captures): internal resolution, live (sharper court pictures).
    const int scale = std::clamp(count_arg, 1, 6);
    Core::QueueHostJob([scale](Core::System&) { Config::SetCurrent(Config::GFX_EFB_SCALE, scale); });
  }
  else if (cmd == "msaa" && words.size() == 2 && ParseInt(words[1], &count_arg))
  {
    // Dev: anti-aliasing samples (1, 2 or 4), to compare looks and speed live.
    const u32 samples = static_cast<u32>(std::clamp(count_arg, 1, 4));
    Core::QueueHostJob([samples](Core::System&) { Config::SetCurrent(Config::GFX_MSAA, samples); });
  }
  else if (cmd == "cfg" && words.size() == 3 && ParseInt(words[2], &count_arg))
  {
    // Dev: flip a graphics setting live to find what breaks a texture.
    static const std::pair<const char*, const Config::Info<bool>*> settings[] = {
        {"efbtoram_skip", &Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM},
        {"defer_efb", &Config::GFX_HACK_DEFER_EFB_COPIES},
        {"fastsampling", &Config::GFX_HACK_FAST_TEXTURE_SAMPLING},
        {"gputex", &Config::GFX_ENABLE_GPU_TEXTURE_DECODING},
        {"efbscaled", &Config::GFX_HACK_COPY_EFB_SCALED},
        {"panes", &Config::GFX_VR_PANES_ON_SCREEN},
        {"detectrt", &Config::GFX_VR_DETECT_RENDER_TARGETS},
        {"autonative", &Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS},
        {"removebars", &Config::GFX_VR_REMOVE_BARS},
        {"rmblackclears", &Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS},
        {"rmblackbg", &Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG},
        {"dumptex", &Config::GFX_DUMP_TEXTURES},
        {"dumpefb", &Config::GFX_DUMP_EFB_TARGET},
    };
    const Config::Info<bool>* info = nullptr;
    for (const auto& [name, setting] : settings)
    {
      if (words[1] == name)
        info = setting;
    }
    if (!info)
      return Out("unknown setting: " + line);
    const bool value = count_arg != 0;
    Core::QueueHostJob([info, value](Core::System&) { Config::SetCurrent(*info, value); });
  }
  else if (cmd == "turn" && words.size() == 2 && ParseFloat(words[1], &x))
  {
    HitzWorld::TurnLifeSize(x);
  }
  else if (cmd == "proj" && words.size() == 1)
  {
    const auto& p = xfmem.projection.rawProjection;
    Out(fmt::format("proj type {} raw {:.6f} {:.6f} {:.6f} {:.6f} {:.6f} {:.6f}",
                    xfmem.projection.type == ProjectionType::Perspective ? "persp" : "ortho", p[0],
                    p[1], p[2], p[3], p[4], p[5]));
  }
#endif
  // watch/code take the CPU thread lock: run them on the host thread like save/load (from this
  // thread the lock froze the game).
  else if (cmd == "watch" && words.size() == 2 && words[1] == "off")
  {
    Core::QueueHostJob([](Core::System& s) { s.GetPowerPC().GetMemChecks().Clear(); });
  }
  else if (cmd == "watch" && words.size() >= 3 && ParseHex(words[1], &addr))
  {
    // Logs (MEMMAP, "MBP ...") every read/write of [addr, addr+len) with the PC doing it.
    int len = 4;
    if (words.size() >= 4)
      ParseInt(words[3], &len);
    const u32 end = addr + static_cast<u32>(std::max(len, 1)) - 1;
    const bool on_read = words[2].find('r') != std::string::npos;
    const bool on_write = words[2].find('w') != std::string::npos;
    Core::QueueHostJob([addr, end, on_read, on_write](Core::System& s) {
      TMemCheck mc;
      mc.start_address = addr;
      mc.end_address = end;
      mc.is_ranged = end > addr;
      mc.is_break_on_read = on_read;
      mc.is_break_on_write = on_write;
      mc.log_on_hit = true;
      mc.break_on_hit = false;
      s.GetPowerPC().GetMemChecks().Add(std::move(mc));
    });
  }
  else if (cmd == "dis" && words.size() == 3 && ParseHex(words[1], &addr) &&
           ParseInt(words[2], &count_arg))
  {
    // Code lives in main RAM: read it directly (no CPU lock needed).
    auto& memory = system.GetMemory();
    for (int i = 0; i < std::min(count_arg, 400); ++i)
    {
      const u32 a = addr + i * 4;
      const u32 phys = a & 0x01FFFFFF;
      if (phys + 4 > memory.GetRamSizeReal())
        break;
      const u8* p = memory.GetRAM() + phys;
      const u32 inst = (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
      Out(fmt::format("dis {:08x} {:08x} {}", a, inst,
                      Common::GekkoDisassembler::Disassemble(inst, a)));
    }
  }
  else if (cmd == "code" && words.size() == 2 && words[1] == "restore")
  {
    Core::QueueHostJob([](Core::System& s) {
      const Core::CPUThreadGuard guard(s);
      for (const auto& [a, original] : s_code_patches)
      {
        PowerPC::MMU::HostWrite<u32>(guard, original, a);
        s.GetJitInterface().InvalidateICache(a, 4, true);
      }
      s_code_patches.clear();
    });
  }
  else if (cmd == "code" && words.size() == 3 && ParseHex(words[1], &addr))
  {
    // Replace one instruction (restorable with "code restore").
    u32 value = 0;
    if (!ParseHex(words[2], &value))
      return Out("bad instruction: " + line);
    Core::QueueHostJob([addr, value](Core::System& s) {
      const Core::CPUThreadGuard guard(s);
      const auto original = PowerPC::MMU::HostTryRead<u32>(guard, addr);
      if (!original)
        return;
      s_code_patches.try_emplace(addr, original->value);
      PowerPC::MMU::HostWrite<u32>(guard, value, addr);
      s.GetJitInterface().InvalidateICache(addr, 4, true);
    });
  }
  else if (cmd == "hold" && words.size() == 2 && words[1] == "off")
  {
    std::lock_guard lk(s_mutex);
    s_holds.clear();
  }
  else if (cmd == "hold" && words.size() == 3 && ParseHex(words[1], &addr))
  {
    HeldBytes h{addr & 0x01FFFFFF, {}};
    const std::string& hex = words[2];
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
      u32 byte = 0;
      if (!ParseHex(hex.substr(i, 2), &byte))
        return Out("bad bytes: " + line);
      h.bytes.push_back(static_cast<u8>(byte));
    }
    std::lock_guard lk(s_mutex);
    s_holds.push_back(std::move(h));
  }
  else
  {
    Out("unknown: " + line);
    return;
  }
  Out("ok: " + line);
}

// The game hides what its TV camera can't see (objects, players, the stadium's props), which
// leaves the stadium behind you (and the field outside the TV picture) empty in Tabletop. In
// Tabletop these instructions are patched to "always visible"; elsewhere the originals come back
// (the big screen only needs what the TV camera sees). Checked every poll, so a savestate load,
// a fresh boot or the game reloading its match module (which restore the original code) is
// patched again. A word is only touched when it holds exactly the original or the patched value.
struct CodePatch
{
  const char* game_id;
  u32 addr;
  u32 orig;
  u32 patched;
  bool match_module;  // Mario Superstar Baseball's game.rel: only valid during a match
};
constexpr CodePatch CODE_PATCHES[] = {
    // NBA Street Vol. 2 (Street MR r2): object-in-view test -> li r3,1; blr
    {"GNZE69", 0x80252168, 0x9421FF90, 0x38600001, false},
    {"GNZE69", 0x8025216C, 0x7C0802A6, 0x4E800020, false},
    // Mario Superstar Baseball: Project Rio's never-cull codes (ProjectRio-Modding).
    {"GYQE01", 0x8001DCF8, 0x7C001B78, 0x38000007, false},  // actor visibility
    {"GYQE01", 0x800BD9DC, 0x7C831B78, 0x38600007, false},
    {"GYQE01", 0x806AA4E4, 0x38000002, 0x38000001, true},   // fielders
    {"GYQE01", 0x806AB8B4, 0x38000000, 0x38000001, true},   // batter, runners
    {"GYQE01", 0x806F7B7C, 0x881A0093, 0x38000003, true},   // stadium hazards
};
// u16 at 0x800E877C: 5 while Mario Superstar Baseball's match module (game.rel) is loaded at its
// fixed addresses (4 = menus).
constexpr u32 MSSB_SCENE_ADDR = 0x800E877C;
std::atomic<bool> s_cull_job_pending{false};

void UpdateCullPatch()
{
#ifdef ENABLE_VR
  if (s_cull_job_pending || !Core::IsRunning(Core::System::GetInstance()))
    return;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* ram = memory.GetRAM();
  if (!ram)
    return;
  const auto word = [&](u32 addr) -> std::optional<u32> {
    const u32 phys = addr & 0x01FFFFFF;
    if (phys + 4 > memory.GetRamSizeReal())
      return std::nullopt;
    const u8* p = ram + phys;
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
  };
  const auto scene = word(MSSB_SCENE_ADDR & ~3u);
  const bool match_module_loaded = scene && ((*scene >> 16) & 0xFFFF) == 5;
  // Never online: a code patch on one headset only would split the two games apart.
  const bool want =
      HitzWorld::GetView() == HitzWorld::View::Tabletop && !NetPlay::IsNetPlayRunning();

  std::vector<std::pair<u32, u32>> writes;  // address, value
  for (const CodePatch& p : CODE_PATCHES)
  {
    if (game_id != p.game_id || (p.match_module && !match_module_loaded))
      continue;
    const auto w = word(p.addr);
    if (!w)
      continue;
    if (want && *w == p.orig)
      writes.emplace_back(p.addr, p.patched);
    else if (!want && *w == p.patched)
      writes.emplace_back(p.addr, p.orig);
  }
  if (writes.empty())
    return;

  s_cull_job_pending = true;
  Core::QueueHostJob([want, writes](Core::System& s) {
    const Core::CPUThreadGuard guard(s);
    for (const auto& [addr, value] : writes)
    {
      PowerPC::MMU::HostWrite<u32>(guard, value, addr);
      s.GetJitInterface().InvalidateICache(addr, 4, true);
    }
    Out(fmt::format("{} ({} words)", want ? "never-cull on (Tabletop)" : "never-cull off",
                    writes.size()));
    s_cull_job_pending = false;
  });
#endif
}

// Mario Superstar Baseball's game state for HitzWorld (Project Rio's map): scene u16 @0x800E877C
// (5 = match module loaded), status u8 @0x80892AAA (1 = at bat; game.rel memory, only valid in a
// match). Other games report -1 (unknown).
void UpdateGameState()
{
#ifdef ENABLE_VR
  if (!Core::IsRunning(Core::System::GetInstance()))
    return;
  if (SConfig::GetInstance().GetGameID() != "GYQE01")
  {
    HitzWorld::SetGameState(-1, 0);
    return;
  }
  auto& memory = Core::System::GetInstance().GetMemory();
  const u8* ram = memory.GetRAM();
  if (!ram || memory.GetRamSizeReal() < 0x00892AAB)
    return;
  const int scene = (int(ram[0x000E877C]) << 8) | ram[0x000E877D];
  const int status = scene == 5 ? ram[0x00892AAA] : 0;
  HitzWorld::SetGameState(scene, status);
#endif
}

// StreetMR r31: a loading screen = the disc read fast for a while (megabytes a second; menus and
// matches read far less, the streamed music isn't counted). On above 1 MB/s over the last second,
// off once under 0.25 MB/s for two seconds. Picks the loading screens' VR 360 environment.
void UpdateDiscLoading()
{
#ifdef ENABLE_VR
  static std::vector<std::pair<Clock::time_point, u64>> samples;
  static bool loading = false;
  static Clock::time_point quiet_since{};
  const auto now = Clock::now();
  samples.emplace_back(now, DVD::DVDThread::TotalBytesRead());
  while (samples.size() > 2 && now - samples.front().first > std::chrono::seconds(1))
    samples.erase(samples.begin());
  const double secs =
      std::chrono::duration<double>(samples.back().first - samples.front().first).count();
  const double rate =
      secs > 0.2 ? static_cast<double>(samples.back().second - samples.front().second) / secs : 0.0;
  if (!loading && rate > 1.0e6)
  {
    loading = true;
    quiet_since = {};
  }
  else if (loading)
  {
    if (rate >= 0.25e6)
      quiet_since = {};
    else if (quiet_since == Clock::time_point{})
      quiet_since = now;
    else if (now - quiet_since > std::chrono::seconds(2))
      loading = false;
  }
  HitzWorld::SetDiscLoading(loading);
#endif
}

void PollLoop()
{
  Common::SetCurrentThreadName("HitzDev");
  const std::string cmd_path = File::GetUserPath(D_USER_IDX) + "hitz_cmd.txt";
  const std::string run_path = cmd_path + ".run";
  auto next_perf = Clock::now() + std::chrono::seconds(5);
  // StreetMR: the same line also goes to files/user/street_perf.txt (fresh every launch), so a
  // session Lance plays can be read afterwards - logcat had rolled over by then (r23 stutter).
  const auto started = Clock::now();
  std::ofstream perf_file(File::GetUserPath(D_USER_IDX) + "street_perf.txt", std::ios::trunc);
  while (true)
  {
    // Game speed every 5 s, so self-tests can tell a 30 fps game from a slow emulator.
    if (Clock::now() >= next_perf)
    {
      next_perf = Clock::now() + std::chrono::seconds(5);  // no catch-up burst after long batches
      std::string view;
#ifdef ENABLE_VR
      view = fmt::format(" view={} {} ptchanges={} backdrop={}{}",
                         HitzWorld::ViewName(HitzWorld::GetView()),
                         HitzModes::StadiumWanted() ? "VR" : "MR",
                         HitzModes::PassthroughSettingChanges(), HitzWorld::CourtKey(),
                         HitzWorld::DisplayStatsText());
#endif
      // r35i: online, how long the game waited for the other headset in these 5 s.
      const std::string online =
          NetPlay::IsNetPlayRunning() ?
              fmt::format(" online_wait_ms={}", NetPlay::TakeStreetPadWaitMs()) :
              std::string();
      const std::string line = fmt::format(
          "t={} fps={:.1f} vps={:.1f} speed={:.0f}%{}{}",
          std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count(),
          g_perf_metrics.GetFPS(), g_perf_metrics.GetVPS(), g_perf_metrics.GetSpeed() * 100.0,
          view, online);
#ifdef ANDROID
      __android_log_print(ANDROID_LOG_INFO, "HitzDev", "perf %s", line.c_str());
#endif
      if (perf_file)
        perf_file << line << std::endl;
    }
    UpdateCullPatch();
    UpdateGameState();
    UpdateDiscLoading();
#ifdef ENABLE_VR
    // StreetMR r33: Lance's snapshot (both sticks held in): census + screenshot, numbered.
    if (HitzWorld::TakeSnapshotRequest())
    {
      static int s_snapshot = 0;
      ++s_snapshot;
      RunCommand("census");
      RunCommand(fmt::format("shot lance_{}", s_snapshot));
      Sleep(700);  // the census is written at the end of the next frame
      const std::string dir = File::GetUserPath(D_USER_IDX);
      File::Copy(dir + "census.txt", dir + fmt::format("census_lance_{}.txt", s_snapshot));
      Out(fmt::format("snapshot {}", s_snapshot));
    }
#endif
    // Rename first so adb can write the next batch while this one runs.
    if (File::Exists(cmd_path) && File::Rename(cmd_path, run_path))
    {
      std::ifstream in(run_path);
      for (std::string line; std::getline(in, line);)
      {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        RunCommand(line);
        // StreetMR r28: also between a batch's commands - a savestate load restores the game's
        // own code, and a census taken in the same batch (r27) ran without the never-cull patch.
        UpdateCullPatch();
      }
      in.close();
      File::Delete(run_path);
    }
    Sleep(50);
  }
}

u8 Axis(float v)
{
  return static_cast<u8>(std::clamp(128.0f + v * 127.0f, 0.0f, 255.0f));
}
}  // namespace

void ApplyInput(int pad_num, GCPadStatus* status)
{
  std::call_once(s_started, [] { std::thread(PollLoop).detach(); });
  if (pad_num != 0)
    return;

#ifdef ENABLE_VR
  // BaseballMR: the right grip is the game's Z, except while it holds the table (Tabletop: it
  // closed on the table).
  // StreetMR: there the right grip is X (steal / trick) and the left grip Z (trick).
  if (HitzWorld::RightHandGrabbing())
    status->button &= ~(PAD_TRIGGER_Z | PAD_BUTTON_X);
  if (HitzWorld::LeftHandGrabbing())
    status->button &= ~PAD_TRIGGER_Z;
#endif

  // BaseballMR: base-throw buttons (Lance): while your fielder holds the ball on a live play,
  // A = home, B = 1st, Y = 2nd, X = 3rd - sent to the game as stick direction + A (its own throw:
  // down = home, right = 1st, up = 2nd, left = 3rd). Batting, pitching (A+down = change-up, B =
  // pickoffs) and fielding without the ball keep the normal buttons. Project Rio's map: game status
  // u8 @0x80892AAA (2 = live ball), fielding team u32 @0x80892994 (0 = P1's team = pad 0), fielder
  // control u8 @0x8088F53B (0x0A = holding the ball).
  if (SConfig::GetInstance().GetGameID() == "GYQE01")
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    const u8* ram = memory.GetRAM();
    if (ram && memory.GetRamSizeReal() > 0x00892AAA &&
        ((u32(ram[0x000E877C]) << 8) | ram[0x000E877D]) == 5 && ram[0x00892AAA] == 2 &&
        ram[0x00892994] == 0 && ram[0x00892995] == 0 && ram[0x00892996] == 0 &&
        ram[0x00892997] == 0 && ram[0x0088F53B] == 0x0A)
    {
      const u16 b = status->button;
      int base = -1;  // 0 home, 1 first, 2 second, 3 third
      if (b & PAD_BUTTON_A)
        base = 0;
      else if (b & PAD_BUTTON_B)
        base = 1;
      else if (b & PAD_BUTTON_Y)
        base = 2;
      else if (b & PAD_BUTTON_X)
        base = 3;
      if (base >= 0)
      {
        static constexpr u8 STICK_X[] = {0x80, 0xFF, 0x80, 0x00};
        static constexpr u8 STICK_Y[] = {0x00, 0x80, 0xFF, 0x80};
        status->button = (b & ~(PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y)) |
                         PAD_BUTTON_A;
        status->analogA = 0xFF;
        status->analogB = 0;
        status->stickX = STICK_X[base];
        status->stickY = STICK_Y[base];
      }
    }
  }

  std::lock_guard lk(s_mutex);
  if (!s_holds.empty())
  {
    // The pad is polled on the CPU thread about once a frame: rewrite the held bytes.
    auto& memory = Core::System::GetInstance().GetMemory();
    for (const HeldBytes& h : s_holds)
    {
      for (size_t i = 0; i < h.bytes.size() && h.addr + i < memory.GetRamSizeReal(); ++i)
        memory.GetRAM()[h.addr + i] = h.bytes[i];
    }
  }
  const Injection& in = s_injection;
  if (in.buttons)
  {
    status->button |= in.buttons;
    if (in.buttons & PAD_BUTTON_A)
      status->analogA = 0xFF;
    if (in.buttons & PAD_BUTTON_B)
      status->analogB = 0xFF;
    if (in.buttons & PAD_TRIGGER_L)
      status->triggerLeft = 0xFF;
    if (in.buttons & PAD_TRIGGER_R)
      status->triggerRight = 0xFF;
  }
  if (in.stick)
  {
    status->stickX = Axis(in.stick_x);
    status->stickY = Axis(in.stick_y);
  }
  if (in.cstick)
  {
    status->substickX = Axis(in.cstick_x);
    status->substickY = Axis(in.cstick_y);
  }

  // StreetMR (NBA Street Vol. 2), Lance: "B is to SHOOT on OFFENSE, and B IS TO BLOCK ON
  // DEFENSE. DONT CHANGE ANY OTHER CONTROL". The game shoots with B and blocks/rebounds
  // with Y. Only B changes, by who has the ball - read from the game's match object, through its
  // static pointer at 0x803DF7C0 (-32448(r13)); measured 2026-10-14 over 45 labelled samples:
  //   +0x110 (u32)  the team with the ball, 0 or 1
  //   +0x9A + team  1 if that team is the human's, 0 if it's the CPU's
  //   +0xF7 (u8)    3 = the ball in the air (a shot, a loose ball), 4/5 = a team has it, 6 = dead
  // Your team has it: B stays B (shoot). The other team has it: B becomes Y (block). Ball in the
  // air: B and Y (block / rebound, or a tip with turbo). r3-r31 read a court prop's redraw flag
  // instead (0x8065040F, cleared every frame); r32-r33 pressed B+Y always (Y on offense = a trick).
  // Online 1v1 (r35): both teams are human, so "your team" is the one this controller plays for:
  //   +0x24..+0x38  six u32, the controller port running each player on court (4 = none),
  //                 team 0's three then team 1's (the human's 0 moves between its team's three
  //                 slots as he switches players; same 45 samples)
  // The port is the in-game one (NetPlay maps this headset's pad 0 to port 0 or 1). In NetPlay
  // this runs before the pad is sent, so both headsets get the same buttons.
  if ((status->button & PAD_BUTTON_B) && SConfig::GetInstance().GetGameID() == "GNZE69")
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    const u8* ram = memory.GetRAM();
    const u32 size = memory.GetRamSizeReal();
    const auto word = [&](u32 phys) -> u32 {
      return (u32(ram[phys]) << 24) | (u32(ram[phys + 1]) << 16) | (u32(ram[phys + 2]) << 8) |
             ram[phys + 3];
    };
    constexpr u32 STREET_MATCH_PTR = 0x003DF7C0;
    if (ram && STREET_MATCH_PTR + 4 <= size)
    {
      const u32 match = word(STREET_MATCH_PTR);
      const u32 phys = match & 0x01FFFFFF;
      if ((match & 0xFE000000) == 0x80000000 && phys + 0x114 <= size)
      {
        const u32 team = word(phys + 0x110);
        const u8 state = ram[phys + 0xF7];
        if (team <= 1 && state >= 3 && state <= 6)
        {
          const u32 port = static_cast<u32>(NetPlay::LocalPadToInGamePadUnlocked(pad_num));
          int my_team = -1;
          for (u32 slot = 0; slot < 6; ++slot)
          {
            if (word(phys + 0x24 + slot * 4) == port)
              my_team = static_cast<int>(slot / 3);
          }
          // No slot (between plays?): the one-player rule, the other team being the CPU's.
          const bool their_ball =
              my_team >= 0 ? static_cast<u32>(my_team) != team :
                             ram[phys + 0x9A + team] == 0 && ram[phys + 0x9A + (1 - team)] == 1;
          if (state == 3)
          {
            status->button |= PAD_BUTTON_Y;
          }
          else if (state != 6 && their_ball)
          {
            status->button = (status->button & ~PAD_BUTTON_B) | PAD_BUTTON_Y;
            status->analogB = 0;
          }
        }
      }
    }
  }
  status->isConnected = true;
}
}  // namespace HitzDev
