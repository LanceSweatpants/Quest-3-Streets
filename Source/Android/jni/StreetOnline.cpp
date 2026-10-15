// Copyright 2026 StreetMR
// SPDX-License-Identifier: GPL-2.0-or-later

// StreetMR r35: online 1v1 on the Quest (Lance, 2026-10-14: "add an online 1 v 1 mode where people
// control 1 team each and face off in a head to head game with voice chat as well"). Dolphin's own
// NetPlay does the work: both headsets run the same game in lockstep and only swap their pads. The
// host gets an 8-character code from Dolphin's traversal server and tells it to the friend, who
// types it in; the traversal server introduces the two (NAT punch-through) and they talk directly.
//
// This file is NetPlay's "dialog" for the Java start screen (StartupHandler): one status line the
// screen polls (STATE|CODE|MESSAGE), the host starting the game by itself once the friend is in
// with the same game, the boot handed to MainAndroid's Run, and the session ending with the game.
// NetPlay's objects are made and destroyed on worker threads (their constructors wait up to 5 s,
// their destructors join threads); s_lock only ever guards the fields here, never a call into them.
// Every session has its own dialog object, so a cancelled session's last words (a disconnect while
// it is torn down) can't land on the next one.

#include "jni/StreetOnline.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <jni.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/TraversalClient.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/VR/HitzModes.h"
#include "VideoCommon/VR/HitzWorld.h"

#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/StreetVoice.h"

namespace
{
enum class State
{
  Idle,
  Working,  // getting a code / connecting
  Waiting,  // host: the code is up, no friend yet
  Ready,    // both in, starting
  Boot,     // the game must start now (Java launches the emulation)
  Playing,
  Error,
  Ended,
};

const char* StateName(State state)
{
  switch (state)
  {
  case State::Idle:
    return "IDLE";
  case State::Working:
    return "WORKING";
  case State::Waiting:
    return "WAITING";
  case State::Ready:
    return "READY";
  case State::Boot:
    return "BOOT";
  case State::Playing:
    return "PLAYING";
  case State::Error:
    return "ERROR";
  case State::Ended:
    return "ENDED";
  }
  return "IDLE";
}

class StreetNetPlayUI;

std::mutex s_lock;  // everything below
State s_state = State::Idle;
std::string s_code;
std::string s_message;
u32 s_session = 0;  // bumped by every host / join / quit: a worker of an old session stops
std::shared_ptr<const UICommon::GameFile> s_game;
std::shared_ptr<StreetNetPlayUI> s_ui;
std::shared_ptr<NetPlay::NetPlayServer> s_server;
std::shared_ptr<NetPlay::NetPlayClient> s_client;
std::unique_ptr<BootSessionData> s_pending_boot;
std::string s_boot_path;

// For the current session only. An error or the end stays up until the screen has shown it.
void SetStatus(u32 session, State state, std::string message)
{
  std::lock_guard lk(s_lock);
  if (session != s_session)
    return;
  if (s_state == State::Error || s_state == State::Ended)
    return;
  s_state = state;
  s_message = std::move(message);
  INFO_LOG_FMT(NETPLAY, "StreetOnline: {} {}", StateName(s_state), s_message);
}

State GetState()
{
  std::lock_guard lk(s_lock);
  return s_state;
}

void QueueCoreStop()
{
  if (Core::IsRunning(Core::System::GetInstance()))
    Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }, true);
}

void StopEmulation()
{
  // The CPU thread may be waiting for the other headset's pad: out of NetPlay first. The client's
  // StopGame calls back into StreetNetPlayUI::StopGame, which queues the stop.
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_lock);
    client = s_client;
  }
  if (client)
    client->StopGame();
  QueueCoreStop();
}

class StreetNetPlayUI final : public NetPlay::NetPlayUI
{
public:
  StreetNetPlayUI(u32 session, bool hosting) : m_session(session), m_hosting(hosting) {}

  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override
  {
    {
      std::lock_guard lk(s_lock);
      if (m_session != s_session)
        return;
      s_pending_boot = std::move(boot_session_data);
      s_boot_path = filename;
    }
    Status(State::Boot, "Starting the game...");
    StreetVoice::MatchBegin();
  }

  // Called from inside NetPlayClient::StopGame (NetPlay already let go of the CPU thread).
  void StopGame() override { QueueCoreStop(); }
  bool IsHosting() const override { return m_hosting; }

  void Update() override {}
  void AppendChat(const std::string& msg) override { INFO_LOG_FMT(NETPLAY, "chat: {}", msg); }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier&, const std::string& netplay_name) override
  {
    INFO_LOG_FMT(NETPLAY, "StreetOnline: the host's game is {}", netplay_name);
  }
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}

  // The host said start: like Dolphin's own dialog, this side starts its client's game (which
  // calls BootGame below). r35g: r35f left this empty - both headsets sat at "Starting...".
  // Off NetPlay's thread, as Dolphin's dialog does it on its UI thread.
  void OnMsgStartGame() override
  {
    std::shared_ptr<NetPlay::NetPlayClient> client;
    std::shared_ptr<const UICommon::GameFile> game;
    {
      std::lock_guard lk(s_lock);
      if (m_session != s_session)
        return;
      client = s_client;
      game = s_game;
    }
    if (!client || !game)
    {
      Status(State::Error, "The game couldn't start (no game file).");
      return;
    }
    std::thread([client, path = game->GetFilePath()] { client->StartGame(path); }).detach();
  }
  // The client ran its StopGame just before this.
  void OnMsgStopGame() override { QueueCoreStop(); }
  void OnMsgPowerButton() override {}

  void OnPlayerConnect(const std::string& player) override
  {
    if (m_hosting)
      Status(State::Ready, fmt::format("{} joined. Starting...", player));
  }

  void OnPlayerDisconnect(const std::string& player) override
  {
    Status(State::Ended, fmt::format("{} left the game.", player));
    if (IsCurrent())
      StopEmulation();
  }

  void OnPadBufferChanged(u32 buffer) override
  {
    INFO_LOG_FMT(NETPLAY, "StreetOnline: pad buffer {}", buffer);
  }
  void OnHostInputAuthorityChanged(bool) override {}

  void OnDesync(u32 frame, const std::string& player) override
  {
    WARN_LOG_FMT(NETPLAY, "StreetOnline: desync at frame {} ({})", frame, player);
  }

  void OnConnectionLost() override
  {
    Status(State::Ended, "Lost the connection to the other player.");
    if (IsCurrent())
      StopEmulation();
  }

  void OnConnectionError(const std::string& message) override
  {
    WARN_LOG_FMT(NETPLAY, "StreetOnline: connection error: {}", message);
    Status(State::Error, m_hosting ? fmt::format("Couldn't start hosting: {}", message) :
                                     "Couldn't reach that game. Check the code, and that the "
                                     "host is still waiting.");
  }

  void OnTraversalError(Common::TraversalClient::FailureReason error) override
  {
    using Reason = Common::TraversalClient::FailureReason;
    const char* why = error == Reason::BadHost             ? "the code server wasn't found" :
                      error == Reason::VersionTooOld       ? "this version is too old" :
                      error == Reason::ServerForgotAboutUs ? "the code server forgot this game" :
                      error == Reason::SocketSendError     ? "the network refused to send" :
                      error == Reason::ResendTimeout       ? "the code server didn't answer" :
                                                             "unknown";
    Status(State::Error,
           fmt::format("Couldn't get online ({}). Check the headset's Wi-Fi and try again.", why));
  }

  void OnTraversalStateChanged(Common::TraversalClient::State state) override
  {
    if (!m_hosting || state != Common::TraversalClient::State::Connected ||
        !Common::g_TraversalClient)
    {
      return;
    }
    const auto id = Common::g_TraversalClient->GetHostID();
    const std::string code(id.begin(), id.end());
    {
      std::lock_guard lk(s_lock);
      if (m_session != s_session || s_state != State::Working)
        return;
      s_code = code;
    }
    Status(State::Waiting, "Tell your friend this code. Waiting for them to join...");
  }

  void OnGameStartAborted() override
  {
    Status(State::Error, "The game couldn't start (copying the host's save failed).");
  }
  void OnGolferChanged(bool, const std::string&) override {}
  void OnTtlDetermined(u8) override {}

  bool IsRecording() override { return false; }

  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found) override
  {
    std::shared_ptr<const UICommon::GameFile> game;
    {
      std::lock_guard lk(s_lock);
      game = s_game;
    }
    const NetPlay::SyncIdentifierComparison comparison =
        game ? game->CompareSyncIdentifier(sync_identifier) :
               NetPlay::SyncIdentifierComparison::DifferentGame;
    if (found)
      *found = comparison;
    if (comparison != NetPlay::SyncIdentifierComparison::SameGame)
    {
      WARN_LOG_FMT(NETPLAY, "StreetOnline: the host's game isn't this one ({})",
                   static_cast<int>(comparison));
      return nullptr;
    }
    return game;
  }

  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override
  {
    return {};
  }
  void ShowGameDigestDialog(const std::string&) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string&) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string&, u64, const std::vector<int>&) override
  {
    Status(State::Ready, "Copying the host's save...");
  }
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

private:
  bool IsCurrent() const
  {
    std::lock_guard lk(s_lock);
    return m_session == s_session;
  }
  void Status(State state, std::string message) { SetStatus(m_session, state, std::move(message)); }

  const u32 m_session;
  const bool m_hosting;
};

// Frees a session's NetPlay objects off the caller's thread: the client first, like Dolphin's own
// UI (its destructor disconnects, up to 3 s), then the server, then their dialog. A new session
// waits for these first (WaitForDisposals): the traversal client is one global that a late
// destructor would otherwise take away from the new session.
std::mutex s_dispose_lock;
std::condition_variable s_dispose_done;
int s_disposing = 0;

void Dispose(std::shared_ptr<NetPlay::NetPlayClient> client,
             std::shared_ptr<NetPlay::NetPlayServer> server, std::shared_ptr<StreetNetPlayUI> ui)
{
  if (!client && !server && !ui)
    return;
  {
    std::lock_guard lk(s_dispose_lock);
    ++s_disposing;
  }
  std::thread([client = std::move(client), server = std::move(server),
               ui = std::move(ui)]() mutable {
    client.reset();
    server.reset();
    ui.reset();
    std::lock_guard lk(s_dispose_lock);
    --s_disposing;
    s_dispose_done.notify_all();
  }).detach();
}

void WaitForDisposals()
{
  std::unique_lock lk(s_dispose_lock);
  s_dispose_done.wait(lk, [] { return s_disposing == 0; });
}

// r35e: same-Wi-Fi play. Lance's first test (two headsets in one house, three codes) never
// connected: the code server hands the friend the host's PUBLIC address, and most home routers
// don't loop a connection to their own public address back inside ("hairpin NAT"). So the host
// also answers on the local network: the friend's headset first asks every address of its own
// subnet (UDP 2627, "STREETMR?<code>"), the host answers "STREETMR!<code> <port>", and the two
// connect directly; only when nobody answers does the code server get used.
constexpr u16 LAN_PORT = 2627;
constexpr std::string_view LAN_ASK = "STREETMR?";
constexpr std::string_view LAN_ANSWER = "STREETMR!";

// Host: answers the friend's LAN question while this session lasts (the code may come later).
void LanResponder(u32 session, u16 game_port)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  timeval tv{0, 300000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LAN_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    WARN_LOG_FMT(NETPLAY, "StreetOnline: LAN port {} busy", LAN_PORT);
    close(fd);
    return;
  }
  INFO_LOG_FMT(NETPLAY, "StreetOnline: answering on the LAN (game port {})", game_port);
  char buf[64];
  while (true)
  {
    std::string code;
    {
      std::lock_guard lk(s_lock);
      if (session != s_session || s_state == State::Error || s_state == State::Ended ||
          s_state == State::Playing)
      {
        break;
      }
      code = s_code;
    }
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0 || code.empty())
      continue;
    const std::string_view msg(buf, static_cast<size_t>(n));
    if (msg.substr(0, LAN_ASK.size()) != LAN_ASK || msg.substr(LAN_ASK.size()) != code)
      continue;
    const std::string reply = fmt::format("{}{} {}", LAN_ANSWER, code, game_port);
    sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), from_len);
    INFO_LOG_FMT(NETPLAY, "StreetOnline: told {} where the game is", inet_ntoa(from.sin_addr));
  }
  close(fd);
}

// Friend: asks the local network for the host of this code; true with its address if it answers
// within a second and a half.
bool FindOnLan(const std::string& code, std::string* ip, u16* port)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return false;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  const std::string ask = fmt::format("{}{}", LAN_ASK, code);
  const auto send_to = [&](u32 host_order_ip) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(LAN_PORT);
    to.sin_addr.s_addr = htonl(host_order_ip);
    sendto(fd, ask.data(), ask.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  };
  // Our own Wi-Fi address: the one the system would send to the internet from (a UDP "connect"
  // sends nothing). getifaddrs needs Android 7, and this app is built for older too. Home
  // networks are /24, so that's what gets asked: every address of it, plus its broadcast.
  u32 self = 0;
  {
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(53);
    out.sin_addr.s_addr = inet_addr("8.8.8.8");
    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    if (probe >= 0 && connect(probe, reinterpret_cast<sockaddr*>(&out), sizeof(out)) == 0 &&
        getsockname(probe, reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
    {
      self = ntohl(local.sin_addr.s_addr);
    }
    if (probe >= 0)
      close(probe);
  }
  if (self != 0)
  {
    const u32 net = self & 0xFFFFFF00u;
    send_to(net | 0xFFu);
    for (u32 h = 1; h < 255; ++h)
    {
      if ((net | h) != self)
        send_to(net | h);
    }
    send_to(0xFFFFFFFFu);
  }
  else
  {
    send_to(0xFFFFFFFFu);
  }
  bool found = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  char buf[64];
  while (!found && std::chrono::steady_clock::now() < deadline)
  {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0)
      continue;
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0)
      continue;
    const std::string msg(buf, static_cast<size_t>(n));
    const std::string want = fmt::format("{}{} ", LAN_ANSWER, code);
    if (msg.rfind(want, 0) != 0)
      continue;
    const int p_num = std::atoi(msg.c_str() + want.size());
    if (p_num <= 0 || p_num > 65535)
      continue;
    *ip = inet_ntoa(from.sin_addr);
    *port = static_cast<u16>(p_num);
    found = true;
  }
  close(fd);
  return found;
}

// Host: once the friend is in with the same game, wait for a ping or two, size the pad buffer to
// the link and start. Both headsets then boot (NetPlay copies the host's memory card over first).
void HostWatch(u32 session)
{
  using namespace std::chrono_literals;
  std::chrono::steady_clock::time_point both_since{};
  while (true)
  {
    std::this_thread::sleep_for(200ms);
    std::shared_ptr<NetPlay::NetPlayClient> client;
    std::shared_ptr<NetPlay::NetPlayServer> server;
    {
      std::lock_guard lk(s_lock);
      if (session != s_session || s_state == State::Error || s_state == State::Ended ||
          s_state == State::Boot || s_state == State::Playing)
      {
        return;
      }
      client = s_client;
      server = s_server;
    }
    if (!client || !server)
      return;

    const std::vector<const NetPlay::Player*> players = client->GetPlayers();
    if (players.size() < 2)
    {
      both_since = {};
      continue;
    }
    u32 ping = 0;
    std::string friend_name;
    bool unknown = false, different = false;
    for (const NetPlay::Player* p : players)
    {
      ping = std::max(ping, p->ping);
      if (!p->IsHost())
        friend_name = p->name;
      if (p->game_status == NetPlay::SyncIdentifierComparison::Unknown)
        unknown = true;
      else if (p->game_status != NetPlay::SyncIdentifierComparison::SameGame)
        different = true;
    }
    if (unknown || different)
    {
      // Unknown: their game check hasn't come in yet.
      if (different)
      {
        SetStatus(session, State::Ready,
                  fmt::format("{} joined, but their game file is a different version (it must "
                              "be NBA Street Vol. 2, USA).",
                              friend_name));
      }
      both_since = {};
      continue;
    }
    if (both_since == std::chrono::steady_clock::time_point{})
      both_since = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - both_since < 2500ms)
      continue;

    // Each frame of buffer is 16.7 ms of head start for the pads: enough to cover the trip one
    // way plus some wobble, so neither game waits on the other.
    const u32 buffer = std::clamp<u32>(ping / 25 + 2, 3, 12);
    server->AdjustPadBufferSize(buffer);
    INFO_LOG_FMT(NETPLAY, "StreetOnline: starting with {} (ping {} ms, buffer {})", friend_name,
                 ping, buffer);
    SetStatus(session, State::Ready, fmt::format("{} joined. Starting...", friend_name));
    if (!server->RequestStartGame())
      SetStatus(session, State::Error, "The game couldn't start.");
    return;
  }
}

// A new session: the game file read and checked, the last session's objects let go.
std::shared_ptr<StreetNetPlayUI> Begin(u32 session, bool hosting, const std::string& game_path)
{
  auto game = std::make_shared<UICommon::GameFile>(game_path);
  if (!game->IsValid())
  {
    SetStatus(session, State::Error, "The game file couldn't be read.");
    return nullptr;
  }
  auto ui = std::make_shared<StreetNetPlayUI>(session, hosting);
  std::shared_ptr<NetPlay::NetPlayClient> old_client;
  std::shared_ptr<NetPlay::NetPlayServer> old_server;
  std::shared_ptr<StreetNetPlayUI> old_ui;
  {
    std::lock_guard lk(s_lock);
    if (session != s_session)
      return nullptr;
    old_client = std::move(s_client);
    old_server = std::move(s_server);
    old_ui = std::exchange(s_ui, ui);
    s_game = std::move(game);
    s_pending_boot.reset();
    s_code.clear();
  }
  Dispose(std::move(old_client), std::move(old_server), std::move(old_ui));
  WaitForDisposals();
  return ui;
}

void Host(u32 session, std::string game_path, std::string nickname)
{
  SetStatus(session, State::Working, "Getting your code...");
  const std::shared_ptr<StreetNetPlayUI> ui = Begin(session, true, game_path);
  if (!ui)
    return;

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const u16 traversal_port_alt = Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT);
  // r35f: the next port up when Dolphin's (2626) is taken - e.g. by the other Street app on the
  // same headset still holding a session (Lance: "i cant even host a game from my build").
  const u16 first_port = Config::Get(Config::NETPLAY_LISTEN_PORT);
  std::shared_ptr<NetPlay::NetPlayServer> server;
  for (u16 port = first_port; port < first_port + 8; ++port)
  {
    server = std::make_shared<NetPlay::NetPlayServer>(
        port, false, ui.get(),
        NetPlay::NetTraversalConfig{true, traversal_host, traversal_port, traversal_port_alt});
    if (server->is_connected)
      break;
    WARN_LOG_FMT(NETPLAY, "StreetOnline: port {} taken", port);
    server.reset();
    Common::ReleaseTraversalClient();
  }
  if (!server)
  {
    SetStatus(session, State::Error, "Couldn't open a game on this headset's network.");
    return;
  }
  if (!server->is_connected)
  {
    SetStatus(session, State::Error, "Couldn't open a game on this headset's network.");
    Dispose(nullptr, std::move(server), ui);
    return;
  }

  std::shared_ptr<const UICommon::GameFile> game;
  {
    std::lock_guard lk(s_lock);
    game = s_game;
  }
  // r35f (Lance's two-headset test stuck at "Copying the host's save"): no save or cheat-code
  // copy before an online game. Both headsets then start on the same blank card, and the game
  // starts at once.
  Config::SetCurrent(Config::NETPLAY_SAVEDATA_LOAD, false);
  Config::SetCurrent(Config::NETPLAY_SAVEDATA_WRITE, false);
  Config::SetCurrent(Config::NETPLAY_SYNC_CODES, false);
  const Core::TitleDatabase title_database;
  server->SetHostInputAuthority(false);
  server->AdjustPadBufferSize(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  server->ChangeGame(game->GetSyncIdentifier(), game->GetNetPlayName(title_database));

  // Our own seat: a client on this headset, straight to the local server.
  auto client = std::make_shared<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), ui.get(), nickname,
      NetPlay::NetTraversalConfig{false, traversal_host, traversal_port});
  if (!client->IsConnected())
  {
    SetStatus(session, State::Error, "Couldn't open a game on this headset.");
    Dispose(std::move(client), std::move(server), ui);
    return;
  }

  {
    std::lock_guard lk(s_lock);
    if (session != s_session)
    {
      Dispose(std::move(client), std::move(server), ui);  // cancelled meanwhile
      return;
    }
    s_server = server;
    s_client = client;
  }
  std::thread(LanResponder, session, server->GetPort()).detach();
  // The code may have come before now: show it.
  if (Common::g_TraversalClient &&
      Common::g_TraversalClient->GetState() == Common::TraversalClient::State::Connected)
  {
    ui->OnTraversalStateChanged(Common::TraversalClient::State::Connected);
  }
  HostWatch(session);
}

void Join(u32 session, std::string code, std::string game_path, std::string nickname)
{
  SetStatus(session, State::Working, "Connecting...");
  const std::shared_ptr<StreetNetPlayUI> ui = Begin(session, false, game_path);
  if (!ui)
    return;

  const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  std::shared_ptr<NetPlay::NetPlayClient> client;
  // Same Wi-Fi first (r35e), straight to the host's local address.
  std::string lan_ip;
  u16 lan_port = 0;
  if (FindOnLan(code, &lan_ip, &lan_port))
  {
    INFO_LOG_FMT(NETPLAY, "StreetOnline: found the host on the LAN at {}:{}", lan_ip, lan_port);
    client = std::make_shared<NetPlay::NetPlayClient>(
        lan_ip, lan_port, ui.get(), nickname,
        NetPlay::NetTraversalConfig{false, traversal_host, traversal_port});
    if (!client->IsConnected())
    {
      WARN_LOG_FMT(NETPLAY, "StreetOnline: the LAN connection failed, trying the code server");
      Dispose(std::move(client), nullptr, nullptr);
      WaitForDisposals();
      client.reset();
      // Its error isn't the last word: back to connecting.
      std::lock_guard lk(s_lock);
      if (session != s_session)
        return;
      s_state = State::Working;
      s_message = "Connecting...";
    }
  }
  if (!client)
  {
    client = std::make_shared<NetPlay::NetPlayClient>(
        code, Config::Get(Config::NETPLAY_CONNECT_PORT), ui.get(), nickname,
        NetPlay::NetTraversalConfig{true, traversal_host, traversal_port});
  }
  if (!client->IsConnected())
  {
    SetStatus(session, State::Error,
              "Couldn't reach that game. Check the code, and that the host is still waiting.");
    Dispose(std::move(client), nullptr, ui);
    return;
  }
  {
    std::lock_guard lk(s_lock);
    if (session != s_session)
    {
      Dispose(std::move(client), nullptr, ui);
      return;
    }
    s_client = client;
  }
  SetStatus(session, State::Ready, "Connected. The host's game starts in a moment...");
}

u32 NewSession()
{
  // r35h: NetPlay's own log lines on (into logcat), so a failed online game says why.
  Common::Log::LogManager::GetInstance()->SetEnable(Common::Log::LogType::NETPLAY, true);
  std::lock_guard lk(s_lock);
  ++s_session;
  s_state = State::Working;
  s_message.clear();
  s_code.clear();
  return s_session;
}

void Quit()
{
  std::shared_ptr<NetPlay::NetPlayClient> client;
  std::shared_ptr<NetPlay::NetPlayServer> server;
  std::shared_ptr<StreetNetPlayUI> ui;
  {
    std::lock_guard lk(s_lock);
    ++s_session;
    client = std::move(s_client);
    server = std::move(s_server);
    ui = std::move(s_ui);
    s_pending_boot.reset();
    s_state = State::Idle;
    s_code.clear();
    s_message.clear();
  }
  StreetVoice::MatchEnd();
  Dispose(std::move(client), std::move(server), std::move(ui));
}
}  // namespace

namespace StreetOnline
{
std::unique_ptr<BootSessionData> TakePendingBoot(std::string* path)
{
  std::lock_guard lk(s_lock);
  if (!s_pending_boot)
    return nullptr;
  *path = s_boot_path;
  s_state = State::Playing;
  s_message = "Playing online.";
  return std::move(s_pending_boot);
}

void LeaveGame()
{
  std::shared_ptr<NetPlay::NetPlayClient> client;
  {
    std::lock_guard lk(s_lock);
    client = s_client;
  }
  if (!client)
    return;
  INFO_LOG_FMT(NETPLAY, "StreetOnline: leaving the online game");
  client->RequestStopGame();
  client->StopGame();
}

void EmulationEnded()
{
  {
    std::lock_guard lk(s_lock);
    if (s_state != State::Playing && s_state != State::Ended && s_state != State::Error)
      return;
    if (s_state == State::Playing)
    {
      s_state = State::Ended;
      s_message = "The online game is over.";
    }
  }
  StreetVoice::MatchEnd();
}
}  // namespace StreetOnline

extern "C" {

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetNetHost(
    JNIEnv* env, jclass, jstring jGamePath, jstring jNickname)
{
  // Lance 2026-10-15: no online in the public build yet (the dev build keeps it).
  if (HitzModes::StreetPublic())
    return JNI_FALSE;
  const State state = GetState();
  if (state == State::Boot || state == State::Playing)
    return JNI_FALSE;
  const u32 session = NewSession();
  std::thread(Host, session, GetJString(env, jGamePath), GetJString(env, jNickname)).detach();
  return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetNetJoin(
    JNIEnv* env, jclass, jstring jCode, jstring jGamePath, jstring jNickname)
{
  if (HitzModes::StreetPublic())
    return JNI_FALSE;
  const State state = GetState();
  if (state == State::Boot || state == State::Playing)
    return JNI_FALSE;
  std::string code = GetJString(env, jCode);
  const u32 session = NewSession();
  if (code.size() != Common::NETPLAY_CODE_SIZE)
  {
    SetStatus(session, State::Error, "The code has 8 characters (0-9 and a-f).");
    return JNI_FALSE;
  }
  std::thread(Join, session, std::move(code), GetJString(env, jGamePath),
              GetJString(env, jNickname))
      .detach();
  return JNI_TRUE;
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetNetStatus(JNIEnv* env,
                                                                                        jclass)
{
  std::string line;
  {
    std::lock_guard lk(s_lock);
    line = fmt::format("{}|{}|{}", StateName(s_state), s_code, s_message);
  }
  return ToJString(env, line);
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetNetQuit(JNIEnv*, jclass)
{
  Quit();
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetVoiceSetEnabled(
    JNIEnv*, jclass, jboolean enabled)
{
  StreetVoice::SetEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetVoiceSetMicAllowed(
    JNIEnv*, jclass, jboolean allowed)
{
  StreetVoice::SetMicAllowed(allowed == JNI_TRUE);
}

// r35c: the start screen's VR background choice (see HitzWorld::SetBackdropStyle).
JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_StreetSetBackdrop(
    JNIEnv* env, jclass, jstring jStyle)
{
  HitzWorld::SetBackdropStyle(GetJString(env, jStyle));
}
}
