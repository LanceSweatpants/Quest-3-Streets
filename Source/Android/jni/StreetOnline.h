// Copyright 2026 StreetMR
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

class BootSessionData;

namespace StreetOnline
{
// MainAndroid's Run: an online game waiting to boot (NetPlay's settings and synced saves), and the
// path NetPlay chose. Empty outside an online session.
std::unique_ptr<BootSessionData> TakePendingBoot(std::string* path);
// MainAndroid's Run, after Core::Shutdown: the online game is over; the session ends with it.
void EmulationEnded();
// This headset quits an online game (StopEmulation): the other headset is told, and our CPU
// thread is let out of its wait for the other pad so the stop can finish.
void LeaveGame();
}  // namespace StreetOnline
