// Copyright 2026 StreetMR
// SPDX-License-Identifier: GPL-2.0-or-later

// StreetMR r35: voice chat in online games (Lance: "an online 1 v 1 mode ... with voice chat as
// well"), ported from Quest 3 Strikers' xr_voice (r44). The microphone is coded with Opus in 20 ms
// frames that go to the other player as NetPlay messages (unreliable, on their own channel); theirs
// are played through an AAudio stream of their own, next to the game's sound.
#pragma once

namespace StreetVoice
{
// The start screen's "Voice chat" switch (on by default there), set before every host / join.
void SetEnabled(bool enabled);
// Android's RECORD_AUDIO answer, from the start screen. Without it you still hear the other player.
void SetMicAllowed(bool allowed);

// The online game, from its boot to its end (StreetOnline).
void MatchBegin();
void MatchEnd();
}  // namespace StreetVoice
