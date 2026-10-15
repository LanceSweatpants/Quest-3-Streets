// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

// HitzMR developer hooks: a command file in the user folder lets adb drive the game for
// self-tests (press buttons, move the stick, take screenshots, save/load states, dump or peek
// game memory). Commands go in <user>/hitz_cmd.txt, one per line; results go to hitz_out.txt.
//
//   press A B START ... <ms>   hold buttons (A B X Y Z L R START UP DOWN LEFT RIGHT)
//   stick <x> <y> <ms>         main stick, -1..1 (y up is +)
//   cstick <x> <y> <ms>        C-stick
//   wait <ms>
//   shot <name>                screenshot -> ScreenShots/<game id>/<name>.png
//   save <slot> / load <slot>  save states
//   dump <name>                MEM1 -> Dump/<name>.bin
//   peek <hex addr> <count>    bytes -> hitz_out.txt
//   poke <hex addr> <hex bytes>

#pragma once

struct GCPadStatus;

namespace HitzDev
{
// Overlays injected input on pad 0. Starts the command poller the first time it runs.
void ApplyInput(int pad_num, GCPadStatus* status);
}  // namespace HitzDev
