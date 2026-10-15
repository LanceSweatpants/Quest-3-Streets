// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

// HitzMR world lock: Tabletop (the field shrunk onto a real table) and the life-size stadium.
//
// The game bakes its broadcast camera into every position matrix. All the static stadium geometry
// (field, walls, stands) is drawn with the camera's view matrix alone, so the matrix that carries
// the most geometry in a frame IS the camera. Undoing it (through OpenXRManager's camera anchor)
// pins the game world to the room, so the field no longer swings with the TV camera; the
// units-per-meter scale then shrinks it to table size or keeps it life-size.
//
// BaseballMR (Mario Superstar Baseball): the field is the big flat surface at the bottom of the
// stadium; home plate faces you on the table (the side of the field the TV camera films from);
// in Tabletop - Stadium you stand at field level behind home plate with the stadium around you.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/VR/OpenXRInputState.h"

namespace HitzWorld
{
enum class View : int
{
  Jumbotron = 0,  // flat big screen (OpenXR quad)
  Tabletop = 1,   // stereo, world-locked, table size
  Rinkside = 2,   // stereo, world-locked, life-size (the stadium around you in Tabletop VR)
};

View GetView();
void SetView(View view);
const char* ViewName(View view);

// Video thread: one perspective draw with a single position matrix (row-major 3x4), weighted by
// its index count.
void OnDraw(const float* rows3x4, u32 weight);

// Video thread, every perspective draw: learns the field outline from the static stadium geometry
// and sorts the draw for Tabletop. Draws entirely outside the field (stands, crowd, scoreboard)
// are Skip in Tabletop MR and Arena in Tabletop VR (drawn life-size around the player).
// Positions are floats (2 or 3 components) at pos_offset in each stride-sized vertex. Pass either
// one 3x4 position matrix, or the transform-matrix table (64 float4 rows) plus the per-vertex
// matrix index byte offset.
// Both = drawn on the table AND life-size around the player (the field under your feet in
// Tabletop VR; the game's effects in Tabletop MR and VR).
// `blended` = the draw alpha-blends (effects do).
enum class DrawClass : int
{
  Rink = 0,
  Skip = 1,
  Arena = 2,
  Both = 3,
};
DrawClass ClassifyDraw(const u8* vertices, u32 count, u32 stride, int pos_offset, int components,
                       const float* single_matrix, const float* matrix_table, int posmtx_offset,
                       bool arena_life_size, bool blended);

// Video thread, at the start of every draw (perspective or not): no class, no scale yet.
void BeginDraw();

// The class of the draw being set up right now (video thread); OpenXRManager picks the life-size
// arena rig for Arena draws.
void SetCurrentDrawClass(DrawClass c);
DrawClass GetCurrentDrawClass();

// Video thread (OpenXRManager, per draw): Tabletop draws the players and the ball bigger than the
// field (like Strikers' 1.5x). True when the current draw is scaled by *k about *center (the rigid
// view space the anchor works in, before GetViewScale's squash).
bool GetDrawScale(float* k, std::array<float, 3>* center);

// Video thread (OpenXRManager, per draw): the world-lock anchor from the freshest camera - this
// frame's exact camera once the field has been drawn (no prediction lag), else the prediction.
// arena = the life-size rig (Tabletop VR surroundings). False = use the per-frame anchor.
bool GetCurrentAnchor(bool arena, std::array<float, 3>* out_position,
                      std::array<float, 9>* out_rotation, float* out_units_per_meter);

// Video thread: the game's view space is squashed per axis (Mario Superstar Baseball scales view
// Y by ~0.93 and corrects it in its projection, which VR replaces). Eye projection rows built for
// the rigid view space divide their xyz by this to take the game's view positions.
std::array<float, 3> GetViewScale();

// Video thread: the life-size rig (you stand behind home plate) used for Arena draws and the
// second copy of Both draws in Tabletop.
bool GetArenaAnchor(const std::array<float, 3>& home, const std::array<float, 3>& head,
                    float head_yaw, std::array<float, 3>* out_position,
                    std::array<float, 9>* out_rotation, float* out_units_per_meter);

// Video thread, once per game frame: settles this frame's camera and predicts the next one.
void CommitFrame();

// A savestate was loaded: the camera jumps, so no motion is carried over (Hitz r13).
void OnStateLoaded();

// Mip levels of extra sharpness for draws on the table ("texsharp" tuning).
float TableLodBias();
// StreetMR r27: the internal resolution Tabletop renders at ("tune tableir", default 3; 0 = the
// Jumbotron's).
int TableInternalResolution();
// StreetMR r29: the game's fog is off for everything drawn while this holds (Tabletop; "tune
// tablefog 1" keeps it). Any thread.
bool TableFogOff();
// StreetMR r33 (Lance: the popups and arrows "make them fit inside the headsets FOV"): in
// Tabletop the game's 2D layer goes on a head-locked panel this many degrees tall (0 = not in
// Tabletop: the 2D layer is handled as before). "tune hudsize <deg>".
float TableHudDegrees();
// StreetMR r33: Lance's snapshot - both thumbsticks held in for a second ask HitzDev for a census
// and a screenshot of that moment (files/user/census_lance_<n>.txt, ScreenShots/lance_<n>).
void RequestSnapshot();
bool TakeSnapshotRequest();

// Any thread (HitzDev's poll loop, Mario Superstar Baseball): scene = u16 @0x800E877C (5 = a match
// is loaded, 4 = menus; -1 = unknown game), status = u8 @0x80892AAA (1 = at bat: the pitch-and-swing
// camera behind home plate). The field is only learned during a match (menu backdrops fooled it:
// Lance r9 "middle of the sky"), everything is relearned when the scene changes, and home plate's
// side is learned from the at-bat camera only.
void SetGameState(int scene, int status);

// Video thread: the camera-anchor rig that maps the room onto the game's view space for the next
// frame. home = OpenXR home position; head/head_yaw place the field the first time a view is
// entered. Returns false when no world lock applies (Jumbotron, or no camera seen yet).
bool GetAnchor(const std::array<float, 3>& home, const std::array<float, 3>& head, float head_yaw,
               std::array<float, 3>* out_position, std::array<float, 9>* out_rotation,
               float* out_units_per_meter);

// Pacing thread: grab the table (left grip anywhere, right grip on the table, or hold the left
// stick in): one hand carries and turns it, two hands resize and turn it. Returns true while
// grabbing.
bool OnControllers(const std::array<Common::VR::OpenXRControllerState, 2>& controllers);

// Any thread: the right grip is holding the table right now (the game's Z is held back).
bool RightHandGrabbing();
// Any thread: the left grip is holding the table right now (its game button is held back).
bool LeftHandGrabbing();

// StreetMR r27: the Jumbotron screen by hand, as in Quest 3 Strikers Charged. Input thread: the
// left grip while the left controller points at the screen moves it (further than the hand when
// it's far), the right grip joins to turn and resize it. One screen per room (MR / VR), saved.
bool UpdateScreenGrab(const Common::VR::OpenXRControllerState& left,
                      const Common::VR::OpenXRControllerState& right);
// Video thread, every frame: where the head is (centre, yaw), for placing the screen ahead.
void NoteHead(const std::array<float, 3>& position, float yaw);
// The left stick tap in the Jumbotron: the screen back in front of you at its size.
void BringScreenHere();
// The right stick tap in the Jumbotron: a new size (height, metres) at `distance` ahead of you.
void SetScreenPreset(float height, float distance);
// Video thread, the flat quad layer: the screen's pose and width (height = width / aspect). Places
// it `default_distance` ahead at `default_height` the first time.
void GetScreen(float aspect, float default_height, float default_distance,
               std::array<float, 3>* position, float* yaw, float* width);

// Tuning, any thread: "upm" = game units per metre, "tablesize" = metres the field's longest side
// takes on the table, "charscale" = players/ball size on the table, "inset" = metres you stand
// inside the field's back edge in Tabletop - Stadium. Also read from
// files/user/baseball_tuning.txt at the first frame. Re-places every view.
void SetTuning(const std::string& key, float value);

// Dev: record the next frame's perspective draws (bounds, kind, class) to files/user/census.txt.
void RequestCensus();

// Dev (StreetMR, "tune levelhead <degrees>"): non-zero = OpenXRManager renders as if the head
// were level, facing -Z, pitched down this many degrees (unworn self-test screenshots).
float DevLevelHead();

// StreetMR diagnostic ("tune flatgate 0/1", default 0): the flat Jumbotron skips an XFB while the
// pacing thread still owes an xrEndFrame for the last released image (Present.cpp).
bool FlatGate();

// StreetMR ("tune flatmr 0/1", default 1): the flat Jumbotron shows the room (passthrough) behind
// its quad while the Passthrough setting is on (OpenXRManager::EndFrameDetached).
bool FlatPassthrough();

// StreetMR ("tune vrsky <0xRRGGBB as a number>", 0 = off): the colour Tabletop clears to instead of
// black when no passthrough shows behind it (FramebufferManager::ClearEFB).
u32 VRSkyColor();

// StreetMR dev ("tune texlist 1"): records the names of the textures drawn (2D = orthographic
// draws, else the 3D ones outside a match) to files/user/texlist_2d.txt / texlist_menu3d.txt.
bool TexListActive();
void NoteTexture(const std::string& name, bool ortho);

// StreetMR: texture sizes NBA Street's movie planes come in (a new texture every frame); the
// texture dumper skips them so only menu/loading art reaches files/user/Dump for the HD pack.
bool SkipTextureDump(u32 width, u32 height);

// StreetMR "VR 360" (Lance: no black around the big screen or the table in VR). Two cube pictures
// per court, both made from the game's own life-size render of that court (Capture-VR360.ps1,
// then Build-VR360.ps1 on the PC): <court>.png = the whole court around you, behind the big screen
// in VR (Lance r23: "the jumbotron VR ... THATS where i'd want to be surrounded by the cage");
// <court>_sky.png = a calm sky in the court's own colours with no court in it, behind the table in
// VR ("chill but still reflect the map/weather"; the game's effects still fall around you).
// Files in files/user/Load/StreetVR360/. Courts are told apart by the court floor's texture.
// Video thread, right after ClassifyDraw: this draw is the court floor.
bool IsCourtFloorDraw();
// Video thread: the court floor draw's first texture (name, or hash) = the court's key.
void NoteCourtTexture(const std::string& key);
// Any thread: the current court's key, and a counter that changes with it.
// StreetMR r31: outside a match it's one of the original pre-match environments instead
// (Build-VR360Env.ps1): "streetmr_loading" while the disc is loading, else "streetmr_menu".
std::string CourtKey();
// r35c: the VR background the start screen picked: "vegas" (Hoop City) or a gradient name
// (sunset, ocean, purple, midnight, mint, fire, gray) -> "streetmr_gradient_<name>".
void SetBackdropStyle(const std::string& style);
// HitzDev's poll: the disc is being read fast (a loading screen).
void SetDiscLoading(bool loading);
// Video thread: this texture is the current court's floor texture (its key).
bool IsCourtTexture(const std::string& name);
u32 CourtKeyGeneration();
// The backend (video thread) has the current court's 360 picture loaded.
void SetCourtPanoramaReady(bool ready);
bool CourtPanoramaReady();
// OpenXRManager, every frame: the backend can show a backdrop cube (cube layers + the GL backend).
// Tabletop VR then always sits under the court's calm sky (its own, or a plain one), never the
// game's court drawn life-size around you (Lance r23).
void SetBackdropSupported(bool supported);
bool BackdropSupported();
// Any thread: the table's heading in the room (radians); 0 until it's placed.
float TableYaw();
// Dev ("tune vr360cap 1"): capture mode for the 360 pictures, in Tabletop VR: only the life-size
// court is drawn (no table, players, ball, effects or HUD), and the eyes take the capture pose.
bool Vr360Capture();
// Dev capture pose ("tune capyaw / cappitch / capfov <degrees>", pitch down positive): true while
// capture mode is on; both eyes then look from the head's centre with a square field of view.
bool DevCapturePose(float* yaw_deg, float* pitch_deg, float* fov_deg);
// True while a census frame is being recorded; the caller may then describe the next draw's
// material (textures, blending, depth test) with SetCensusMaterial before ClassifyDraw.
bool CensusActive();
void SetCensusMaterial(std::string material);
// A free-form census line (EFB copies, clears) in draw order.
void CensusNote(const std::string& line);

// Mario Superstar Baseball's dirt shadows: the game renders the players' shadows into a 640x448
// EFB copy (4-bit) and reads it back as a 640x480 paletted texture, which speckles the dirt
// circles on the Quest. "tune shadowfix": 0 = as is, 1 = no shadow texture (plain dirt),
// 2 = decode it from RAM only (EFB copies to RAM, no partial updates from the copy),
// 3 = dev (black dirt), 4 = dev (reverse texture loads), 5 = the default: those draws use their
// first TEV stage only (dirt x light, no player shadow) - the 2-stage version speckles.
int ShadowFix();
// EFB copy destinations (BPStructs), so the texture cache can tell a paletted reinterpretation
// of a recent copy.
void NoteEfbCopy(u32 address);
bool IsRecentEfbCopyTarget(u32 address);

// StreetMR r25 ("tune refresh <Hz>", default 120, 0 = the headset's choice): the display rate
// OpenXRManager asks for. The game runs at 60: at 72 Hz a frame repeated 13-17 times a second;
// at 120 Hz every frame shows exactly twice.
float DisplayRefreshWanted();
// r35n: the XR pacing thread fills every display refresh in MR too (as in VR). "tune mreager 0":
// in MR it paces to the game instead.
bool MrEagerHeartbeat();
// r35l: the XR pacing thread's last 5 s - display rate in use, game frames shown (fresh) and
// display frames that showed a game frame again (repeat) - and the perf line's text for them.
void NoteDisplayStats(float hz, u32 fresh, u32 repeat, u32 cycles, double seconds,
                      const std::string& extra = {});
std::string DisplayStatsText();

// Jumbotron: send every (non render-to-texture) draw to the fork's virtual screen.
bool ForceScreenRoute();

// Video thread: a match is on (the field was drawn in the last two seconds).
bool InMatch();
// r35n: the same, from any thread (the CPU thread's pad poll).
bool InMatchAnyThread();

// Video thread: the game's latest perspective projection (xfmem rawProjection), for the census.
void NotePerspectiveProjection(const float* raw6);

// Puts the table back in front of you at waist height (single tap left stick, like Strikers).
void BringTableHere();

// Dev: turn the life-size stadium around you by `degrees` (to screenshot what's behind you on an
// unworn headset).
void TurnLifeSize(float degrees);

// Video thread, right after ClassifyDraw: if this draw is the field floor (once a frame, in
// Tabletop), returns the table box under the mini field in the draw's own model (= world)
// coordinates. Corner i: bit0 = high on the first horizontal axis, bit1 = high on the second,
// bit2 = bottom (else top).
bool TakeTableBox(std::array<std::array<float, 3>, 8>* corners);
}  // namespace HitzWorld
