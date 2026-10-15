// Copyright 2026 StreetMR
// SPDX-License-Identifier: GPL-2.0-or-later

// StreetMR r35: see StreetVoice.h. Ported from Quest 3 Strikers' xr_voice.cpp (r44, where Lance
// rated the echo handling "PERFECT").
//
// One thread of its own opens and closes the two AAudio streams (opening one takes tens of
// milliseconds) and reads the microphone in 20 ms frames: Opus, a level gate (frames that are only
// room noise are coded but not sent, so a quiet player costs nothing), and each frame that passes
// goes straight out as a NetPlay message. The other player's frames go into a short queue that the
// speaker stream's callback decodes from, after 60 ms of them have gathered; a lost or late frame
// is filled in by Opus's own concealment.
#include "jni/StreetVoice.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <dlfcn.h>
#include <opus.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "Core/NetPlayProto.h"

namespace StreetVoice
{
namespace
{
void VoiceLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void VoiceLog(const char* fmt, ...)
{
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  __android_log_print(ANDROID_LOG_INFO, "StreetVoice", "%s", line);
}
#define VLOG(...) VoiceLog(__VA_ARGS__)

// AAudio came with Android 8 (API 26) and the app is built for older ones too (its functions are
// hidden from this build), so it is looked up at run time - the Quest has it - like cubeb's AAudio
// backend does. The signatures are AAudio.h's.
#define STREET_AAUDIO_FUNCS(X)                                                                     \
  X(AAudio_createStreamBuilder, aaudio_result_t, AAudioStreamBuilder**)                            \
  X(AAudioStreamBuilder_setDirection, void, AAudioStreamBuilder*, aaudio_direction_t)              \
  X(AAudioStreamBuilder_setSampleRate, void, AAudioStreamBuilder*, int32_t)                        \
  X(AAudioStreamBuilder_setChannelCount, void, AAudioStreamBuilder*, int32_t)                      \
  X(AAudioStreamBuilder_setFormat, void, AAudioStreamBuilder*, aaudio_format_t)                    \
  X(AAudioStreamBuilder_setUsage, void, AAudioStreamBuilder*, aaudio_usage_t)                      \
  X(AAudioStreamBuilder_setContentType, void, AAudioStreamBuilder*, aaudio_content_type_t)         \
  X(AAudioStreamBuilder_setDataCallback, void, AAudioStreamBuilder*, AAudioStream_dataCallback,    \
    void*)                                                                                         \
  X(AAudioStreamBuilder_setErrorCallback, void, AAudioStreamBuilder*, AAudioStream_errorCallback,  \
    void*)                                                                                         \
  X(AAudioStreamBuilder_setInputPreset, void, AAudioStreamBuilder*, aaudio_input_preset_t)         \
  X(AAudioStreamBuilder_openStream, aaudio_result_t, AAudioStreamBuilder*, AAudioStream**)         \
  X(AAudioStreamBuilder_delete, aaudio_result_t, AAudioStreamBuilder*)                             \
  X(AAudioStream_getSampleRate, int32_t, AAudioStream*)                                            \
  X(AAudioStream_getChannelCount, int32_t, AAudioStream*)                                          \
  X(AAudioStream_requestStart, aaudio_result_t, AAudioStream*)                                     \
  X(AAudioStream_requestStop, aaudio_result_t, AAudioStream*)                                      \
  X(AAudioStream_close, aaudio_result_t, AAudioStream*)                                            \
  X(AAudioStream_read, aaudio_result_t, AAudioStream*, void*, int32_t, int64_t)                    \
  X(AAudio_convertResultToText, const char*, aaudio_result_t)

struct AAudioApi
{
#define STREET_AAUDIO_DECLARE(f, ret, ...) ret (*f)(__VA_ARGS__) = nullptr;
  STREET_AAUDIO_FUNCS(STREET_AAUDIO_DECLARE)
#undef STREET_AAUDIO_DECLARE
};
AAudioApi s_aa;

bool LoadAAudio()
{
  static const bool loaded = [] {
    void* lib = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr)
    {
      VLOG("voice: no libaaudio.so");
      return false;
    }
    bool all = true;
#define STREET_AAUDIO_LOAD(f, ...)                                                                 \
  s_aa.f = reinterpret_cast<decltype(s_aa.f)>(dlsym(lib, #f));                                     \
  if (s_aa.f == nullptr)                                                                           \
  {                                                                                                \
    VLOG("voice: AAudio lacks " #f);                                                               \
    all = false;                                                                                   \
  }
    STREET_AAUDIO_FUNCS(STREET_AAUDIO_LOAD)
#undef STREET_AAUDIO_LOAD
    return all;
  }();
  return loaded;
}

constexpr int kRate = 48000;
constexpr int kFrame = kRate / 50;     // 20 ms
constexpr size_t kMaxIn = 12;          // 240 ms queued to play at most: older is dropped
constexpr size_t kStartDepth = 3;      // 60 ms gathered before the speaker starts (again)
constexpr uint64_t kStartWaitMs = 90;  // or this long after the first one came, whichever is first
constexpr int kHangFrames = 15;        // the gate stays open 300 ms after the voice drops
constexpr float kGateRms = 0.012f;     // about -38 dBFS: speech at a headset's mic, not the room

uint64_t NowMs()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Packet
{
  uint16_t seq = 0;
  uint8_t len = 0;
  uint8_t data[255];
};

std::mutex m;  // everything below
bool on = true;
bool inMatch = false;
std::deque<Packet> inQ;  // from the other player
uint64_t inFirstMs = 0;  // when inQ last went from empty to not
bool playing = false;    // the speaker is draining inQ (else gathering)
uint32_t statSent = 0, statHeard = 0, statUnderruns = 0, statDropped = 0;

std::atomic<bool> threadUp{false};
std::atomic<bool> micAllowed{false};
std::atomic<bool> outDead{false}, inDead{false};

// ---------------------------------------------------------------------------------------------
// The speaker: the other player's voice, decoded in the stream's own callback
// ---------------------------------------------------------------------------------------------

struct Playback
{
  OpusDecoder* dec = nullptr;
  float pcm[kFrame * 2];
  int count = 0, at = 0;
  int rate = kRate;
  int concealed = 0;  // frames made up in a row (the callback's own)
};
Playback play;

aaudio_data_callback_result_t OnSpeaker(AAudioStream*, void*, void* audio, int32_t frames)
{
  auto* outBuf = static_cast<float*>(audio);
  int32_t done = 0;
  while (done < frames)
  {
    if (play.at < play.count)
    {
      const int n = std::min<int>(frames - done, play.count - play.at);
      for (int i = 0; i < n; ++i)
        outBuf[done + i] = std::clamp(play.pcm[play.at + i] * 1.4f, -1.0f, 1.0f);
      play.at += n;
      done += n;
      continue;
    }
    Packet p;
    bool got = false, conceal = false;
    {
      std::lock_guard<std::mutex> lock(m);
      if (!playing &&
          (inQ.size() >= kStartDepth || (!inQ.empty() && NowMs() - inFirstMs >= kStartWaitMs)))
      {
        playing = true;
        play.concealed = 0;
      }
      if (playing)
      {
        if (!inQ.empty())
        {
          p = inQ.front();
          inQ.pop_front();
          got = true;
          play.concealed = 0;
        }
        else if (play.concealed < 3)
        {
          // Ran dry mid-word (a frame lost or late): Opus fills up to 60 ms in, so speech carries
          // on where it should instead of after a gap of gathering again.
          conceal = true;
          ++play.concealed;
        }
        else
        {
          playing = false;  // the end of a sentence (or a long hold-up): gather again
          ++statUnderruns;
        }
      }
    }
    if (!got && !conceal)
    {
      std::memset(outBuf + done, 0, sizeof(float) * static_cast<size_t>(frames - done));
      break;
    }
    const int n = conceal ?
                      opus_decode_float(play.dec, nullptr, 0, play.pcm, play.rate / 50, 0) :
                      opus_decode_float(play.dec, p.data, p.len, play.pcm, play.rate / 50, 0);
    play.count = n > 0 ? n : 0;
    play.at = 0;
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void OnStreamError(AAudioStream*, void* which, aaudio_result_t error)
{
  // Not closed here (AAudio forbids it in the callback): the voice thread reopens it.
  (which == &play ? outDead : inDead) = true;
  VLOG("voice: %s stream error %s", which == &play ? "speaker" : "microphone",
       s_aa.AAudio_convertResultToText(error));
}

AAudioStream* OpenSpeaker()
{
  AAudioStreamBuilder* b = nullptr;
  if (s_aa.AAudio_createStreamBuilder(&b) != AAUDIO_OK)
    return nullptr;
  s_aa.AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  s_aa.AAudioStreamBuilder_setSampleRate(b, kRate);
  s_aa.AAudioStreamBuilder_setChannelCount(b, 1);
  s_aa.AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  // The game's own volume (media), not a phone call's: the Quest's volume buttons work on it.
  s_aa.AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
  s_aa.AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
  s_aa.AAudioStreamBuilder_setDataCallback(b, OnSpeaker, nullptr);
  s_aa.AAudioStreamBuilder_setErrorCallback(b, OnStreamError, &play);
  AAudioStream* s = nullptr;
  aaudio_result_t r = s_aa.AAudioStreamBuilder_openStream(b, &s);
  s_aa.AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK)
  {
    VLOG("voice: no speaker stream (%s)", s_aa.AAudio_convertResultToText(r));
    return nullptr;
  }
  play.rate = s_aa.AAudioStream_getSampleRate(s);
  int err = 0;
  if (play.dec != nullptr)
    opus_decoder_destroy(play.dec);
  play.dec = opus_decoder_create(play.rate, 1, &err);
  if (play.dec == nullptr || s_aa.AAudioStream_getChannelCount(s) != 1 || play.rate / 50 > kFrame * 2)
  {
    VLOG("voice: speaker stream at %d Hz x%d is not one Opus can feed", play.rate,
         s_aa.AAudioStream_getChannelCount(s));
    s_aa.AAudioStream_close(s);
    return nullptr;
  }
  play.count = play.at = 0;
  outDead = false;
  if ((r = s_aa.AAudioStream_requestStart(s)) != AAUDIO_OK)
  {
    VLOG("voice: the speaker stream did not start (%s)", s_aa.AAudio_convertResultToText(r));
    s_aa.AAudioStream_close(s);
    return nullptr;
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// The microphone
// ---------------------------------------------------------------------------------------------

struct Capture
{
  AAudioStream* stream = nullptr;
  OpusEncoder* enc = nullptr;
  int rate = kRate, channels = 1, frame = kFrame;
  int16_t raw[kFrame * 2];  // as read (up to two channels)
  int16_t pcm[kFrame];      // one channel, one 20 ms frame
  int have = 0;
  uint16_t seq = 0;
  int hang = 0;
  bool hasPrev = false;  // the frame before the gate opened, sent with it (the word's start)
  Packet prev;
  uint64_t retryAt = 0;
};
Capture cap;

void CloseMic()
{
  if (cap.stream != nullptr)
  {
    s_aa.AAudioStream_requestStop(cap.stream);
    s_aa.AAudioStream_close(cap.stream);
    cap.stream = nullptr;
  }
  if (cap.enc != nullptr)
  {
    opus_encoder_destroy(cap.enc);
    cap.enc = nullptr;
  }
}

bool OpenMic()
{
  AAudioStreamBuilder* b = nullptr;
  if (s_aa.AAudio_createStreamBuilder(&b) != AAUDIO_OK)
    return false;
  s_aa.AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  s_aa.AAudioStreamBuilder_setSampleRate(b, kRate);
  s_aa.AAudioStreamBuilder_setChannelCount(b, 1);
  s_aa.AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  // The voice-call path: the system's echo canceller and noise suppressor, so the game's sound and
  // the other player's voice out of the headset's speakers stay out of what they hear.
  s_aa.AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
  s_aa.AAudioStreamBuilder_setErrorCallback(b, OnStreamError, &cap);
  aaudio_result_t r = s_aa.AAudioStreamBuilder_openStream(b, &cap.stream);
  s_aa.AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK)
  {
    VLOG("voice: no microphone stream (%s)", s_aa.AAudio_convertResultToText(r));
    cap.stream = nullptr;
    return false;
  }
  cap.rate = s_aa.AAudioStream_getSampleRate(cap.stream);
  cap.channels = s_aa.AAudioStream_getChannelCount(cap.stream);
  cap.frame = cap.rate / 50;
  int err = 0;
  const bool opusRate = cap.rate == 8000 || cap.rate == 12000 || cap.rate == 16000 ||
                        cap.rate == 24000 || cap.rate == 48000;
  cap.enc = opusRate && cap.channels >= 1 && cap.channels <= 2 ?
                opus_encoder_create(cap.rate, 1, OPUS_APPLICATION_VOIP, &err) :
                nullptr;
  if (cap.enc == nullptr)
  {
    VLOG("voice: microphone at %d Hz x%d is not one Opus takes", cap.rate, cap.channels);
    CloseMic();
    return false;
  }
  opus_encoder_ctl(cap.enc, OPUS_SET_BITRATE(20000));
  opus_encoder_ctl(cap.enc, OPUS_SET_COMPLEXITY(5));
  opus_encoder_ctl(cap.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  cap.have = 0;
  cap.hang = 0;
  cap.hasPrev = false;
  inDead = false;
  if ((r = s_aa.AAudioStream_requestStart(cap.stream)) != AAUDIO_OK)
  {
    VLOG("voice: the microphone did not start (%s)", s_aa.AAudio_convertResultToText(r));
    CloseMic();
    return false;
  }
  VLOG("voice: microphone on (%d Hz)", cap.rate);
  return true;
}

// Frames out, as [count u8] then per frame [seq u16][len u8][len bytes] (Strikers' layout).
void Send(const Packet* frames, uint8_t count)
{
  std::vector<uint8_t> msg;
  msg.reserve(1 + count * (3 + 64));
  msg.push_back(count);
  for (uint8_t i = 0; i < count; ++i)
  {
    const Packet& p = frames[i];
    msg.push_back(static_cast<uint8_t>(p.seq));
    msg.push_back(static_cast<uint8_t>(p.seq >> 8));
    msg.push_back(p.len);
    msg.insert(msg.end(), p.data, p.data + p.len);
  }
  NetPlay::SendStreetVoice(msg.data(), msg.size());
  std::lock_guard<std::mutex> lock(m);
  statSent += count;
}

// One 20 ms frame in cap.pcm: coded, and sent if the gate is open.
void ProcessFrame()
{
  double sum = 0.0;
  for (int i = 0; i < cap.frame; ++i)
    sum += static_cast<double>(cap.pcm[i]) * cap.pcm[i];
  const float rms = static_cast<float>(std::sqrt(sum / cap.frame) / 32768.0);
  Packet p;
  p.seq = cap.seq++;
  const opus_int32 n = opus_encode(cap.enc, cap.pcm, cap.frame, p.data, sizeof(p.data));
  if (n <= 0)
    return;
  p.len = static_cast<uint8_t>(n);
  if (rms >= kGateRms)
  {
    if (cap.hang == 0 && cap.hasPrev)
    {
      const Packet both[2] = {cap.prev, p};
      cap.hang = kHangFrames - 1;
      cap.hasPrev = false;
      Send(both, 2);
      return;
    }
    cap.hang = kHangFrames;
  }
  if (cap.hang > 0)
  {
    --cap.hang;
    cap.hasPrev = false;
    Send(&p, 1);
  }
  else
  {
    cap.prev = p;
    cap.hasPrev = true;
  }
}

// Whatever the microphone has, up to one frame, waiting at most 40 ms for it.
void ReadMic()
{
  const int want = cap.frame - cap.have;
  const aaudio_result_t got = s_aa.AAudioStream_read(cap.stream, cap.raw, want, 40'000'000);
  if (got < 0)
  {
    VLOG("voice: microphone read failed (%s)", s_aa.AAudio_convertResultToText(got));
    inDead = true;
    return;
  }
  for (int i = 0; i < got; ++i)
  {
    cap.pcm[cap.have + i] =
        cap.channels == 2 ?
            static_cast<int16_t>((static_cast<int>(cap.raw[2 * i]) + cap.raw[2 * i + 1]) / 2) :
            cap.raw[i];
  }
  cap.have += got;
  if (cap.have == cap.frame)
  {
    cap.have = 0;
    ProcessFrame();
  }
}

// The other player's frames (NetPlay's thread), same layout as Send.
void TakeIncoming(const uint8_t* data, size_t size)
{
  if (size == 0)
    return;
  std::lock_guard<std::mutex> lock(m);
  if (!on || !inMatch)
    return;  // the switch off: neither way
  const uint8_t n = data[0];
  size_t at = 1;
  for (uint8_t i = 0; i < n; ++i)
  {
    if (at + 3 > size)
      return;
    Packet p;
    p.seq = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
    p.len = data[at + 2];
    at += 3;
    if (p.len == 0 || at + p.len > size)
      return;
    std::memcpy(p.data, data + at, p.len);
    at += p.len;
    if (inQ.empty())
      inFirstMs = NowMs();
    inQ.push_back(p);
    ++statHeard;
    if (inQ.size() > kMaxIn)
    {
      inQ.pop_front();
      ++statDropped;
    }
  }
}

void VoiceThread()
{
  AAudioStream* speaker = nullptr;
  uint64_t lastStatsMs = NowMs();
  bool noMicLogged = false;
  for (;;)
  {
    bool want = false;
    {
      std::lock_guard<std::mutex> lock(m);
      want = on && inMatch;
    }
    if (!want)
    {
      if (speaker != nullptr || cap.stream != nullptr)
      {
        CloseMic();
        if (speaker != nullptr)
        {
          s_aa.AAudioStream_requestStop(speaker);
          s_aa.AAudioStream_close(speaker);
          speaker = nullptr;
        }
        std::lock_guard<std::mutex> lock(m);
        VLOG("voice: off (%u frames sent, %u heard, %u gaps, %u dropped)", statSent, statHeard,
             statUnderruns, statDropped);
        inQ.clear();
        playing = false;
      }
      usleep(50000);
      continue;
    }
    if (outDead.load() && speaker != nullptr)
    {
      s_aa.AAudioStream_close(speaker);
      speaker = nullptr;
    }
    if (inDead.load() && cap.stream != nullptr)
    {
      CloseMic();
      cap.retryAt = NowMs() + 2000;
    }
    if (speaker == nullptr)
    {
      speaker = OpenSpeaker();
      if (speaker == nullptr)
      {
        usleep(500000);
        continue;
      }
      VLOG("voice: speaker on (%d Hz)", play.rate);
    }
    if (cap.stream == nullptr && NowMs() >= cap.retryAt)
    {
      if (micAllowed.load())
      {
        if (!OpenMic())
          cap.retryAt = NowMs() + 5000;
        noMicLogged = false;
      }
      else
      {
        cap.retryAt = NowMs() + 3000;
        if (!noMicLogged)
          VLOG("voice: no microphone permission; you hear the other player, they don't hear you");
        noMicLogged = true;
      }
    }
    if (cap.stream != nullptr)
      ReadMic();
    else
      usleep(20000);
    if (NowMs() - lastStatsMs > 10000)
    {
      lastStatsMs = NowMs();
      std::lock_guard<std::mutex> lock(m);
      VLOG("voice: %u frames sent, %u heard, %u gaps, %u dropped, %zu waiting to play", statSent,
           statHeard, statUnderruns, statDropped, inQ.size());
    }
  }
}

void EnsureThread()
{
  if (!LoadAAudio())
    return;
  bool expected = false;
  if (threadUp.compare_exchange_strong(expected, true))
  {
    NetPlay::SetStreetVoiceReceiver(TakeIncoming);
    std::thread(VoiceThread).detach();
  }
}
}  // namespace

void SetEnabled(bool enabled)
{
  {
    std::lock_guard<std::mutex> lock(m);
    on = enabled;
  }
  VLOG("voice chat %s", enabled ? "on" : "off");
}

void SetMicAllowed(bool allowed)
{
  micAllowed = allowed;
  VLOG("voice: microphone %s", allowed ? "allowed" : "not allowed");
}

void MatchBegin()
{
  bool want = false;
  {
    std::lock_guard<std::mutex> lock(m);
    inMatch = true;
    inQ.clear();
    playing = false;
    statSent = statHeard = statUnderruns = statDropped = 0;
    want = on;
  }
  if (want)
    EnsureThread();
  VLOG("voice: online game started, voice chat %s", want ? "on" : "off");
}

void MatchEnd()
{
  std::lock_guard<std::mutex> lock(m);
  inMatch = false;
  inQ.clear();
  playing = false;
}
}  // namespace StreetVoice
