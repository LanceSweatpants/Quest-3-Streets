// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/HitzWorld.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/VR/HitzModes.h"

namespace HitzWorld
{
namespace
{
using Mat3 = std::array<float, 9>;  // row-major
using Vec3 = std::array<float, 3>;

// A camera: view = scale * (rot * world + trans), rot orthogonal (row-major), scale per view axis
// (Mario Superstar Baseball squashes view Y by ~0.93 and undoes it in its projection).
struct Camera
{
  Mat3 rot{1, 0, 0, 0, 1, 0, 0, 0, 1};
  Vec3 trans{};
  Vec3 scale{1, 1, 1};
};

std::atomic<int> s_view{static_cast<int>(View::Jumbotron)};

// ---- Per-frame matrix vote (video thread only) ----
struct Candidate
{
  float m[12];
  u64 weight;
};
std::vector<Candidate> s_candidates;

// ---- Ground vote (BaseballMR) ----
// Mario Superstar Baseball positions every model with its own matrix (field, stands, the town
// around the park, each body part), so "the matrix with the most geometry" is just the stands.
// The field itself is perfectly flat in its own model space (the grass and dirt pieces all sit at
// one height), so the matrix carrying the most perfectly flat, field-sized surface IS the field:
// the camera follows it, and the world becomes the field's own model space (up = its flat axis).
struct GroundCandidate
{
  float m[12];
  double area;
  int thin_axis;
  float level;
};
std::vector<GroundCandidate> s_ground;
bool s_ground_mode = false;          // the camera follows the field's matrix
int s_field_axis = -1;               // the field's flat axis (world up) in ground mode
float s_field_up_sign = 1.0f;        // which way along it the camera sits
float s_field_model_level = 0.0f;
u32 s_frames_without_ground = 0;
double s_last_ground_area = 0.0;

// Game state from HitzDev (Mario Superstar Baseball): scene 5 = match, status 1 = at bat.
std::atomic<int> s_game_scene{-1};
std::atomic<int> s_game_status{0};
std::atomic<bool> s_scene_reset{false};

bool InMatchScene()
{
  const int scene = s_game_scene.load(std::memory_order_relaxed);
  return scene < 0 || scene == 5;  // unknown game: always
}
bool s_have_camera = false;
bool s_have_prev = false;
Camera s_cam;       // this frame's camera
Camera s_cam_prev;  // previous frame's camera
Camera s_cam_next;  // prediction used for the next frame's draws
// This frame's exact camera: the field's own matrix, taken from its first piece drawn this frame.
// Draws after it (the stadium, the players) are classified and anchored with it instead of the
// prediction, so the table doesn't swim while the TV camera pans.
Camera s_cam_exact;
bool s_exact_this_frame = false;
u32 s_exact_frames = 0;   // frames with an exact camera, for the log
std::array<float, 3> s_last_home{};  // OpenXR home position (room metres), for per-draw anchors

const Camera& CurrentCamera()
{
  return s_exact_this_frame ? s_cam_exact : s_cam_next;
}
u32 s_log_frames = 0;
u32 s_last_candidates = 0;
u64 s_last_best_weight = 0;
u64 s_last_second_weight = 0;
float s_last_scale = 0.0f;

// ---- Placement (shared with the pacing thread) ----
struct Placement
{
  bool placed = false;
  View placed_for = View::Jumbotron;
  Mat3 world_to_rink{1, 0, 0, 0, 1, 0, 0, 0, 1};  // world axes -> field frame (Y up, -Z = to centre field)
  Vec3 position{};       // field centre in the room (metres)
  float yaw = 0.0f;      // field heading in the room (radians)
  float size = 1.0f;     // room metres per game metre (Tabletop ~1/80, life-size 1)
  float units_per_meter_life = 1.0f;  // game view units per real metre
  Vec3 rink_center{};                 // world point placed at `position` (the field's centre)
  float half_width_m = 50.0f;         // field half extents in the field frame, life-size metres
  float half_depth_m = 50.0f;
  u32 ice_gen = 0;                    // field outline generation placed with (0 = none yet)
  bool user_moved = false;            // grabbed by Lance: never re-place automatically
};
std::mutex s_placement_mutex;
std::array<Placement, 3> s_places;  // per View
std::array<float, 3> s_last_head{};  // latest head position passed to the anchors (room metres)

// ---- Grab state (pacing thread only) ----
// Lance's rule for every MR project (2026-10-11, via Hitz r17): one hand moves the table (up/down
// and sideways, never turning); both hands turn it about the vertical and resize it, around the
// midpoint between them; no stick resize, no wrist turn. Re-based on every change of hands so
// nothing jumps. Street: a grip only grabs when the hand is on the table at the press (Lance);
// anywhere else it stays the game's button for that whole press.
enum class GrabMode
{
  None,
  OneHand,
  TwoHand,
};
GrabMode s_grab_mode = GrabMode::None;
int s_grab_hand = 0;  // OneHand: which hand
Vec3 s_grab_pos0{};   // table position at the (re)base
float s_grab_yaw0 = 0.0f;
float s_grab_size0 = 1.0f;
Vec3 s_grab_hand0{};  // OneHand: that hand at the base
Vec3 s_grab_mid0{};   // TwoHand: midpoint, flat angle and distance of the hands at the base
float s_grab_angle0 = 0.0f;
float s_grab_dist0 = 1.0f;
std::array<bool, 2> s_press_seen{};   // this grip press already decided: table or game button
std::array<bool, 2> s_press_grabs{};
std::atomic<bool> s_right_grab{false};
std::atomic<bool> s_left_grab{false};
bool s_right_hold = false;  // the right grip helped turn the table/screen: held back until let go

// ---- The Jumbotron screen by hand (StreetMR r27, as in Quest 3 Strikers Charged) ----
// One screen per room: [0] MR (your room), [1] VR. Left grip while the left controller points at it
// = grab (moves further than the hand when it's far: gain = distance / 0.6 m, 1..15); the right grip
// joins to turn it about the vertical and resize it. Saved to files/user/street_screens.txt.
struct FreeScreen
{
  bool placed = false;
  Vec3 position{};
  float yaw = 0.0f;     // radians about +Y; the quad faces +Z turned by this
  float width = 2.67f;  // metres
};
std::mutex s_screen_mutex;  // input thread (grab) vs video thread (the quad layer)
std::array<FreeScreen, 2> s_screens;
float s_screen_aspect = 4.0f / 3.0f;
Vec3 s_head_pos{};
float s_head_yaw = 0.0f;
bool s_head_valid = false;
bool s_screens_loaded = false;
bool s_screen_bring = false;
float s_screen_preset_height = 0.0f, s_screen_preset_distance = 0.0f;  // > 0: re-place this size
// Grab (input thread only).
int s_screen_grab = 0;  // bit 0 left hand, bit 1 both
bool s_screen_two_hands = false;
bool s_screen_prev_grip = false;
float s_screen_gain = 1.0f;
Vec3 s_sgrab_pos{}, s_sgrab_hand{};
float s_sgrab_dist = 1.0f, s_sgrab_angle = 0.0f, s_sgrab_width = 1.0f, s_sgrab_yaw = 0.0f;
Vec3 s_sgrab_pos2{}, s_sgrab_mid{};

// ---- Tuning (BaseballMR) ----
// files/user/baseball_tuning.txt ("<key> <value>" per line, # comments) or the dev command
// "tune <key> <value>":
//   upm        game units per metre (Mario Superstar Baseball's unit, measured on the headset)
//   tablesize  metres the field's longest side takes on the table (Strikers' table is ~1.5 m)
//   charscale  players + ball on the table, times their real size (Strikers: 1.5)
//   inset      Tabletop - Stadium: metres you stand inside the field's back edge (behind home plate)
std::atomic<float> s_upm{1.0f};
std::atomic<float> s_table_size{1.6f};
std::atomic<float> s_char_scale{1.5f};
std::atomic<float> s_inset{8.0f};
std::atomic<float> s_texsharp{2.0f};  // mip levels sharper on the table
// StreetMR: the court is the flat piece(s) around the world origin, not the plaza around it.
//   courtradius  field pieces must be centred within this many metres of the origin (0 = any)
//   fieldmax     longest side a field piece may have, metres (Mario Stadium: 250)
//   gripgrab     1 = the grips grab the table (Strikers); 0 = only the held left stick does, and
//                the right grip joins it for two hands (Street: both grips are game buttons)
std::atomic<float> s_court_radius{0.0f};
std::atomic<float> s_field_max{250.0f};
std::atomic<bool> s_grip_grab{true};
std::atomic<float> s_dev_level_head{0.0f};  // dev "tune levelhead <degrees>", see DevLevelHead()
std::atomic<float> s_table_max_h{0.0f};      // "tablemaxh <m>": taller court structure leaves the table
std::atomic<bool> s_flat_mr{true};           // "tune flatmr 0/1", see FlatPassthrough()
std::atomic<u32> s_vr_sky{0};                // "tune vrsky <rgb>", see VRSkyColor()
std::atomic<float> s_refresh{120.0f};        // "tune refresh <Hz>", see DisplayRefreshWanted()
std::atomic<bool> s_mr_eager{true};          // "tune mreager 0/1", see MrEagerHeartbeat()
std::atomic<int> s_table_ir{3};              // "tune tableir <n>", see TableInternalResolution()
std::atomic<bool> s_table_fog{false};        // "tune tablefog 1" = the game's fog on the table too
std::atomic<bool> s_table_shadows{false};    // "tune tableshadows 1" = shadows on the table (r29)
std::atomic<bool> s_table_box{false};        // "tune tablebox 1" = the box under the court (r2-r32)
std::atomic<float> s_hud_deg{34.0f};         // "tune hudsize <deg>": Tabletop 2D panel height
// VR 360 (see IsCourtFloorDraw and the rest in HitzWorld.h).
std::atomic<bool> s_vr360_cap{false};        // dev "tune vr360cap 1"
std::atomic<float> s_cap_yaw{0.0f};          // dev capture pose, degrees
std::atomic<float> s_cap_pitch{0.0f};
std::atomic<float> s_cap_fov{90.0f};
bool s_floor_draw_now = false;               // video thread: this draw is the court floor
bool s_particle_now = false;                 // video thread: this draw is an effect (particles)
bool s_floor_seen_frame = false;             // video thread: the first floor draw of the frame
std::string s_court_candidate;               // video thread: a new key must hold 30 frames
u32 s_court_candidate_frames = 0;
std::string s_court_key_vt;                  // video thread: the last court confirmed
std::mutex s_court_mutex;
std::string s_court_key;                     // the backdrop's key (a court, or a pre-match one)
std::atomic<u32> s_court_gen{0};
std::atomic<bool> s_disc_loading{false};     // HitzDev: the disc is being read fast
std::atomic<bool> s_panorama_ready{false};
std::atomic<bool> s_backdrop_supported{false};
// "tune texlist 1" (video thread only): texture names seen in 2D draws / in 3D outside a match.
std::atomic<bool> s_texlist{false};
std::set<std::string> s_tex_2d, s_tex_menu3d;
bool s_texlist_dirty = false;
u32 s_texlist_frames = 0;
// "tune flatgate 0/1", see FlatGate(). Off by default: an unproven throttle (flat only, the 100 ms
// escape still lets a second release through, and "Missing previous frame submission" also shows
// in Baseball runs that never crashed). One diagnostic at a time.
std::atomic<bool> s_flat_gate{false};
std::atomic<int> s_shadowfix{0};      // see ShadowFix() (StreetMR: off, Baseball's dirt fix)
std::array<u32, 8> s_efb_copy_targets{};
size_t s_efb_copy_next = 0;
std::atomic<bool> s_reset_pending{false};
std::once_flag s_tuning_loaded;
std::atomic<bool> s_force_screen_route{true};  // dev "tune screenroute 0/1"
// Frames since the field was last drawn (a match is on while this stays small).
u32 s_frames_since_court = 1000000;
std::atomic<bool> s_match_on{false};  // r35n: InMatch() for other threads

float Upm()
{
  return std::max(s_upm.load(std::memory_order_relaxed), 1e-4f);
}

void LoadTuning()
{
  std::ifstream in(File::GetUserPath(D_USER_IDX) + "street_tuning.txt");
  for (std::string key; in >> key;)
  {
    if (key[0] == '#')
    {
      std::string rest;
      std::getline(in, rest);
      continue;
    }
    float value = 0.0f;
    if (!(in >> value))
      break;
    SetTuning(key, value);
  }
}

Vec3 MulVec(const Mat3& m, const Vec3& v)
{
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 Mul(const Mat3& a, const Mat3& b)
{
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
      r[i * 3 + j] = a[i * 3 + 0] * b[0 + j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
  }
  return r;
}

Mat3 Transpose(const Mat3& m)
{
  return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

Mat3 RotY(float a)
{
  const float c = std::cos(a), s = std::sin(a);
  return {c, 0, s, 0, 1, 0, -s, 0, c};
}

float Length(const Vec3& v)
{
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

Vec3 Normalize(const Vec3& v)
{
  const float l = Length(v);
  return l > 1e-6f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0, 0, 0};
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float Dot(const Vec3& a, const Vec3& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float WrapAngle(float a)
{
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  return a;
}

// Rows of a 3x4 position matrix -> camera (Gram-Schmidt on the rows). The third row keeps the
// matrix's own handedness (downstream only needs an orthogonal matrix: inverse = transpose), and
// each row's length is kept as the view scale: Mario Superstar Baseball's view matrix squashes
// view Y by ~0.93 (its projection, which VR replaces, undoes it). The translation is stored in
// the rigid (unsquashed) view space: view = scale * (rot * world + trans).
bool ToCamera(const float* m, Camera* out)
{
  Vec3 r0{m[0], m[1], m[2]}, r1{m[4], m[5], m[6]};
  const Vec3 r2_raw{m[8], m[9], m[10]};
  const float s0 = Length(r0);
  if (s0 < 1e-6f)
    return false;
  r0 = Normalize(r0);
  const float d = Dot(r1, r0);
  r1 = Normalize({r1[0] - d * r0[0], r1[1] - d * r0[1], r1[2] - d * r0[2]});
  if (Length(r1) < 0.5f)
    return false;
  Vec3 r2 = Cross(r0, r1);
  if (Dot(r2, r2_raw) < 0.0f)
    r2 = {-r2[0], -r2[1], -r2[2]};
  // Per-axis view scale = each row's length along its own (orthonormal) direction.
  const Vec3 r1_raw{m[4], m[5], m[6]};
  out->scale = {s0, std::max(Dot(r1_raw, r1), 1e-6f), std::max(std::abs(Dot(r2_raw, r2)), 1e-6f)};
  out->rot = {r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], r2[0], r2[1], r2[2]};
  out->trans = {m[3] / out->scale[0], m[7] / out->scale[1], m[11] / out->scale[2]};
  return true;
}

// The game flips view Y in its projection (Mario Superstar Baseball): the shaders mirror view Y
// before the VR projection to keep the game's triangle winding (GeometryShaderManager
// stereoparams.y), so the anchor works in that mirrored view space.
std::atomic<bool> s_flip_y{false};

Camera RigCamera(const Camera& c)
{
  if (!s_flip_y.load(std::memory_order_relaxed))
    return c;
  Camera f = c;
  for (int k = 0; k < 3; ++k)
    f.rot[3 + k] = -f.rot[3 + k];
  f.trans[1] = -f.trans[1];
  return f;
}

// Camera position in world (view units): -R^T t.
Vec3 CameraWorldPos(const Camera& c)
{
  const Vec3 p = MulVec(Transpose(c.rot), c.trans);
  return {-p[0], -p[1], -p[2]};
}

// Rotation angle between two rotations, radians.
float AngleBetween(const Mat3& a, const Mat3& b)
{
  const Mat3 d = Mul(a, Transpose(b));
  const float c = std::clamp((d[0] + d[4] + d[8] - 1.0f) * 0.5f, -1.0f, 1.0f);
  return std::acos(c);
}

// Re-orthonormalise a rotation after extrapolation.
Mat3 Orthonormalize(const Mat3& m)
{
  Camera c;
  const float rows[12] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0};
  return ToCamera(rows, &c) ? c.rot : m;
}

// World up, learned over many frames: the broadcast camera never rolls, so its right vector has
// no component along world up, while it pans the other components around. A second cue breaks
// ties: the camera's motion is mostly horizontal too. Score = share of |right| + share of
// |motion|; lowest = up.
std::array<double, 3> s_right_abs_sum{};
std::array<double, 3> s_up_sum{};
std::array<double, 3> s_motion_abs_sum{};
u32 s_up_frames = 0;

void LearnUp(const Camera& c, const Camera* prev)
{
  for (int i = 0; i < 3; ++i)
  {
    s_right_abs_sum[i] += std::abs(c.rot[i]);
    s_up_sum[i] += c.rot[3 + i];
  }
  if (prev)
  {
    const Vec3 p1 = CameraWorldPos(c), p0 = CameraWorldPos(*prev);
    const float step = Length({p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]});
    if (step < 3.0f * Upm())  // ignore cuts
    {
      for (int i = 0; i < 3; ++i)
        s_motion_abs_sum[i] += std::abs(p1[i] - p0[i]);
    }
  }
  ++s_up_frames;
}

int UpAxis()
{
  if (s_ground_mode && s_field_axis >= 0)
    return s_field_axis;
  const double right_total =
      std::max(s_right_abs_sum[0] + s_right_abs_sum[1] + s_right_abs_sum[2], 1e-6);
  const double motion_total =
      s_motion_abs_sum[0] + s_motion_abs_sum[1] + s_motion_abs_sum[2];
  std::array<double, 3> score{};
  for (int i = 0; i < 3; ++i)
  {
    score[i] = s_right_abs_sum[i] / right_total;
    if (motion_total > 1.0 * Upm())  // a metre of camera travel before trusting it
      score[i] += s_motion_abs_sum[i] / motion_total;
  }
  int axis = 0;
  for (int i = 1; i < 3; ++i)
  {
    // Exact ties: prefer the axis the camera's up leans on most.
    if (score[i] < score[axis] - 1e-3 ||
        (std::abs(score[i] - score[axis]) <= 1e-3 &&
         std::abs(s_up_sum[i]) > std::abs(s_up_sum[axis])))
    {
      axis = i;
    }
  }
  return axis;
}

float UpSign()
{
  if (s_ground_mode && s_field_axis >= 0)
    return s_field_up_sign;
  return s_up_sum[UpAxis()] >= 0 ? 1.0f : -1.0f;
}

// ---- Field outline ("ice" in the Hitz code) ----
// The field is the LOWEST big flat surface drawn with the camera matrix alone. It may come in
// several pieces (infield, outfield, foul ground): every piece at that level is merged.
bool s_have_ice = false;
Vec3 s_ice_min{}, s_ice_max{};
// Bumped whenever the outline is replaced or grows a lot, so views placed with the old one
// re-place themselves (unless Lance moved them).
u32 s_ice_gen = 0;
int s_lower_ice_frames = 0;
int s_field_streak = 0;
bool s_frame_ice = false;
Vec3 s_frame_ice_min{}, s_frame_ice_max{};
float s_frame_ice_level = 0.0f;  // height along the learned up direction (signed)
u32 s_frame_ice_pieces = 0;
// The last field forgotten (menus between games): the same outline again keeps the placements.
bool s_had_ice = false;
Vec3 s_last_ice_min{}, s_last_ice_max{};

// Every flat camera-only surface seen this frame (any size), for the calibration log.
struct FlatSurface
{
  Vec3 lo, hi;
  bool camera_only;
};
std::vector<FlatSurface> s_frame_flats;
u32 s_flat_log_frames = 0;
// Tallest skinned (per-vertex matrix) draw this frame.
float s_frame_tallest_skinned = 0.0f;

// Home plate's side of the field: the broadcast camera films the pitch and the swing from behind
// home plate (and sits between home plate and the field's centre for the pitcher's view too), so
// the camera's average position, seen from the field's centre, points at home plate.
Vec3 s_home_sum{};
float s_home_weight = 0.0f;
bool s_have_home = false;
Vec3 s_home_dir{};  // world, horizontal unit vector: field centre -> home plate side
// Where the at-bat camera films from (its average world position over the same frames): in
// Tabletop - Stadium you stand there at field level, behind home plate and in front of the
// backstop (Mario Stadium: 11 m behind the plate; the field outline's back edge is 26 m behind
// it, among the stands, so "inset from the edge" stood you behind the backstop facing a wall).
Vec3 s_spot_sum{};
float s_spot_weight = 0.0f;

// Players for the Tabletop size-up: skinned parts this frame, clustered at the frame's end into
// players (one foot point each); the next frame scales each part about its player's feet.
struct Part
{
  float a, b;   // horizontal centre (world, the two non-up axes)
  float low;    // lowest point, height above the field
  float high;   // highest point, height above the field
  u32 n;        // vertex count (weight)
};
std::vector<Part> s_frame_parts;
struct Player
{
  float a, b, low, high;
  u32 n;
  int parts;
};
std::vector<Player> s_players;

// StreetMR r28: hoop centres (horizontal), from the nets seen last frame (video thread).
// r33: and the rim's height above the court (the top net ring's).
struct Hoop
{
  float a, b;
  float rim = 0.0f;
};
std::vector<Hoop> s_hoops, s_frame_hoops;

void NoteNet(float a, float b, float top, float u)
{
  for (Hoop& h : s_frame_hoops)
  {
    if (std::hypot(h.a - a, h.b - b) < 1.0f * u)
    {
      h.rim = std::max(h.rim, top);
      return;
    }
  }
  if (s_frame_hoops.size() < 4)
    s_frame_hoops.push_back({a, b, top});
}

// StreetMR r33 (Lance: crowd heads "floating above them"): spectators standing by the court, seen
// last frame - each one's body draw (own matrix, standing, person-sized) with its feet and the top
// of the body. Their heads are separate small draws that look just like the ball.
struct Spectator
{
  float a, b, top;
};
std::vector<Spectator> s_spectators, s_frame_spectators;

const Hoop* NearHoop(float a, float b, float radius)
{
  for (const Hoop& h : s_hoops)
  {
    if (std::hypot(h.a - a, h.b - b) < radius)
      return &h;
  }
  return nullptr;
}

// The current draw's class and size-up (video thread).
DrawClass s_current_draw_class = DrawClass::Rink;
bool s_scale_active = false;
float s_scale_k = 1.0f;
Vec3 s_scale_center{};
u32 s_scaled_draws = 0;  // this frame, for the log

// The table under the mini field: drawn once a frame together with the field floor's own draw
// (same textures and shading, so it matches each stadium).
bool s_plinth_pending = false;     // the draw being classified is the field floor
bool s_plinth_done_frame = false;  // already drawn this frame

// Dev "census": one frame of every perspective draw written to files/user/census.txt.
std::atomic<bool> s_census_request{false};
bool s_census_active = false;
std::string s_census_text;
std::string s_census_material;  // the next draw's textures/blend/depth test (VertexManagerBase)
u32 s_census_draws = 0;

bool Finite(const Vec3& v)
{
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float FlatArea(const Vec3& lo, const Vec3& hi, int a, int b)
{
  return (hi[a] - lo[a]) * (hi[b] - lo[b]);
}

Vec3 IceCenter()
{
  return {0.5f * (s_ice_min[0] + s_ice_max[0]), 0.5f * (s_ice_min[1] + s_ice_max[1]),
          0.5f * (s_ice_min[2] + s_ice_max[2])};
}

float IceLevel()
{
  const int up = UpAxis();
  return 0.5f * (s_ice_min[up] + s_ice_max[up]);
}

// The field frame: up = the learned world up; forward = from home plate out to centre field, so
// home plate is nearest you on the table and you look out over the field like the TV camera.
// Snapped to the world axes/diagonals when within 12 degrees. Before home plate is known,
// forward = the TV camera's forward flattened and snapped to the nearer horizontal axis. Rows
// are the field axes in world coordinates.
Mat3 RinkFrame(const Camera& c)
{
  const int axis = UpAxis();
  Vec3 up{0, 0, 0};
  up[axis] = UpSign();
  const int a = (axis + 1) % 3, b = (axis + 2) % 3;
  Vec3 fwd{0, 0, 0};
  if (s_have_home)
  {
    fwd = {-s_home_dir[0], -s_home_dir[1], -s_home_dir[2]};
    float best_dot = -2.0f;
    Vec3 best{};
    for (int i = 0; i < 8; ++i)
    {
      const float ang = static_cast<float>(i) * 0.78539816f;
      Vec3 d{0, 0, 0};
      d[a] = std::cos(ang);
      d[b] = std::sin(ang);
      const float dd = Dot(d, fwd);
      if (dd > best_dot)
      {
        best_dot = dd;
        best = d;
      }
    }
    if (best_dot > 0.978f)
      fwd = best;
  }
  else
  {
    const Vec3 cam_fwd{-c.rot[6], -c.rot[7], -c.rot[8]};
    const float d = Dot(cam_fwd, up);
    const Vec3 flat =
        Normalize({cam_fwd[0] - d * up[0], cam_fwd[1] - d * up[1], cam_fwd[2] - d * up[2]});
    if (Length(flat) > 0.5f)
    {
      const int k = std::abs(flat[a]) >= std::abs(flat[b]) ? a : b;
      fwd[k] = flat[k] > 0 ? 1.0f : -1.0f;
    }
    else
    {
      fwd[a] = 1.0f;  // camera straight down: any horizontal axis
    }
  }
  Vec3 right = Cross(fwd, up);
  // BaseballMR: Mario Superstar Baseball's camera is a rotation but its projection flips Y, so
  // its TV picture is a mirror image of its own world coordinates (third base at +X, yet on the
  // left of the screen). The rig camera then is a mirror too, and so must be this frame: with
  // only one of them mirrored, the room shows the field mirrored (Toad at second base on the
  // wrong side) and every triangle's winding flips, so the game's back-face culling removed the
  // field floor, the sea and the sky dome (one-sided surfaces): the "pool of green" Lance saw was
  // the inside of the table box under an invisible field.
  const Mat3 rig = RigCamera(c).rot;
  const float rig_det = rig[0] * (rig[4] * rig[8] - rig[5] * rig[7]) -
                        rig[1] * (rig[3] * rig[8] - rig[5] * rig[6]) +
                        rig[2] * (rig[3] * rig[7] - rig[4] * rig[6]);
  if (rig_det < 0.0f)
    right = {-right[0], -right[1], -right[2]};
  // room.x = right, room.y = up, room.z = -forward
  return {right[0], right[1], right[2], up[0], up[1], up[2], -fwd[0], -fwd[1], -fwd[2]};
}

// The field's half extents across (right) and along (forward) a field frame, game units.
void FieldHalfExtents(const Mat3& frame, float* half_width, float* half_depth)
{
  float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
  for (int c = 0; c < 8; ++c)
  {
    const Vec3 p{(c & 1) ? s_ice_max[0] : s_ice_min[0], (c & 2) ? s_ice_max[1] : s_ice_min[1],
                 (c & 4) ? s_ice_max[2] : s_ice_min[2]};
    const float x = frame[0] * p[0] + frame[1] * p[1] + frame[2] * p[2];
    const float z = frame[6] * p[0] + frame[7] * p[1] + frame[8] * p[2];
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    z0 = std::min(z0, z);
    z1 = std::max(z1, z);
  }
  *half_width = 0.5f * (x1 - x0);
  *half_depth = 0.5f * (z1 - z0);
}

void ForgetField()
{
  if (s_have_ice)
  {
    s_had_ice = true;
    s_last_ice_min = s_ice_min;
    s_last_ice_max = s_ice_max;
  }
  s_have_ice = false;
  s_field_streak = 0;
  s_lower_ice_frames = 0;
  s_hoops.clear();  // StreetMR r32: hoops are sticky now; a new court learns its own
  s_spectators.clear();
}

bool SameOutline(const Vec3& lo0, const Vec3& hi0, const Vec3& lo1, const Vec3& hi1, float tol)
{
  for (int k = 0; k < 3; ++k)
  {
    if (std::abs(lo0[k] - lo1[k]) > tol || std::abs(hi0[k] - hi1[k]) > tol)
      return false;
  }
  return true;
}
}  // namespace

View GetView()
{
  return static_cast<View>(s_view.load(std::memory_order_relaxed));
}

void SetView(View view)
{
  s_view.store(static_cast<int>(view), std::memory_order_relaxed);
}

const char* ViewName(View view)
{
  switch (view)
  {
  case View::Tabletop:
    return "Tabletop";
  case View::Rinkside:
    return "Stadium";
  default:
    return "Jumbotron";
  }
}

void OnDraw(const float* rows3x4, u32 weight)
{
  for (Candidate& c : s_candidates)
  {
    if (std::memcmp(c.m, rows3x4, sizeof(c.m)) == 0)
    {
      c.weight += weight;
      return;
    }
  }
  if (s_candidates.size() < 96)
  {
    Candidate c;
    std::memcpy(c.m, rows3x4, sizeof(c.m));
    c.weight = weight;
    s_candidates.push_back(c);
  }
}

namespace
{
std::string s_census_line;  // the draw being classified (census only)

// Scale the current draw by k about its own centre `c`, drawn with that centre at `c_new`
// (world coordinates); stored as one scale about a point in the anchor's (rigid, mirrored if the
// game flips Y) view space.
void SetScale(float k, const Vec3& c, const Vec3& c_new)
{
  if (k < 1.01f || k > 4.0f)
    return;
  // v' = k (v - c) + c_new = k (v - P) + P  with  P = (c_new - k c) / (1 - k)
  Vec3 p{};
  for (int i = 0; i < 3; ++i)
    p[i] = (c_new[i] - k * c[i]) / (1.0f - k);
  const Camera cam = RigCamera(CurrentCamera());
  const Vec3 v = MulVec(cam.rot, p);
  s_scale_center = {v[0] + cam.trans[0], v[1] + cam.trans[1], v[2] + cam.trans[2]};
  s_scale_k = k;
  s_scale_active = true;
  s_scaled_draws++;
}

// Nearest player (last frame) to a horizontal point, within `radius`. A player = at least two
// parts together, 0.6-4 m tall (a lone part is a base, a sign, the ball in flight).
const Player* NearestPlayer(float a, float b, float radius)
{
  const Player* best = nullptr;
  float best_d = radius;
  const float u = Upm();
  for (const Player& p : s_players)
  {
    if (p.parts < 2 || p.high - p.low < 0.6f * u || p.high - p.low > 4.0f * u)
      continue;
    const float d = std::hypot(p.a - a, p.b - b);
    if (d < best_d)
    {
      best_d = d;
      best = &p;
    }
  }
  return best;
}

// StreetMR r31 (Lance: "ball tracing looks crazy, itll disappear and reappear"): where the ball
// sits sideways once the players are bigger. Each player within `reach` pulls it along with his
// own size-up, (k - 1) * (ball - player), fully within a metre and fading out to `reach`; with
// two players at it (a steal, a block, a pass caught) the pulls are averaged instead of jumping
// to whichever was nearest this frame.
// StreetMR r33 (Lance: "the ball dont even go in the hoop, it looks like airballs half the time
// but it goes in"): the point a player grows about, sideways. His feet - except up at a rim: a
// player reaching rim height near a net (a dunk, a layup, a block at the rim) grows about the floor
// under that net, as the hoop does, so his hands, the ball and the rim meet. Blended in over the
// last metre of distance and the last half metre of height, so nothing snaps.
std::pair<float, float> PlayerAnchor(const Player& p)
{
  const float u = Upm();
  float best = 0.0f;
  const Hoop* at = nullptr;
  for (const Hoop& h : s_hoops)
  {
    const float rim = h.rim > 0.0f ? h.rim : 3.05f * u;
    const float d = std::hypot(h.a - p.a, h.b - p.b);
    const float w = std::clamp((2.0f * u - d) / (1.0f * u), 0.0f, 1.0f) *
                    std::clamp((p.high - (rim - 0.6f * u)) / (0.5f * u), 0.0f, 1.0f);
    if (w > best)
    {
      best = w;
      at = &h;
    }
  }
  if (at == nullptr)
    return {p.a, p.b};
  return {p.a + best * (at->a - p.a), p.b + best * (at->b - p.b)};
}

std::pair<float, float> BallReachOffset(float a, float b, float h, float k, float reach)
{
  const float u = Upm();
  float da = 0.0f, db = 0.0f, total = 0.0f;
  for (const Player& p : s_players)
  {
    if (p.parts < 2 || p.high - p.low < 0.6f * u || p.high - p.low > 4.0f * u)
      continue;
    const float d = std::hypot(p.a - a, p.b - b);
    const float w = std::clamp((reach - d) / (reach - 1.0f * u), 0.0f, 1.0f);
    if (w <= 0.0f)
      continue;
    const auto [pa, pb] = PlayerAnchor(p);
    da += w * (k - 1.0f) * (a - pa);
    db += w * (k - 1.0f) * (b - pb);
    total += w;
  }
  if (total > 1.0f)
  {
    da /= total;
    db /= total;
  }
  // r33: at the rim the ball belongs to the hoop - it grows about the floor under the net like
  // the rim, so a make goes through the rim (before, players under the basket pulled it a foot
  // off to the side: makes looked like airballs). Blended in over the last 0.7 m and the metre
  // below the rim.
  for (const Hoop& hp : s_hoops)
  {
    const float rim = hp.rim > 0.0f ? hp.rim : 3.05f * u;
    const float d = std::hypot(hp.a - a, hp.b - b);
    const float w = std::clamp((1.5f * u - d) / (0.7f * u), 0.0f, 1.0f) *
                    std::clamp((h - (rim - 1.0f * u)) / (0.5f * u), 0.0f, 1.0f);
    if (w <= 0.0f)
      continue;
    da = (1.0f - w) * da + w * (k - 1.0f) * (a - hp.a);
    db = (1.0f - w) * db + w * (k - 1.0f) * (b - hp.b);
    break;
  }
  return {da, db};
}

// A perfectly flat, field-sized piece in its own model space (game units are metres): 20-250
// long, at least 8 wide, flat within 1%. Returns its flat axis, or -1.
int GroundPieceAxis(const Vec3& lo, const Vec3& hi)
{
  const float ex[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  int thin = 0;
  for (int k = 1; k < 3; ++k)
  {
    if (ex[k] < ex[thin])
      thin = k;
  }
  const float ea = ex[(thin + 1) % 3], eb = ex[(thin + 2) % 3];
  const float big = std::max(ea, eb), small = std::min(ea, eb);
  if (!(ex[thin] <= 0.01f * big) || big < 20.0f || big > 250.0f || small < 8.0f)
    return -1;
  return thin;
}

// Each field piece votes for its matrix (the ground vote).
void GroundVote(const float* m, const Vec3& lo, const Vec3& hi)
{
  const int thin = GroundPieceAxis(lo, hi);
  if (thin < 0)
    return;
  const float ea = hi[(thin + 1) % 3] - lo[(thin + 1) % 3];
  const float eb = hi[(thin + 2) % 3] - lo[(thin + 2) % 3];
  for (GroundCandidate& g : s_ground)
  {
    if (std::memcmp(g.m, m, sizeof(g.m)) == 0)
    {
      g.area += static_cast<double>(ea) * eb;
      g.level = std::min(g.level, lo[thin]);
      return;
    }
  }
  if (s_ground.size() < 32)
  {
    GroundCandidate g;
    std::memcpy(g.m, m, sizeof(g.m));
    g.area = static_cast<double>(ea) * eb;
    g.thin_axis = thin;
    g.level = lo[thin];
    s_ground.push_back(g);
  }
}

DrawClass ClassifyDrawImpl(const u8* vertices, u32 count, u32 stride, int pos_offset,
                           int components, const float* single_matrix, const float* matrix_table,
                           int posmtx_offset, bool arena_life_size, bool blended)
{
  if (count == 0 || components < 2 || (!single_matrix && (!matrix_table || posmtx_offset < 0)))
    return DrawClass::Rink;

  // Model-space bounds first (single matrix): the ground vote needs them before any camera exists.
  // Sampled (at most ~128 vertices plus the last; Mario Superstar Baseball has ~740 draws a frame
  // and the r1 build ran at 75%); a draw that looks like a flat field piece gets an exact scan so
  // the field outline is exact.
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  const auto grow = [](Vec3& mn, Vec3& mx, const float* p) {
    for (int k = 0; k < 3; ++k)
    {
      mn[k] = std::min(mn[k], p[k]);
      mx[k] = std::max(mx[k], p[k]);
    }
  };
  const auto model_pos = [&](u32 i, float* p) {
    p[0] = p[1] = p[2] = 0.0f;
    std::memcpy(p, vertices + i * stride + pos_offset, sizeof(float) * std::min(components, 3));
  };
  const u32 step = std::max<u32>(1, count / 128);
  if (single_matrix)
  {
    float p[3];
    for (u32 i = 0; i < count; i += step)
    {
      model_pos(i, p);
      grow(lo, hi, p);
    }
    model_pos(count - 1, p);
    grow(lo, hi, p);
    if (!Finite(lo) || !Finite(hi))
      return DrawClass::Rink;
    const float ex0 = hi[0] - lo[0], ex1 = hi[1] - lo[1], ex2 = hi[2] - lo[2];
    const float thin = std::min({ex0, ex1, ex2}), big = std::max({ex0, ex1, ex2});
    if (step > 1 && big >= 15.0f && thin <= 0.01f * big && count <= 16384)
    {
      for (u32 i = 0; i < count; ++i)
      {
        model_pos(i, p);
        grow(lo, hi, p);
      }
    }
    // Only during a match: the menus' 3D backdrops are big flat planes too (a 374 x 283 wall at
    // z -511 took the camera in Lance's r9 run).
    if (InMatchScene())
      GroundVote(single_matrix, lo, hi);

    // This frame's exact camera: the first field piece whose matrix is close to the prediction
    // (a flat stand tier with its own matrix is far from it).
    if (InMatchScene() && s_ground_mode && s_have_camera && !s_exact_this_frame &&
        GroundPieceAxis(lo, hi) >= 0)
    {
      Camera c;
      if (ToCamera(single_matrix, &c))
      {
        const Vec3 p0 = CameraWorldPos(c), p1 = CameraWorldPos(s_cam_next);
        if (AngleBetween(c.rot, s_cam_next.rot) < 0.2f &&
            Length({p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2]}) < 8.0f * Upm())
        {
          s_cam_exact = c;
          s_exact_this_frame = true;
          s_exact_frames++;
        }
      }
    }
  }

  if (!s_have_camera || s_up_frames < 30)
    return DrawClass::Rink;

  const Camera& cam = CurrentCamera();
  const Mat3 rt = Transpose(cam.rot);
  // world = R^T (view / scale - trans): undo the game's view squash, then the rigid camera.
  const auto to_world = [&](const float* m, const float* p) {
    const Vec3 v{(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]) / cam.scale[0] - cam.trans[0],
                 (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]) / cam.scale[1] - cam.trans[1],
                 (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]) / cam.scale[2] - cam.trans[2]};
    return MulVec(rt, v);
  };

  // World bounds (via the predicted camera): skinned draws vertex by vertex (sampled), each
  // through its own matrix row; single-matrix draws from their 8 model-space corners below.
  Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
  if (!single_matrix)
  {
    const auto add_vertex = [&](u32 i) {
      float p[3];
      model_pos(i, p);
      // Per-vertex matrix (skinned players, crowd sprites): row index into the matrix table.
      const u32 row = std::min<u32>(vertices[i * stride + posmtx_offset], 61);
      const Vec3 w = to_world(matrix_table + row * 4, p);
      grow(wlo, whi, w.data());
    };
    for (u32 i = 0; i < count; i += step)
      add_vertex(i);
    add_vertex(count - 1);
  }

  const float* m = single_matrix;
  if (m)
  {
    // World bounds of the 8 model-space corners.
    for (int c = 0; c < 8; ++c)
    {
      const float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1],
                          (c & 4) ? hi[2] : lo[2]};
      const Vec3 w = to_world(m, p);
      grow(wlo, whi, w.data());
    }
  }

  const int up = UpAxis();
  const int a = (up + 1) % 3, b = (up + 2) % 3;
  const float u = Upm();
  if (!Finite(wlo) || !Finite(whi))
    return DrawClass::Rink;
  const float sign = UpSign();
  const float field_level = s_have_ice ? IceLevel() : 0.0f;
  // Heights above the field (positive up).
  const float h0 = sign * (wlo[up] - field_level), h1 = sign * (whi[up] - field_level);
  const float hmin = std::min(h0, h1), hmax = std::max(h0, h1);
  const float wca = 0.5f * (wlo[a] + whi[a]), wcb = 0.5f * (wlo[b] + whi[b]);

  // Draws with the camera's own matrix (the field and everything modelled with it): their model
  // coordinates ARE world coordinates, so the field is learned from those raw bounds (exact).
  // Same rotation, same scale (characters are scaled 1.8) and same translation; against this
  // frame's exact camera the match is exact, against the prediction it tolerates a little motion.
  bool camera_only = false;
  if (m)
  {
    float rot_match = 0.0f;
    bool unit = true;
    for (int r = 0; r < 3; ++r)
    {
      const Vec3 row{m[r * 4 + 0] / cam.scale[r], m[r * 4 + 1] / cam.scale[r],
                     m[r * 4 + 2] / cam.scale[r]};
      rot_match += Dot(row, {cam.rot[r * 3 + 0], cam.rot[r * 3 + 1], cam.rot[r * 3 + 2]});
      const float len = Length(row);
      unit = unit && len > 0.98f && len < 1.02f;
    }
    const Vec3 dt{m[3] / cam.scale[0] - cam.trans[0], m[7] / cam.scale[1] - cam.trans[1],
                  m[11] / cam.scale[2] - cam.trans[2]};
    const float tol = (s_exact_this_frame ? 0.01f : 0.05f) * (1.0f + Length(cam.trans));
    camera_only = unit && rot_match > 2.99f && Length(dt) < tol;
  }
  const Vec3& ilo = camera_only ? lo : wlo;
  const Vec3& ihi = camera_only ? hi : whi;
  if (s_census_active)
  {
    s_census_line = fmt::format(
        "{} n={} blend={} camonly={} world ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
        m ? "single" : "skinned", count, blended ? 1 : 0, camera_only ? 1 : 0, wlo[0], wlo[1],
        wlo[2], whi[0], whi[1], whi[2]);
    if (m)
    {
      s_census_line += fmt::format(" model ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) t ({:.1f},{:.1f},{:.1f})",
                                   lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], m[3], m[7], m[11]);
      s_census_line += fmt::format(" m [{:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f}]",
                                   m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    }
  }
  // Calibration log: every flat single-matrix surface (thinner than 5% of its size), any size.
  const float ext_a = ihi[a] - ilo[a], ext_b = ihi[b] - ilo[b];
  const float ext_up = ihi[up] - ilo[up];
  if (m && Finite(ilo) && Finite(ihi) && ext_up < 0.05f * std::max(ext_a, ext_b) && ext_a > 0 &&
      ext_b > 0 && s_frame_flats.size() < 64)
  {
    s_frame_flats.push_back({ilo, ihi, camera_only});
  }

  // Field pieces: camera-only, flat (under half a metre, or 2% of its size: the mound and the
  // crown of the outfield), at least 10 m long and 2 m wide, at most 250 m (Mario Stadium's sea
  // and beach are 330-390 m pieces with the field's own matrix, a metre below the grass). The
  // lowest level wins (stand tiers and roofs are higher); every piece at that level joins the
  // outline.
  const float max_side = std::max(ext_a, ext_b), min_side = std::min(ext_a, ext_b);
  bool field_piece = false;
  float piece_level = 0.0f;
  const float court_radius = s_court_radius.load(std::memory_order_relaxed) * u;
  const bool near_origin =
      court_radius <= 0.0f || (std::abs(0.5f * (ilo[a] + ihi[a])) <= court_radius &&
                               std::abs(0.5f * (ilo[b] + ihi[b])) <= court_radius);
  if (InMatchScene() && m && camera_only && ext_up < std::max(0.5f * u, 0.02f * max_side) &&
      max_side >= 10.0f * u && min_side >= 2.0f * u &&
      max_side <= s_field_max.load(std::memory_order_relaxed) * u && near_origin)
  {
    field_piece = true;
    piece_level = sign * 0.5f * (ilo[up] + ihi[up]);
    if (!s_frame_ice || piece_level < s_frame_ice_level - 0.6f * u)
    {
      s_frame_ice_min = ilo;
      s_frame_ice_max = ihi;
      s_frame_ice_level = piece_level;
      s_frame_ice_pieces = 1;
      s_frame_ice = true;
    }
    else if (piece_level <= s_frame_ice_level + 0.6f * u)
    {
      for (int k = 0; k < 3; ++k)
      {
        s_frame_ice_min[k] = std::min(s_frame_ice_min[k], ilo[k]);
        s_frame_ice_max[k] = std::max(s_frame_ice_max[k], ihi[k]);
      }
      s_frame_ice_pieces++;
    }
  }

  // Skinned parts on or just above the field (players run, dive, climb the wall): collected for
  // the players' foot points.
  const bool inside_field_xy = s_have_ice && wca > s_ice_min[a] - 3.0f * u &&
                               wca < s_ice_max[a] + 3.0f * u && wcb > s_ice_min[b] - 3.0f * u &&
                               wcb < s_ice_max[b] + 3.0f * u;
  // Moving parts: skinned draws, or small draws with a matrix of their own (Mario Superstar
  // Baseball draws each body part, the bat and the ball with its own matrix).
  if (!m)
    s_frame_tallest_skinned = std::max(s_frame_tallest_skinned, whi[up] - wlo[up]);
  // StreetMR r28: the hoops, learned from their nets (small camera-only rings at rim height; the
  // court's own model, so they never move). The hoop's skinned parts (rim, backboard) are then
  // told from a player's by where they hang: right at a known net, 2.5-5 m up. Before r28 the rim
  // and backboard joined the players' parts and became a "player" of their own at the hoop.
  if (m && camera_only && inside_field_xy && hmin > 2.5f * u && hmax < 4.0f * u &&
      std::max({whi[a] - wlo[a], whi[b] - wlo[b], whi[up] - wlo[up]}) < 0.8f * u)
  {
    NoteNet(wca, wcb, hmax, u);
  }
  const float wide_ab = std::max(whi[a] - wlo[a], whi[b] - wlo[b]);
  const bool hoop_height = hmin > 2.5f * u && hmax < 5.0f * u && wide_ab < 3.0f * u;
  const Hoop* at_net = NearHoop(wca, wcb, 1.0f * u);
  const bool hoop_part = hoop_height && (at_net || (s_hoops.empty() && hmin > 2.9f * u));
  // StreetMR r32 (Lance: "the pole on the hoop moving"): a hoop's pole or post - tall and thin,
  // standing up from the floor or hanging from above, near a known net - belongs to the hoop, never
  // to a player passing under it (no player part is 2 m tall). Before, a skinned pole joined the
  // nearest player and grew about his feet, so it slid whenever someone came near the basket.
  const bool hoop_post = !hoop_part && wide_ab < 1.5f * u && hmax - hmin > 2.0f * u &&
                         (hmin < 0.5f * u || hmax > 3.0f * u) && NearHoop(wca, wcb, 3.0f * u);
  // StreetMR r29: a player's shadow (skinned, squashed flat) is drawn pulled toward the TV camera
  // (NBA Street's way of keeping it above the court in the depth buffer), so it lies flat about a
  // metre up - right from the TV, wrong anywhere else. Not a player's part.
  const bool floating_flat = !m && whi[up] - wlo[up] < 0.05f * u && hmin > 0.2f * u;
  // StreetMR r32: the same for a shadow with a matrix of its own (the ball's): a solid model in
  // its own space, squashed flat by its matrix.
  const bool flattened_shadow =
      m && !camera_only && whi[up] - wlo[up] < 0.03f * u &&
      std::min({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) >
          0.2f * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  // StreetMR r31: nor is the ball (it joined whichever player held it and shifted his centre) or
  // a spectator beside the court (own-matrix figures there became "players" the ball sized up
  // about).
  const float e_max = std::max({wide_ab, whi[up] - wlo[up]});
  const float e_min = std::min({whi[a] - wlo[a], whi[b] - wlo[b], whi[up] - wlo[up]});
  const bool beside_court = m && s_have_ice &&
                            (wca < s_ice_min[a] || wca > s_ice_max[a] || wcb < s_ice_min[b] ||
                             wcb > s_ice_max[b]);
  // StreetMR r33: a spectator's body (own matrix, standing on the ground, person-sized and narrow;
  // the players are skinned) and its head (a small round draw just on top of a body seen last
  // frame - it looks just like the ball, which grew and rose while the body didn't: heads floated).
  const bool spectator_body = m && !camera_only && hmin < 0.3f * u &&
                              whi[up] - wlo[up] > 1.0f * u && whi[up] - wlo[up] < 2.4f * u &&
                              wide_ab < 1.2f * u;
  if (spectator_body && s_frame_spectators.size() < 96)
    s_frame_spectators.push_back({wca, wcb, hmax});
  const Spectator* head_of = nullptr;
  if (m && !camera_only && e_max < 1.0f * u)
  {
    for (const Spectator& sp : s_spectators)
    {
      if (std::hypot(sp.a - wca, sp.b - wcb) < 0.5f * u && hmin > sp.top - 0.8f * u &&
          hmin < sp.top + 0.3f * u)
      {
        head_of = &sp;
        break;
      }
    }
  }
  const bool ball_like =
      m && !camera_only && e_max < 1.0f * u && e_min > 0.25f * e_max && head_of == nullptr;
  if ((!m || !camera_only) && inside_field_xy && !hoop_part && !hoop_post && !floating_flat &&
      !flattened_shadow && !ball_like && !beside_court && !spectator_body && head_of == nullptr &&
      hmin > -1.0f * u &&
      hmin < 6.0f * u && hmax - hmin < 4.0f * u &&
      std::max(whi[a] - wlo[a], whi[b] - wlo[b]) < 3.0f * u && s_frame_parts.size() < 256)
  {
    s_frame_parts.push_back({wca, wcb, hmin, hmax, count});
  }

  // The field floor's own draw (the biggest piece at the settled level, at least 40% of the
  // outline): carries the table in Tabletop; in every view its texture tells the courts apart
  // (StreetMR VR 360).
  const bool court_floor = field_piece && s_have_ice &&
                           std::abs(piece_level - sign * field_level) <= 0.6f * u &&
                           FlatArea(ilo, ihi, a, b) >= 0.4f * FlatArea(s_ice_min, s_ice_max, a, b);
  if (court_floor && !s_floor_seen_frame)
  {
    s_floor_draw_now = true;
    s_floor_seen_frame = true;
  }

  if (GetView() != View::Tabletop)
    return DrawClass::Rink;

  if (court_floor && !s_plinth_done_frame)
    s_plinth_pending = true;

  // Field = everything inside the field outline (3 m slack: the walls stand on its edge) and
  // under 60 m (pop-ups fly high). Surroundings = anything centred outside the field (stands,
  // crowd, scoreboard, the town around the park) or big and high above it (roofs, lights).
  const DrawClass outside = arena_life_size ? DrawClass::Arena : DrawClass::Skip;
  if (hmin > 60.0f * u)
    return outside;
  if (!s_have_ice)
    return DrawClass::Rink;

  bool inside = hmax <= 60.0f * u;
  bool centre_outside = false;
  for (int k : {a, b})
  {
    if (wlo[k] < s_ice_min[k] - 3.0f * u || whi[k] > s_ice_max[k] + 3.0f * u)
      inside = false;
    const float centre = 0.5f * (wlo[k] + whi[k]);
    if (centre < s_ice_min[k] - 2.0f * u || centre > s_ice_max[k] + 2.0f * u)
      centre_outside = true;
    // Much bigger than the field (Mario Stadium's sea and beach, 330 m pieces centred on it):
    // surroundings, whatever its centre.
    if (whi[k] - wlo[k] > 1.3f * (s_ice_max[k] - s_ice_min[k]) + 6.0f * u)
      centre_outside = true;
  }

  const float k_char = s_char_scale.load(std::memory_order_relaxed);
  const float wext_a = whi[a] - wlo[a], wext_b = whi[b] - wlo[b], wext_up = whi[up] - wlo[up];
  const float wmax = std::max({wext_a, wext_b, wext_up});
  const float wmin = std::min({wext_a, wext_b, wext_up});

  // StreetMR r33 (Lance, Tabletop: the tree and building shadows "keep them gone"; the black
  // squares floating around Broad Street, the tree shapes hanging over Rec Center): NBA Street
  // lifts every flat thing it lays on the court - the players', the ball's, the trees', the
  // buildings' and the props' shadows - toward its TV camera. On the table they hang in the air.
  // Anything lying level above the court (not the nets' rings) is off the table.
  if (wext_up < 0.05f * u && hmin > 0.2f * u && !hoop_part &&
      !s_table_shadows.load(std::memory_order_relaxed))
  {
    return DrawClass::Skip;
  }

  // r33: spectators grow with their own body, head and all, about the floor under them. Those
  // standing beyond the court's edge stay off the table, heads too (r31).
  if ((spectator_body || head_of != nullptr) && beside_court)
    return outside;
  if (k_char > 1.01f && (spectator_body || head_of != nullptr) && !centre_outside)
  {
    Vec3 foot{};
    foot[a] = head_of ? head_of->a : wca;
    foot[b] = head_of ? head_of->b : wcb;
    foot[up] = field_level;
    SetScale(k_char, foot, foot);
    return DrawClass::Rink;
  }

  // The ball (small, compact, moving with its own matrix): stays with the game on the table even
  // when a home run sails out of the park. Drawn bigger like the players; near a player it moves
  // with that player's size-up (in a hand, in a glove, off the bat) so they still meet.
  if (ball_like && wmax > 0.02f * u && (!centre_outside || hmin > 2.0f * u))
  {
    const Vec3 c{0.5f * (wlo[0] + whi[0]), 0.5f * (wlo[1] + whi[1]), 0.5f * (wlo[2] + whi[2])};
    Vec3 c_new = c;
    // StreetMR r27 (Lance: "the ball doesnt register"): its height always grows with the players
    // and the hoops (k times as high above the court), in flight too, so a shot meets the rim;
    // sideways it only follows a nearby player's bigger reach. Before, a ball in flight dropped
    // back to its real height while the rim it was aiming for had been made bigger.
    c_new[up] = field_level + k_char * (c[up] - field_level);
    const auto [da, db] =
        BallReachOffset(wca, wcb, 0.5f * (hmin + hmax), k_char, 2.5f * u);
    c_new[a] += da;
    c_new[b] += db;
    SetScale(k_char, c, c_new);
    return DrawClass::Rink;
  }

  // Effects (sparks, dust, star trails, the game's particles): a batch of many small pieces spread
  // over metres, not flat on the ground (shadows, paint). Over the field they're drawn on the
  // table AND life-size around you; beyond it, life-size only. In the room (MR) as well as in the
  // stadium (VR). Particles are separate little quads (or triangles) scattered around: the gaps
  // inside each piece are tiny, the jumps from one piece to the next are much bigger. A solid
  // mesh (a wall, a player) has short gaps all the way through.
  (void)blended;
  // Spread sideways too: a foul pole (20 m of thin triangles, 0.4 m across) passed the gap test
  // and stood life-size in Lance's room (r16 census).
  if (m && count >= 12 && count <= 4096 && std::max({wext_a, wext_b, wext_up}) > 2.0f * u &&
      wext_up > 0.3f * u && std::max(wext_a, wext_b) > 1.0f * u)
  {
    const auto vertex_world = [&](u32 i) {
      float p[3] = {0, 0, 0};
      std::memcpy(p, vertices + i * stride + pos_offset, sizeof(float) * std::min(components, 3));
      return to_world(m, p);
    };
    const auto gap = [&](u32 i) {
      const Vec3 w0 = vertex_world(i), w1 = vertex_world(i + 1);
      return Length({w1[0] - w0[0], w1[1] - w0[1], w1[2] - w0[2]});
    };
    for (const u32 period : {4u, 3u})
    {
      if (count % period != 0)
        continue;
      std::array<float, 32> inside_gaps{}, between{};
      u32 ni = 0, nb = 0;
      const u32 pieces = count / period;
      const u32 piece_step = std::max<u32>(1, pieces / 32);
      for (u32 p = 0; p + 1 < pieces && (ni < inside_gaps.size() || nb < between.size());
           p += piece_step)
      {
        const u32 first = p * period;
        if (ni < inside_gaps.size())
          inside_gaps[ni++] = gap(first);
        if (nb < between.size())
          between[nb++] = gap(first + period - 1);
      }
      if (ni < 3 || nb < 3)
        continue;
      std::nth_element(inside_gaps.begin(), inside_gaps.begin() + ni / 2, inside_gaps.begin() + ni);
      std::nth_element(between.begin(), between.begin() + nb / 2, between.begin() + nb);
      const float piece = inside_gaps[ni / 2], jump = between[nb / 2];
      if (piece < 0.6f * u && jump > 4.0f * piece && jump > 0.2f * u)
      {
        s_particle_now = true;  // VR 360 capture: no effects frozen in the picture
        // High up (floodlight glows, sky effects): the stadium's, never the table's. Over the
        // field: on the table, and in the VR stadium life-size around you as well (in the room,
        // MR, only on the table).
        // StreetMR VR 360: Tabletop VR is the table under the court's calm sky (no court around
        // you, Lance), but the game's effects over the court - falling leaves, weather - still
        // fall around you life-size.
        if (hmin > 6.0f * u || centre_outside)
          return outside;
        const bool effects_life_size =
            arena_life_size || (HitzModes::StadiumWanted() && GetView() == View::Tabletop);
        return effects_life_size ? DrawClass::Both : DrawClass::Rink;
      }
    }
  }

  // StreetMR r27 (Lance: "the hoop be moving"): the hoops grow with the players, as one piece.
  // Before, the backboard and rim (skinned) took the players' size-up about whichever player was
  // nearest while the net (the court's own model) stayed real-size, so the hoop came apart and
  // jumped around. Now everything hanging 2.9-4.6 m up over the court that isn't wide (backboard,
  // rim, net pieces; not the cage) is k times bigger about its own centre and k times as high,
  // and a free-standing hoop's pole k times bigger about its foot - the rim then sits where the
  // bigger players dunk and the ball (above) flies.
  // r28: only at a net learned from the court (falls back to the height band while none is known),
  // so a dunker's hands and head at the rim stay with his body; and every piece of a known hoop
  // (rim, backboard, net, pole) grows about the one point on the floor under its net, so the hoop
  // stays in one piece.
  if (k_char > 1.01f && !centre_outside)
  {
    const float wide_h = wide_ab;
    const bool pole = (m && camera_only && hmin < 0.5f * u && hmax > 2.9f * u &&
                       hmax < 5.0f * u && wide_h < 1.2f * u) ||
                      hoop_post;
    const Hoop* hoop = at_net ? at_net : (pole ? NearHoop(wca, wcb, 3.0f * u) : nullptr);
    if (hoop && (hoop_part || pole))
    {
      Vec3 pivot{};
      pivot[a] = hoop->a;
      pivot[b] = hoop->b;
      pivot[up] = field_level;
      SetScale(k_char, pivot, pivot);
      return DrawClass::Rink;
    }
    if (hoop_part)
    {
      const Vec3 c{0.5f * (wlo[0] + whi[0]), 0.5f * (wlo[1] + whi[1]), 0.5f * (wlo[2] + whi[2])};
      Vec3 c_new = c;
      c_new[up] = field_level + k_char * (c[up] - field_level);
      SetScale(k_char, c, c_new);
      return DrawClass::Rink;
    }
    if (pole)
    {
      Vec3 foot{};
      foot[a] = wca;
      foot[b] = wcb;
      foot[up] = field_level;
      SetScale(k_char, foot, foot);
      return DrawClass::Rink;
    }
  }

  // StreetMR r32 (Lance: "get rid of the shadows on the tabletop version entirely, they just
  // collide with the players too much"): no shadows on the table - the players' and the ball's.
  // "tune tableshadows 1" brings back r29's (each lowered onto the court along the TV camera's
  // rays, then grown with its player).
  if ((floating_flat || flattened_shadow) && !s_table_shadows.load(std::memory_order_relaxed))
    return DrawClass::Skip;
  // StreetMR r29 (Lance: the shadows "arent in places they should be"): a floating flat shadow
  // goes back down onto the court along the TV camera's own rays (the exact inverse of how the
  // game lifted it: scaled about the camera so its plane lands on the floor - the TV picture is
  // unchanged), then grows with its player about the floor under him like his body.
  if (floating_flat && s_have_ice)
  {
    const Vec3 eye = CameraWorldPos(cam);
    const float cam_h = sign * (eye[up] - field_level);
    if (cam_h > hmin + 0.5f * u && hmin < 0.6f * cam_h)
    {
      const float s = cam_h / (cam_h - hmin);
      Vec3 on_floor{};
      for (int i = 0; i < 3; ++i)
        on_floor[i] = eye[i] + s * (0.5f * (wlo[i] + whi[i]) - eye[i]);
      Vec3 foot = on_floor;
      foot[up] = field_level;
      if (const Player* p = NearestPlayer(on_floor[a], on_floor[b], 2.5f * u))
      {
        foot[a] = p->a;
        foot[b] = p->b;
      }
      const float k = std::max(k_char, 1.0f);
      Vec3 eye_new{};
      for (int i = 0; i < 3; ++i)
        eye_new[i] = foot[i] + k * (eye[i] - foot[i]);
      SetScale(k * s, eye, eye_new);
      return DrawClass::Rink;
    }
  }

  // StreetMR r31 (Lance: "axe the cage entirely for now and just focus on the court itself"): on
  // the table only the court - its floor and lines, the hoops, the players and the ball. The
  // court's own model standing up from it (fences, the cage's arches, walls, benches, bins) and
  // everything with a matrix of its own beside the court (the crowd, props) go. The hoops' poles
  // and pads near a net stay. The VR 360 capture still gets all of it life-size (outside = Arena).
  if (m && !hoop_part && s_have_ice)
  {
    const bool by_net = NearHoop(wca, wcb, 2.5f * u) != nullptr;
    if (camera_only && hmax > 0.6f * u && !by_net)
      return outside;
    const bool off_court = wca < s_ice_min[a] || wca > s_ice_max[a] || wcb < s_ice_min[b] ||
                           wcb > s_ice_max[b];
    if (!camera_only && wmax > 1.0f * u && off_court)
      return outside;
  }

  // Tabletop VR: the field floor (grass, dirt, lines too) also lies life-size under your feet.
  // Camera-only, flat, at field level, bigger than a player's blob shadow.
  if (arena_life_size && camera_only && hi[up] - lo[up] < 0.3f * u &&
      std::abs(0.5f * (lo[up] + hi[up]) - field_level) < 0.5f * u &&
      std::max(hi[a] - lo[a], hi[b] - lo[b]) > 3.0f * u && !centre_outside)
  {
    return DrawClass::Both;
  }

  // StreetMR ("tablemaxh", metres; 0 = off): the court's own fixed structure taller than this
  // leaves the table (Lance: "take the cage off ... or at least make it open at the top"). The
  // Cage's fence and roof arches are camera-only draws 4-9 m tall; the backboards (4.5 m) are
  // skinned and the nets top out at 3.6 m, so they stay. Life-size around you in VR as before.
  if (const float max_h = s_table_max_h.load(std::memory_order_relaxed);
      max_h > 0.0f && m && camera_only && hmax > max_h * u)
  {
    return outside;
  }

  // Stadium, not field (Lance r8: light towers, roof canopies and the crowd behind home plate sat on
  // the Living Room table): anything wider than 2 m that starts 1.5 m above the field (stands,
  // crowd, roofs, light banks, the scoreboard), and anything over 12 m tall and wider than 1.5 m
  // (light towers, stand frames; the foul poles are thinner). The walls, dugouts and backstop
  // start at the ground and stay low.
  const float wide = std::max(wext_a, wext_b);
  if ((hmin > 1.5f * u && wide > 2.0f * u) || (hmax > 12.0f * u && wide > 1.5f * u))
    return outside;
  // Stand tiers and crowd rows (r16: Mario Stadium's start 0.4 m up right behind the walls and
  // rise to 11.7 m, just under the rule above, so a whole bleacher sat on the Living Room table):
  // anything over 6 m tall and wider than 3 m that isn't the field's own model. The walls,
  // backstop and dugouts stay under 4.5 m; the foul poles are thin.
  if (!camera_only && hmax > 6.0f * u && wide > 3.0f * u)
    return outside;
  // Anything floating more than 6 m up that isn't the ball (handled above): floodlight panels,
  // roof pieces (Lance r13: white panels hanging over the Living Room table).
  if (hmin > 6.0f * u)
    return outside;

  if (!inside && centre_outside)
    return outside;

  // On the table. Players (skinned parts) and what they hold (bat, glove, cap: small own-matrix
  // draws inside a player's reach) are drawn bigger about the player's feet.
  if (k_char > 1.01f && hmin > -1.0f * u && hmin < 6.0f * u && wext_up < 4.0f * u &&
      std::max(wext_a, wext_b) < 4.0f * u && (!m || !camera_only))
  {
    const Player* p = NearestPlayer(wca, wcb, m ? 1.2f * u : 1.5f * u);
    if (p || !m)
    {
      // StreetMR r28: about the court floor under the player, not his lowest point - a jump is
      // k times as high too, so a dunk meets the bigger rim and a shot the ball's bigger arc
      // (the ball and the hoops grow from the floor the same way).
      // r33: up at a rim, the floor under the net (PlayerAnchor).
      Vec3 foot{};
      foot[a] = wca;
      foot[b] = wcb;
      if (p)
        std::tie(foot[a], foot[b]) = PlayerAnchor(*p);
      foot[up] = field_level;
      SetScale(k_char, foot, foot);
    }
  }
  // Tabletop VR: the field's fixed parts (walls, backstop, dugouts, mound) also stand life-size
  // around you on the life-size field, not only on the table. Players and small props (anything
  // under 3 m that isn't the field's own model) stay on the table.
  if (arena_life_size && !s_scale_active && (camera_only || wide > 3.0f * u))
    return DrawClass::Both;
  return DrawClass::Rink;
}
}  // namespace

DrawClass ClassifyDraw(const u8* vertices, u32 count, u32 stride, int pos_offset, int components,
                       const float* single_matrix, const float* matrix_table, int posmtx_offset,
                       bool arena_life_size, bool blended)
{
  s_census_line.clear();
  s_plinth_pending = false;
  s_scale_active = false;
  s_floor_draw_now = false;
  s_particle_now = false;
  DrawClass cls = ClassifyDrawImpl(vertices, count, stride, pos_offset, components, single_matrix,
                                   matrix_table, posmtx_offset, arena_life_size, blended);
  // StreetMR VR 360 capture (Tabletop VR): only the life-size court - the table and everything on
  // it (players, ball, shadows) and the effects stay out of the picture; the floor and the court's
  // fixed parts are drawn life-size only.
  if (s_vr360_cap.load(std::memory_order_relaxed) && GetView() == View::Tabletop)
  {
    if (cls == DrawClass::Rink || s_particle_now)
      cls = DrawClass::Skip;
    else if (cls == DrawClass::Both)
      cls = DrawClass::Arena;
    s_plinth_pending = false;
  }
  if (cls != DrawClass::Rink)
    s_scale_active = false;
  if (s_census_active && !s_census_line.empty() && s_census_draws < 1200)
  {
    static constexpr const char* names[] = {"field", "skip", "around", "both"};
    s_census_text += fmt::format("{:4} {} -> {}{} |{}\n", s_census_draws++, s_census_line,
                                 names[static_cast<int>(cls) & 3],
                                 s_scale_active ? fmt::format(" x{:.1f}", s_scale_k) : "",
                                 s_census_material);
  }
  s_census_material.clear();
  return cls;
}

bool CensusActive()
{
  return s_census_active;
}

void SetCensusMaterial(std::string material)
{
  s_census_material = std::move(material);
}

void CensusNote(const std::string& line)
{
  if (s_census_active && s_census_draws < 1200)
    s_census_text += line + "\n";
}

int ShadowFix()
{
  return s_shadowfix.load(std::memory_order_relaxed);
}

void NoteEfbCopy(u32 address)
{
  // Video thread only. A handful of copies a frame; keep the last few addresses.
  for (u32& a : s_efb_copy_targets)
  {
    if (a == address)
      return;
  }
  s_efb_copy_targets[s_efb_copy_next] = address;
  s_efb_copy_next = (s_efb_copy_next + 1) % s_efb_copy_targets.size();
}

bool IsRecentEfbCopyTarget(u32 address)
{
  for (const u32 a : s_efb_copy_targets)
  {
    if (a != 0 && a == address)
      return true;
  }
  return false;
}

void BeginDraw()
{
  s_current_draw_class = DrawClass::Rink;
  s_scale_active = false;
}

bool GetDrawScale(float* k, std::array<float, 3>* center)
{
  if (!s_scale_active || s_current_draw_class != DrawClass::Rink ||
      GetView() != View::Tabletop)
  {
    return false;
  }
  *k = s_scale_k;
  *center = s_scale_center;
  return true;
}

void RequestCensus()
{
  s_census_request.store(true);
}

void OnStateLoaded()
{
  s_have_prev = false;
  s_exact_this_frame = false;
}

float TableLodBias()
{
  return s_texsharp.load(std::memory_order_relaxed);
}

int TableInternalResolution()
{
  return s_table_ir.load(std::memory_order_relaxed);
}

namespace
{
std::atomic<bool> s_snapshot_requested{false};
}

void RequestSnapshot()
{
  s_snapshot_requested.store(true, std::memory_order_release);
}

bool TakeSnapshotRequest()
{
  return s_snapshot_requested.exchange(false, std::memory_order_acq_rel);
}

float TableHudDegrees()
{
  return GetView() == View::Tabletop ? s_hud_deg.load(std::memory_order_relaxed) : 0.0f;
}

bool TableFogOff()
{
  return GetView() == View::Tabletop && !s_table_fog.load(std::memory_order_relaxed) &&
         !s_vr360_cap.load(std::memory_order_relaxed);
}

void SetGameState(int scene, int status)
{
  if (s_game_scene.exchange(scene, std::memory_order_relaxed) != scene)
    s_scene_reset.store(true);
  s_game_status.store(status, std::memory_order_relaxed);
}

bool InMatch()
{
  return s_frames_since_court < 120;
}

bool InMatchAnyThread()
{
  return s_match_on.load(std::memory_order_relaxed);
}

namespace
{
std::array<float, 6> s_last_persp_proj{};

std::mutex s_style_mutex;
std::string s_backdrop_style = "midnight";

std::string GetBackdropStyle()
{
  std::lock_guard lk(s_style_mutex);
  return s_backdrop_style;
}

// StreetMR r31 (Lance: VR environments "reflective of the game and NY basketball, until we get to
// the game", and NBA Street Vol. 2 themed ones for the load screens): the backdrop's key, once a
// frame (video thread). In a match: the court's own pictures. Four seconds without the court (the
// menus, the end of a game): the original rooftop at golden hour, or at night while the disc is
// loading. A new court is only shown once confirmed, so a match start doesn't flash the old one.
void UpdateBackdropKey()
{
  const bool in_match = s_frames_since_court < 240 && !s_court_key_vt.empty() &&
                        (s_court_candidate_frames == 0 || s_court_candidate == s_court_key_vt);
  // r33 (Lance: "each time you're at the menu it can rotate between day and night, but dont keep
  // rotatin day and night while im at the menu"): the rooftop is day or night for a whole stay in
  // the menus and loading screens - day the first time, then the other one after each match.
  static bool s_was_in_match = true;
  static bool s_night_visit = true;
  if (!in_match && s_was_in_match)
    s_night_visit = !s_night_visit;
  s_was_in_match = in_match;
  // r35: a court with no picture of its own (every court in the public build: the court pictures
  // are captures of the game and don't ship) gets the night rooftop around it.
  static std::map<std::string, bool> s_court_has_picture;
  bool court_picture = false;
  if (in_match)
  {
    auto it = s_court_has_picture.find(s_court_key_vt);
    if (it == s_court_has_picture.end())
    {
      const std::string base = File::GetUserPath(D_LOAD_IDX) + "StreetVR360/" + s_court_key_vt;
      it = s_court_has_picture
               .emplace(s_court_key_vt,
                        File::Exists(base + ".png") || File::Exists(base + "_sky.png"))
               .first;
    }
    court_picture = it->second;
  }
  // r35c: the start screen's VR background, one plain gradient. r35l (Lance: "delete the whole damn
  // blender render we'll try again tomorrow"): Hoop City is gone; an old "vegas" choice gets the
  // midnight gradient.
  std::string style = GetBackdropStyle();
  if (style.empty() || style == "vegas")
    style = "midnight";
  const std::string night = "streetmr_gradient_" + style;
  const std::string day = night;
  const std::string want = in_match && court_picture ? s_court_key_vt :
                           in_match                  ? night :
                           s_night_visit             ? night :
                                                       day;
  {
    std::lock_guard lk(s_court_mutex);
    if (want == s_court_key)
      return;
    s_court_key = want;
  }
  s_panorama_ready.store(false);
  s_court_gen.fetch_add(1);
  NOTICE_LOG_FMT(VIDEO, "HitzWorld: backdrop {}", want);
}
}  // namespace

void SetBackdropStyle(const std::string& style)
{
  std::lock_guard lk(s_style_mutex);
  if (s_backdrop_style != style)
    NOTICE_LOG_FMT(VIDEO, "HitzWorld: VR background {}", style);
  s_backdrop_style = style;
}

void NotePerspectiveProjection(const float* raw6)
{
  std::memcpy(s_last_persp_proj.data(), raw6, sizeof(float) * 6);
  // Negative Y scale = the game flips Y in its projection (see RigCamera).
  s_flip_y.store(raw6[2] < 0.0f, std::memory_order_relaxed);
}

bool ForceScreenRoute()
{
  // Off since Street r4 (Jumbotron is the flat quad); kept for "tune screenroute 1" experiments.
  return false && GetView() == View::Jumbotron &&
         s_force_screen_route.load(std::memory_order_relaxed);
}

void BringTableHere()
{
  std::lock_guard lk(s_placement_mutex);
  s_places[static_cast<int>(View::Tabletop)] = Placement{};  // re-placed in front of you next frame
}

void TurnLifeSize(float degrees)
{
  // Spin the life-size stadium about your head (not about its own centre), so the view from
  // where you stand turns around.
  std::lock_guard lk(s_placement_mutex);
  Placement& pl = s_places[static_cast<int>(View::Rinkside)];
  if (!pl.placed)
    return;
  const float d = degrees * 0.0174533f;
  const float c = std::cos(d), s = std::sin(d);
  const float dx = pl.position[0] - s_last_head[0], dz = pl.position[2] - s_last_head[2];
  // Same handedness as RotY (yaw about +Y).
  pl.position[0] = s_last_head[0] + c * dx + s * dz;
  pl.position[2] = s_last_head[2] - s * dx + c * dz;
  pl.yaw += d;
  pl.user_moved = true;
}

bool TakeTableBox(std::array<std::array<float, 3>, 8>* corners)
{
  // StreetMR r33 (Lance: "take away that box under the court"): no box; the court lies on its
  // own. "tune tablebox 1" brings it back.
  if (!s_table_box.load(std::memory_order_relaxed))
    return false;
  if (!s_plinth_pending || s_plinth_done_frame || !s_have_ice)
    return false;
  s_plinth_pending = false;
  s_plinth_done_frame = true;

  const float u = Upm();
  float size = 1.0f / 80.0f;
  {
    std::lock_guard lk(s_placement_mutex);
    const Placement& pl = s_places[static_cast<int>(View::Tabletop)];
    if (pl.placed)
      size = std::max(pl.size, 1e-4f);
  }
  const int up = UpAxis();
  const int a = (up + 1) % 3, b = (up + 2) % 3;
  // As wide as the field (+2 m, under the walls), from just under the field's lowest point down to
  // about your floor (the table top sits 0.55 m below eye level; 1.15 m more reaches the floor).
  const float sign = UpSign();
  const float lowest = sign > 0 ? s_ice_min[up] : s_ice_max[up];
  const float top = lowest - sign * 0.05f * u;
  const float bottom = top - sign * 1.15f * u / size;
  const float margin = 2.0f * u;
  for (int i = 0; i < 8; ++i)
  {
    std::array<float, 3>& c = (*corners)[i];
    c[a] = (i & 1) ? s_ice_max[a] + margin : s_ice_min[a] - margin;
    c[b] = (i & 2) ? s_ice_max[b] + margin : s_ice_min[b] - margin;
    c[up] = (i & 4) ? bottom : top;
  }
  return true;
}

void CommitFrame()
{
  s_plinth_done_frame = false;
  s_floor_seen_frame = false;
  // The next frame's draws start from the prediction until its first field piece arrives.
  s_exact_this_frame = false;

  // Census: write the frame just recorded, or start recording the next one.
  if (s_census_active)
  {
    s_census_active = false;
    std::ofstream out(File::GetUserPath(D_USER_IDX) + "census.txt");
    out << s_census_text;
    NOTICE_LOG_FMT(VIDEO, "HitzWorld: census written ({} draws)", s_census_draws);
  }
  if (s_census_request.exchange(false))
  {
    s_census_active = true;
    s_census_draws = 0;
    const Vec3 p = CameraWorldPos(s_cam);
    s_census_text = fmt::format(
        "camera pos ({:.2f},{:.2f},{:.2f}) right ({:.2f},{:.2f},{:.2f}) up ({:.2f},{:.2f},{:.2f}) "
        "back ({:.2f},{:.2f},{:.2f}) | up axis {} | field {} ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) "
        "| home dir {} ({:.2f},{:.2f},{:.2f}) | players {} | view {} | persp proj {:.6f} {:.6f} "
        "{:.6f} {:.6f} {:.6f} {:.6f}\n",
        p[0], p[1], p[2], s_cam.rot[0], s_cam.rot[1], s_cam.rot[2], s_cam.rot[3], s_cam.rot[4],
        s_cam.rot[5], s_cam.rot[6], s_cam.rot[7], s_cam.rot[8], UpAxis(), s_have_ice ? 1 : 0,
        s_ice_min[0], s_ice_min[1], s_ice_min[2], s_ice_max[0], s_ice_max[1], s_ice_max[2],
        s_have_home ? 1 : 0, s_home_dir[0], s_home_dir[1], s_home_dir[2], s_players.size(),
        ViewName(GetView()), s_last_persp_proj[0], s_last_persp_proj[1], s_last_persp_proj[2],
        s_last_persp_proj[3], s_last_persp_proj[4], s_last_persp_proj[5]);
    for (const Player& pl : s_players)
    {
      s_census_text += fmt::format("player ({:.1f},{:.1f}) low {:.2f} high {:.2f} n {}\n", pl.a,
                                   pl.b, pl.low, pl.high, pl.n);
    }
    for (const Hoop& h : s_hoops)
      s_census_text += fmt::format("hoop ({:.1f},{:.1f}) rim {:.2f}\n", h.a, h.b, h.rim);
    s_census_text += fmt::format("spectators {}\n", s_spectators.size());
  }

  std::call_once(s_tuning_loaded, LoadTuning);

  // "tune texlist 1": every 2 s, rewrite the texture lists when something new was seen.
  if (s_texlist_dirty && ++s_texlist_frames % 120 == 0)
  {
    s_texlist_dirty = false;
    const auto write_list = [](const char* file, const std::set<std::string>& names) {
      std::ofstream out(File::GetUserPath(D_USER_IDX) + file, std::ios::trunc);
      for (const std::string& n : names)
        out << n << '\n';
    };
    write_list("texlist_2d.txt", s_tex_2d);
    write_list("texlist_menu3d.txt", s_tex_menu3d);
  }
  const float u = Upm();

  // A new scene (menus <-> match): relearn everything from scratch - the camera, the field, home
  // plate's side - and place the views again.
  if (s_scene_reset.exchange(false))
  {
    NOTICE_LOG_FMT(VIDEO, "HitzWorld: scene {} - relearning the field",
                   s_game_scene.load(std::memory_order_relaxed));
    s_ground_mode = false;
    s_field_axis = -1;
    s_frames_without_ground = 0;
    s_ground.clear();
    s_have_prev = false;
    s_exact_this_frame = false;
    s_reset_pending.store(true);
  }

  // New tuning: forget the field and place every view again.
  if (s_reset_pending.exchange(false))
  {
    s_have_ice = false;
    s_had_ice = false;
    s_field_streak = 0;
    s_lower_ice_frames = 0;
    s_ice_gen++;
    s_have_home = false;
    s_home_sum = {};
    s_home_weight = 0.0f;
    s_spot_sum = {};
    s_spot_weight = 0.0f;
    std::lock_guard lk(s_placement_mutex);
    for (Placement& pl : s_places)
      pl = Placement{};
  }

  // Players: this frame's skinned parts clustered into players (parts within a metre of each
  // other horizontally), each with a foot point for next frame's size-up.
  s_players.clear();
  for (const Part& p : s_frame_parts)
  {
    Player* best = nullptr;
    float best_d = 1.0f * u;
    for (Player& q : s_players)
    {
      const float d = std::hypot(q.a - p.a, q.b - p.b);
      if (d < best_d)
      {
        best_d = d;
        best = &q;
      }
    }
    if (best)
    {
      const float w = static_cast<float>(best->n + p.n);
      best->a = (best->a * best->n + p.a * p.n) / w;
      best->b = (best->b * best->n + p.b * p.n) / w;
      best->low = std::min(best->low, p.low);
      best->high = std::max(best->high, p.high);
      best->n += p.n;
      best->parts++;
    }
    else
    {
      s_players.push_back({p.a, p.b, p.low, p.high, p.n, 1});
    }
  }
  s_frame_parts.clear();
  // Nets: kept while the court is drawn (a frame without the court - a menu, a cut - keeps them).
  // StreetMR r32 (Lance: "nothing feels solid ... yeah i mean the hoop"): a hoop, once found,
  // stays put. The net swings when the ball goes through and its rings' centre wobbles with it;
  // every hoop piece grows about that centre, so the whole hoop shook. A net seen within a metre of
  // a known hoop is the same hoop; a new one only adds.
  for (const Hoop& h : s_frame_hoops)
  {
    bool known = false;
    for (const Hoop& k : s_hoops)
      known = known || std::hypot(k.a - h.a, k.b - h.b) < 1.0f * u;
    if (!known && s_hoops.size() < 4)
      s_hoops.push_back(h);
    // r33: the rim height can only firm up (the net hangs below it; take the highest ring).
    for (Hoop& k : s_hoops)
    {
      if (std::hypot(k.a - h.a, k.b - h.b) < 1.0f * u)
        k.rim = std::max(k.rim, h.rim);
    }
  }
  s_frame_hoops.clear();
  // r33: this frame's spectators for the next frame's heads.
  s_spectators.swap(s_frame_spectators);
  s_frame_spectators.clear();

  // Calibration log every 2 s: the biggest flat surfaces seen (to measure the game's units).
  if (s_flat_log_frames++ % 120 == 0 && !s_frame_flats.empty())
  {
    const int up = UpAxis();
    const int a = (up + 1) % 3, b = (up + 2) % 3;
    std::sort(s_frame_flats.begin(), s_frame_flats.end(),
              [a, b](const FlatSurface& x, const FlatSurface& y) {
                return FlatArea(x.lo, x.hi, a, b) > FlatArea(y.lo, y.hi, a, b);
              });
    std::string line;
    size_t shown = 0;
    for (size_t i = 0; i < s_frame_flats.size() && shown < 10; ++i)
    {
      const FlatSurface& f = s_frame_flats[i];
      if (!Finite(f.lo) || !Finite(f.hi))
        continue;
      // The same surface drawn in several passes: show it once.
      if (i > 0 && std::abs(FlatArea(f.lo, f.hi, a, b) -
                            FlatArea(s_frame_flats[i - 1].lo, s_frame_flats[i - 1].hi, a, b)) <
                       0.01f)
      {
        continue;
      }
      shown++;
      line += fmt::format(" [{}{:.1f}x{:.1f} at h {:.1f}, c ({:.1f},{:.1f})]",
                          f.camera_only ? "" : "m ", f.hi[a] - f.lo[a], f.hi[b] - f.lo[b],
                          0.5f * (f.lo[up] + f.hi[up]), 0.5f * (f.lo[a] + f.hi[a]),
                          0.5f * (f.lo[b] + f.hi[b]));
    }
    NOTICE_LOG_FMT(VIDEO,
                   "HitzWorld: flats (scene {} status {}, up axis {} sign {}, ground {} area {:.0f}, exact {}/120, "
                   "view scale ({:.3f},{:.3f},{:.3f}), flip y {}, field {}, upm {:.2f}, players {}, "
                   "sized-up draws {}, {} seen):{}",
                   s_game_scene.load(), s_game_status.load(), up, UpSign(),
                   s_ground_mode ? "on" : "off", s_last_ground_area,
                   s_exact_frames, s_cam.scale[0], s_cam.scale[1], s_cam.scale[2],
                   s_flip_y.load() ? "yes" : "no", s_have_ice ? "yes" : "no", u, s_players.size(),
                   s_scaled_draws, s_frame_flats.size(), line);
    s_exact_frames = 0;
  }
  s_frame_flats.clear();
  s_frame_tallest_skinned = 0.0f;
  s_scaled_draws = 0;

  // Settle the field outline.
  bool saw_field = false;
  if (s_frame_ice)
  {
    const int up = UpAxis();
    const float settled_level = s_have_ice ? UpSign() * IceLevel() : 0.0f;
    if (!s_have_ice)
    {
      // A new field needs ten frames in a row (a menu's floor flashing by doesn't count).
      if (++s_field_streak >= 10)
      {
        const bool same_as_before =
            s_had_ice && SameOutline(s_frame_ice_min, s_frame_ice_max, s_last_ice_min,
                                     s_last_ice_max, 2.0f * u);
        s_ice_min = same_as_before ? s_last_ice_min : s_frame_ice_min;
        s_ice_max = same_as_before ? s_last_ice_max : s_frame_ice_max;
        s_have_ice = true;
        s_field_streak = 0;
        if (!same_as_before)
        {
          s_ice_gen++;
          s_have_home = false;
          s_home_sum = {};
          s_home_weight = 0.0f;
          s_spot_sum = {};
          s_spot_weight = 0.0f;
        }
        NOTICE_LOG_FMT(VIDEO,
                       "HitzWorld: field ({:.1f},{:.1f},{:.1f}) .. ({:.1f},{:.1f},{:.1f}) from {} "
                       "pieces, gen {}{}",
                       s_ice_min[0], s_ice_min[1], s_ice_min[2], s_ice_max[0], s_ice_max[1],
                       s_ice_max[2], s_frame_ice_pieces, s_ice_gen,
                       same_as_before ? " (same field as before)" : "");
      }
      saw_field = true;
    }
    else if (s_frame_ice_level < settled_level - 0.6f * u)
    {
      // A surface clearly below the settled outline, seen for a third of a second, is the real
      // field (the settled one was a raised surface): start over from it.
      if (++s_lower_ice_frames >= 20)
      {
        s_ice_min = s_frame_ice_min;
        s_ice_max = s_frame_ice_max;
        s_lower_ice_frames = 0;
        s_ice_gen++;
        s_have_home = false;
        s_home_sum = {};
        s_home_weight = 0.0f;
        s_spot_sum = {};
        s_spot_weight = 0.0f;
        NOTICE_LOG_FMT(VIDEO, "HitzWorld: lower field ({:.1f},{:.1f},{:.1f}) .. ({:.1f},{:.1f},{:.1f}) gen {}",
                       s_ice_min[0], s_ice_min[1], s_ice_min[2], s_ice_max[0], s_ice_max[1],
                       s_ice_max[2], s_ice_gen);
      }
    }
    else if (s_frame_ice_level <= settled_level + 0.6f * u)
    {
      // The same field: pieces the camera didn't show before make it grow (it never shrinks).
      s_lower_ice_frames = 0;
      saw_field = true;
      const int a = (up + 1) % 3, b = (up + 2) % 3;
      bool grew_a_lot = false;
      for (int k : {a, b})
      {
        const float size = std::max(s_ice_max[k] - s_ice_min[k], 1.0f * u);
        if (s_frame_ice_min[k] < s_ice_min[k] - 0.1f * size ||
            s_frame_ice_max[k] > s_ice_max[k] + 0.1f * size)
        {
          grew_a_lot = true;
        }
        s_ice_min[k] = std::min(s_ice_min[k], s_frame_ice_min[k]);
        s_ice_max[k] = std::max(s_ice_max[k], s_frame_ice_max[k]);
      }
      if (grew_a_lot)
      {
        s_ice_gen++;
        NOTICE_LOG_FMT(VIDEO, "HitzWorld: field grew to ({:.1f},{:.1f},{:.1f}) .. ({:.1f},{:.1f},{:.1f}) gen {}",
                       s_ice_min[0], s_ice_min[1], s_ice_min[2], s_ice_max[0], s_ice_max[1],
                       s_ice_max[2], s_ice_gen);
      }
    }
    s_frame_ice = false;
  }
  else
  {
    s_field_streak = 0;
  }
  s_frames_since_court = saw_field ? 0 : s_frames_since_court + 1;
  s_match_on.store(s_frames_since_court < 120, std::memory_order_relaxed);  // r35n, InMatchAnyThread
  UpdateBackdropKey();
  // Fifteen seconds without the field: back in the menus. Forget it (a new stadium may follow);
  // the same field again restores the old placements.
  if (s_have_ice && s_frames_since_court > 900)
  {
    NOTICE_LOG_FMT(VIDEO, "HitzWorld: field gone for 15 s, forgotten");
    ForgetField();
  }

  // The camera: the field's own matrix whenever the field is in view (ground vote). While it is
  // out of view (a pop-up against the sky) the last camera stays; 15 s without it (menus) falls
  // back to the matrix carrying the most geometry.
  const GroundCandidate* ground = nullptr;
  for (const GroundCandidate& g : s_ground)
  {
    if (g.area >= 1500.0 && (!ground || g.area > ground->area))
      ground = &g;
  }
  float cam_m[12];
  if (ground)
  {
    std::memcpy(cam_m, ground->m, sizeof(cam_m));
    if (!s_ground_mode)
    {
      NOTICE_LOG_FMT(VIDEO, "HitzWorld: camera follows the field (flat axis {}, level {:.2f}, area {:.0f}, scale {:.3f})",
                     ground->thin_axis, ground->level, ground->area,
                     Length({ground->m[0], ground->m[1], ground->m[2]}));
    }
    s_ground_mode = true;
    s_frames_without_ground = 0;
    s_field_axis = ground->thin_axis;
    s_field_model_level = ground->level;
    s_last_ground_area = ground->area;
    s_ground.clear();
    s_candidates.clear();
  }
  else if (s_ground_mode && ++s_frames_without_ground < 900)
  {
    s_ground.clear();
    s_candidates.clear();
    return;
  }
  else
  {
    if (s_ground_mode)
    {
      NOTICE_LOG_FMT(VIDEO, "HitzWorld: field out of view for 15 s, camera = most geometry again");
      s_ground_mode = false;
      s_field_axis = -1;
    }
    s_ground.clear();
    if (s_candidates.empty())
      return;
    const auto best = std::max_element(s_candidates.begin(), s_candidates.end(),
                                       [](const Candidate& x, const Candidate& y) {
                                         return x.weight < y.weight;
                                       });
    s_last_candidates = static_cast<u32>(s_candidates.size());
    s_last_best_weight = best->weight;
    s_last_second_weight = 0;
    for (const Candidate& c : s_candidates)
    {
      if (&c != &*best)
        s_last_second_weight = std::max(s_last_second_weight, c.weight);
    }
    std::memcpy(cam_m, best->m, sizeof(cam_m));
    s_candidates.clear();
  }
  s_last_scale = Length({cam_m[0], cam_m[1], cam_m[2]});
  Camera cam;
  if (!ToCamera(cam_m, &cam))
    return;
  if (s_ground_mode)
  {
    // The camera is above the field: that side of the flat axis is up.
    const Vec3 cp = CameraWorldPos(cam);
    s_field_up_sign = cp[s_field_axis] >= s_field_model_level ? 1.0f : -1.0f;
  }

  s_cam_prev = s_cam;
  s_have_prev = s_have_camera;
  s_cam = cam;
  s_have_camera = true;
  LearnUp(cam, s_have_prev ? &s_cam_prev : nullptr);

  // Predict the next frame by repeating this frame's motion, unless it was a cut.
  s_cam_next = s_cam;
  float cam_angle = 1.0f, cam_step = 1e30f;
  if (s_have_prev)
  {
    cam_angle = AngleBetween(s_cam.rot, s_cam_prev.rot);
    const Vec3 p0 = CameraWorldPos(s_cam_prev), p1 = CameraWorldPos(s_cam);
    const Vec3 dp{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    cam_step = Length(dp);
    const float dist = std::max(Length(p1), 1e-3f);
    if (cam_angle < 0.12f && cam_step < dist * 0.15f)
    {
      // rot_next = (rot * rot_prev^T) * rot ; pos_next = p1 + dp
      const Mat3 delta = Mul(s_cam.rot, Transpose(s_cam_prev.rot));
      s_cam_next.rot = Orthonormalize(Mul(delta, s_cam.rot));
      const Vec3 p2{p1[0] + dp[0], p1[1] + dp[1], p1[2] + dp[2]};
      const Vec3 t = MulVec(s_cam_next.rot, p2);
      s_cam_next.trans = {-t[0], -t[1], -t[2]};
    }
  }

  // Home plate's side: the camera's average position seen from the field's centre (a still
  // camera, the pitch-and-swing view, counts four times). In Mario Superstar Baseball only the
  // at-bat camera counts (it is always behind home plate; the intro flyover circles the park).
  const bool scene_known = s_game_scene.load(std::memory_order_relaxed) >= 0;
  const bool at_bat = s_game_status.load(std::memory_order_relaxed) == 1;
  if (s_have_ice && saw_field && s_up_frames >= 30 && (!scene_known || at_bat))
  {
    const int up = UpAxis();
    const Vec3 cp = CameraWorldPos(s_cam), c = IceCenter();
    Vec3 d{cp[0] - c[0], cp[1] - c[1], cp[2] - c[2]};
    d[up] = 0.0f;
    const float len = Length(d);
    if (len > 2.0f * u)
    {
      const float w = (cam_angle < 0.004f && cam_step < 0.05f * u) ? 4.0f : 1.0f;
      for (int i = 0; i < 3; ++i)
      {
        s_home_sum[i] += w * d[i] / len;
        s_spot_sum[i] += w * cp[i];
      }
      s_home_weight += w;
      s_spot_weight += w;
    }
    if (s_home_weight >= 240.0f)
    {
      const float consistency = Length(s_home_sum) / s_home_weight;
      const Vec3 dir = Normalize(s_home_sum);
      if (consistency > 0.3f && (!s_have_home || Dot(dir, s_home_dir) < 0.7f))
      {
        s_home_dir = dir;
        s_have_home = true;
        s_ice_gen++;  // re-place the views facing out from home plate
        NOTICE_LOG_FMT(VIDEO, "HitzWorld: home plate side ({:.2f},{:.2f},{:.2f}) (consistency {:.2f}), gen {}",
                       dir[0], dir[1], dir[2], consistency, s_ice_gen);
      }
    }
  }

  if (s_log_frames++ % 60 == 0)
  {
    const Vec3 p = CameraWorldPos(s_cam);
    INFO_LOG_FMT(VIDEO,
                 "HitzWorld: cam pos ({:.1f},{:.1f},{:.1f}) dist {:.1f} | right ({:.2f},{:.2f},{:.2f}) "
                 "up ({:.2f},{:.2f},{:.2f}) back ({:.2f},{:.2f},{:.2f}) | scale {:.3f} | best {} of {} "
                 "cands, 2nd {} | view {}",
                 p[0], p[1], p[2], Length(p), s_cam.rot[0], s_cam.rot[1], s_cam.rot[2],
                 s_cam.rot[3], s_cam.rot[4], s_cam.rot[5], s_cam.rot[6], s_cam.rot[7],
                 s_cam.rot[8], s_last_scale, s_last_best_weight, s_last_candidates,
                 s_last_second_weight, ViewName(GetView()));
  }
}

namespace
{
// Places a view the first time (and again when the field outline or home plate's side arrives,
// unless Lance moved it). Caller holds s_placement_mutex.
Placement& EnsurePlaced(View view, const std::array<float, 3>& head, float head_yaw)
{
  Placement& pl = s_places[static_cast<int>(view)];
  if (!pl.placed || (s_have_ice && pl.ice_gen != s_ice_gen && !pl.user_moved))
  {
    const float u = Upm();
    pl.world_to_rink = RinkFrame(s_cam);
    pl.units_per_meter_life = u;
    pl.rink_center = s_have_ice ? IceCenter() : Vec3{0, 0, 0};
    pl.ice_gen = s_have_ice ? s_ice_gen : 0;
    pl.yaw = head_yaw;
    if (s_have_ice)
    {
      float hw = 0.0f, hd = 0.0f;
      FieldHalfExtents(pl.world_to_rink, &hw, &hd);
      pl.half_width_m = hw / u;
      pl.half_depth_m = hd / u;
    }
    const Vec3 fwd{-std::sin(head_yaw), 0.0f, -std::cos(head_yaw)};
    if (view == View::Tabletop)
    {
      // The whole field fits `tablesize` metres; its near edge (home plate) 35 cm in front of
      // you, the top at waist height.
      const float longest = std::max({2.0f * pl.half_width_m, 2.0f * pl.half_depth_m, 1.0f});
      pl.size = std::clamp(s_table_size.load(std::memory_order_relaxed) / longest, 0.002f, 1.0f);
      const float dist = pl.half_depth_m * pl.size + 0.35f;
      pl.position = {head[0] + fwd[0] * dist, head[1] - 0.55f, head[2] + fwd[2] * dist};
    }
    else
    {
      // The life-size stadium around you: you stand at field level where the at-bat camera films
      // from (behind home plate, in front of the backstop), looking out to centre field, the
      // table in front of you. Until that spot is known: `inset` metres inside the field's back
      // edge. `off` = from you to the field's centre, field frame, metres.
      pl.size = 1.0f;
      Vec3 off{0.0f, 0.0f,
               -std::max(pl.half_depth_m - s_inset.load(std::memory_order_relaxed), 1.0f)};
      if (s_have_ice && s_have_home && s_spot_weight > 0.0f)
      {
        const Vec3 spot{s_spot_sum[0] / s_spot_weight, s_spot_sum[1] / s_spot_weight,
                        s_spot_sum[2] / s_spot_weight};
        const Vec3 d = MulVec(pl.world_to_rink, {pl.rink_center[0] - spot[0],
                                                 pl.rink_center[1] - spot[1],
                                                 pl.rink_center[2] - spot[2]});
        off = {d[0] / u, 0.0f, d[2] / u};
      }
      const Vec3 r = MulVec(RotY(head_yaw), off);
      pl.position = {head[0] + r[0], head[1] - 1.7f, head[2] + r[2]};
    }
    pl.placed = true;
    pl.placed_for = view;
    NOTICE_LOG_FMT(VIDEO,
                   "HitzWorld: {} placed at ({:.2f},{:.2f},{:.2f}) yaw {:.0f} size 1/{:.1f} field "
                   "{:.0f} x {:.0f} m, life upm {:.2f} | up axis {} | home {}",
                   ViewName(view), pl.position[0], pl.position[1], pl.position[2],
                   pl.yaw * 57.2958f, 1.0f / std::max(pl.size, 1e-4f), 2.0f * pl.half_width_m,
                   2.0f * pl.half_depth_m, pl.units_per_meter_life, UpAxis(),
                   s_have_home ? "known" : "not yet");
  }
  return pl;
}

// `cam` = the rig camera (RigCamera of the current or predicted camera): rigid view space,
// mirrored in Y when the game flips Y.
void ComputeAnchor(const Placement& pl, const std::array<float, 3>& home, const Camera& cam,
                   std::array<float, 3>* out_position, std::array<float, 9>* out_rotation,
                   float* out_units_per_meter)
{
  // world -> room rotation Y = RotY(yaw) * world_to_rink; the rig (room) -> view rotation the
  // anchor needs is A = R_cam * Y^T.
  const Mat3 Y = Mul(RotY(pl.yaw), pl.world_to_rink);
  const Mat3 A = Mul(cam.rot, Transpose(Y));
  const float s = pl.units_per_meter_life / std::max(pl.size, 1e-4f);

  // anchor = A * (home - P) * s + R * c + t, where c is the field centre in world coordinates
  // that sits at P in the room (derivation in the Hitz NOTES.md, r5/r8).
  const Vec3 d{(home[0] - pl.position[0]) * s, (home[1] - pl.position[1]) * s,
               (home[2] - pl.position[2]) * s};
  const Vec3 ad = MulVec(A, d);
  const Vec3 rc = MulVec(cam.rot, pl.rink_center);
  *out_position = {ad[0] + rc[0] + cam.trans[0], ad[1] + rc[1] + cam.trans[1],
                   ad[2] + rc[2] + cam.trans[2]};
  *out_rotation = A;
  *out_units_per_meter = s;
}

// The hand is on (or right at) the table: over its top within 8 cm of the edge, or along its
// sides down toward the floor. Caller holds s_placement_mutex.
bool HandOnTable(const Placement& pl, const std::array<float, 3>& hand)
{
  if (!pl.placed)
    return false;
  const float dx = hand[0] - pl.position[0], dz = hand[2] - pl.position[2];
  const float c = std::cos(pl.yaw), s = std::sin(pl.yaw);
  // Field frame axes in the room: right = RotY(yaw)*(1,0,0), back = RotY(yaw)*(0,0,1).
  const float lx = c * dx - s * dz;
  const float lz = s * dx + c * dz;
  const float dy = hand[1] - pl.position[1];
  return std::abs(lx) <= pl.half_width_m * pl.size + 0.08f &&
         std::abs(lz) <= pl.half_depth_m * pl.size + 0.08f && dy <= 0.10f && dy >= -1.2f;
}
}  // namespace

void SetTuning(const std::string& key, float value)
{
  if (key == "upm" && value > 0.0f)
    s_upm.store(value);
  else if (key == "tablesize" && value >= 0.2f && value <= 6.0f)
    s_table_size.store(value);
  else if (key == "charscale" && value >= 1.0f && value <= 4.0f)
    s_char_scale.store(value);
  else if (key == "inset" && value >= 0.0f && value <= 60.0f)
    s_inset.store(value);
  else if (key == "texsharp" && value >= 0.0f && value <= 6.0f)
    s_texsharp.store(value);
  else if (key == "shadowfix" && value >= 0.0f && value <= 6.0f)
    s_shadowfix.store(static_cast<int>(value));
  else if (key == "screenroute")
    s_force_screen_route.store(value != 0.0f);
  else if (key == "courtradius" && value >= 0.0f && value <= 200.0f)
    s_court_radius.store(value);
  else if (key == "fieldmax" && value >= 10.0f && value <= 1000.0f)
    s_field_max.store(value);
  else if (key == "gripgrab")
    s_grip_grab.store(value != 0.0f);  // StreetMR r14: unused (grips grab on the table only)
  else if (key == "levelhead" && value >= -90.0f && value <= 90.0f)
    s_dev_level_head.store(value);
  else if (key == "flatgate")
    s_flat_gate.store(value != 0.0f);
  else if (key == "tablemaxh" && value >= 0.0f && value <= 60.0f)
    s_table_max_h.store(value);
  else if (key == "flatmr")
    s_flat_mr.store(value != 0.0f);
  else if (key == "vrsky" && value >= 0.0f && value <= 16777215.0f)
    s_vr_sky.store(static_cast<u32>(value));
  else if (key == "texlist")
    s_texlist.store(value != 0.0f);
  else if (key == "refresh" && value >= 0.0f && value <= 144.0f)
    s_refresh.store(value);
  else if (key == "mreager")
    s_mr_eager.store(value != 0.0f);
  else if (key == "tableir" && value >= 0.0f && value <= 6.0f)
    s_table_ir.store(static_cast<int>(value));
  else if (key == "tablefog")
    s_table_fog.store(value != 0.0f);
  else if (key == "tableshadows")
    s_table_shadows.store(value != 0.0f);
  else if (key == "tablebox")
    s_table_box.store(value != 0.0f);
  else if (key == "hudsize" && value >= 10.0f && value <= 90.0f)
    s_hud_deg.store(value);
  else if (key == "vr360cap")
    s_vr360_cap.store(value != 0.0f);  // re-places every view (the capture rig faces yaw 0)
  else if (key == "capyaw" && value >= -360.0f && value <= 360.0f)
    s_cap_yaw.store(value);
  else if (key == "cappitch" && value >= -90.0f && value <= 90.0f)
    s_cap_pitch.store(value);
  else if (key == "capfov" && value >= 30.0f && value <= 170.0f)
    s_cap_fov.store(value);
  else
  {
    WARN_LOG_FMT(VIDEO, "HitzWorld: unknown tuning {} {}", key, value);
    return;
  }
  if (key != "screenroute" && key != "charscale" && key != "texsharp" && key != "gripgrab" &&
      key != "shadowfix" && key != "levelhead" && key != "flatgate" && key != "flatmr" &&
      key != "vrsky" && key != "texlist" && key != "refresh" && key != "mreager" &&
      key != "tableir" && key != "capyaw" &&
      key != "cappitch" && key != "capfov" && key != "tablefog" && key != "tableshadows" &&
      key != "tablebox" && key != "hudsize")
    s_reset_pending.store(true);
  NOTICE_LOG_FMT(VIDEO, "HitzWorld: tuning {} = {}", key, value);
}

bool GetAnchor(const std::array<float, 3>& home, const std::array<float, 3>& head, float head_yaw,
               std::array<float, 3>* out_position, std::array<float, 9>* out_rotation,
               float* out_units_per_meter)
{
  const View view = GetView();
  // Wait for a second of camera data so the learned up axis is settled before placing.
  if (view == View::Jumbotron || !s_have_camera || s_up_frames < 30)
    return false;

  std::lock_guard lk(s_placement_mutex);
  s_last_head = head;
  s_last_home = home;
  ComputeAnchor(EnsurePlaced(view, head, head_yaw), home, RigCamera(s_cam_next), out_position,
                out_rotation, out_units_per_meter);
  return true;
}

bool GetArenaAnchor(const std::array<float, 3>& home, const std::array<float, 3>& head,
                    float head_yaw, std::array<float, 3>* out_position,
                    std::array<float, 9>* out_rotation, float* out_units_per_meter)
{
  // Only Tabletop has a separate life-size stadium around the player.
  if (GetView() != View::Tabletop || !s_have_camera || s_up_frames < 30)
    return false;

  std::lock_guard lk(s_placement_mutex);
  ComputeAnchor(EnsurePlaced(View::Rinkside, head, head_yaw), home, RigCamera(s_cam_next),
                out_position, out_rotation, out_units_per_meter);
  return true;
}

bool GetCurrentAnchor(bool arena, std::array<float, 3>* out_position,
                      std::array<float, 9>* out_rotation, float* out_units_per_meter)
{
  const View view = GetView();
  if (view == View::Jumbotron || !s_have_camera || s_up_frames < 30 ||
      (arena && view != View::Tabletop))
  {
    return false;
  }
  const Camera rig = RigCamera(CurrentCamera());
  std::lock_guard lk(s_placement_mutex);
  const Placement& pl = s_places[static_cast<int>(arena ? View::Rinkside : view)];
  if (!pl.placed)
    return false;
  ComputeAnchor(pl, s_last_home, rig, out_position, out_rotation, out_units_per_meter);
  return true;
}

std::array<float, 3> GetViewScale()
{
  return CurrentCamera().scale;
}

void SetCurrentDrawClass(DrawClass c)
{
  s_current_draw_class = c;
}

DrawClass GetCurrentDrawClass()
{
  return s_current_draw_class;
}

float DevLevelHead()
{
  return s_dev_level_head.load(std::memory_order_relaxed);
}

float DisplayRefreshWanted()
{
  return s_refresh.load(std::memory_order_relaxed);
}

bool MrEagerHeartbeat()
{
  return s_mr_eager.load(std::memory_order_relaxed);
}

namespace
{
std::mutex s_display_stats_mutex;
std::string s_display_stats;
}  // namespace

void NoteDisplayStats(float hz, u32 fresh, u32 repeat, u32 cycles, double seconds,
                      const std::string& extra)
{
  if (seconds <= 0.0)
    return;
  std::string text =
      fmt::format(" hz={:.0f} shown={:.1f}/s repeated={:.1f}/s display={:.1f}/s{}", hz,
                  fresh / seconds, repeat / seconds, cycles / seconds, extra);
  std::lock_guard lk(s_display_stats_mutex);
  s_display_stats = std::move(text);
}

std::string DisplayStatsText()
{
  std::lock_guard lk(s_display_stats_mutex);
  return s_display_stats;
}

bool IsCourtFloorDraw()
{
  return s_floor_draw_now;
}

void NoteCourtTexture(const std::string& key)
{
  // The first floor draw of each frame; a new key has to hold for half a second (the menus' 3D
  // backdrops or a cut-away must not flip the court back and forth).
  if (key.empty() || key == s_court_key_vt)
  {
    s_court_candidate_frames = 0;
    return;
  }
  if (key != s_court_candidate)
  {
    s_court_candidate = key;
    s_court_candidate_frames = 0;
  }
  if (++s_court_candidate_frames < 30)
    return;
  s_court_key_vt = key;
  s_court_candidate_frames = 0;
  s_hoops.clear();  // StreetMR r32: another court, other hoops
  // The backdrop follows in UpdateBackdropKey (end of frame).
  NOTICE_LOG_FMT(VIDEO, "HitzWorld: court {}", key);
  // For Capture-VR360.ps1: which court the pictures being taken belong to.
  File::WriteStringToFile(File::GetUserPath(D_USER_IDX) + "vr360_court.txt", key + "\n");
}

std::string CourtKey()
{
  std::lock_guard lk(s_court_mutex);
  return s_court_key;
}

void SetDiscLoading(bool loading)
{
  s_disc_loading.store(loading, std::memory_order_relaxed);
}

bool IsCourtTexture(const std::string& name)
{
  // Video thread (s_court_key_vt is the video thread's).
  return !name.empty() && name == s_court_key_vt;
}

u32 CourtKeyGeneration()
{
  return s_court_gen.load(std::memory_order_acquire);
}

void SetCourtPanoramaReady(bool ready)
{
  s_panorama_ready.store(ready, std::memory_order_release);
}

bool CourtPanoramaReady()
{
  return s_panorama_ready.load(std::memory_order_acquire);
}

void SetBackdropSupported(bool supported)
{
  s_backdrop_supported.store(supported, std::memory_order_release);
}

bool BackdropSupported()
{
  return s_backdrop_supported.load(std::memory_order_acquire);
}

float TableYaw()
{
  std::lock_guard lk(s_placement_mutex);
  const Placement& pl = s_places[static_cast<int>(View::Tabletop)];
  return pl.placed ? pl.yaw : 0.0f;
}

bool Vr360Capture()
{
  return s_vr360_cap.load(std::memory_order_relaxed);
}

bool DevCapturePose(float* yaw_deg, float* pitch_deg, float* fov_deg)
{
  if (!s_vr360_cap.load(std::memory_order_relaxed))
    return false;
  *yaw_deg = s_cap_yaw.load(std::memory_order_relaxed);
  *pitch_deg = s_cap_pitch.load(std::memory_order_relaxed);
  *fov_deg = s_cap_fov.load(std::memory_order_relaxed);
  return true;
}

bool FlatGate()
{
  return s_flat_gate.load(std::memory_order_relaxed);
}

bool FlatPassthrough()
{
  return s_flat_mr.load(std::memory_order_relaxed);
}

u32 VRSkyColor()
{
  return s_vr_sky.load(std::memory_order_relaxed);
}

bool TexListActive()
{
  return s_texlist.load(std::memory_order_relaxed);
}

bool SkipTextureDump(u32 width, u32 height)
{
  // Y and chroma planes of the intro, menu and court-preview movies (Street r17 dump: 3958
  // textures, ~3800 of them movie frames).
  static constexpr std::array<std::pair<u32, u32>, 12> MOVIE_PLANES = {{
      {640, 240}, {320, 120}, {320, 240}, {160, 120}, {160, 416}, {80, 208},
      {176, 288}, {88, 144}, {336, 192}, {168, 96}, {176, 96}, {88, 48},
  }};
  for (const auto& [w, h] : MOVIE_PLANES)
  {
    if (width == w && height == h)
      return true;
  }
  return false;
}

void NoteTexture(const std::string& name, bool ortho)
{
  if (name.empty())
    return;
  if (ortho)
    s_texlist_dirty |= s_tex_2d.insert(name).second;
  else if (!InMatch())
    s_texlist_dirty |= s_tex_menu3d.insert(name).second;
}

bool RightHandGrabbing()
{
  return s_right_grab.load(std::memory_order_relaxed);
}

namespace
{
int ScreenRoom()
{
  return HitzModes::StadiumWanted() ? 1 : 0;
}

// Caller holds s_screen_mutex.
void PlaceScreenAhead(FreeScreen& s, float height, float distance)
{
  const float yaw = s_head_valid ? s_head_yaw : 0.0f;
  const Vec3 from = s_head_valid ? s_head_pos : Vec3{0.0f, 0.0f, 0.0f};
  s.position = {from[0] - std::sin(yaw) * distance, from[1], from[2] - std::cos(yaw) * distance};
  s.yaw = yaw;
  s.width = height * s_screen_aspect;
  s.placed = true;
}

// Caller holds s_screen_mutex.
void LoadScreens()
{
  s_screens_loaded = true;
  std::ifstream in(File::GetUserPath(D_USER_IDX) + "street_screens.txt");
  int room = 0, placed = 0;
  float x, y, z, yaw, width;
  while (in >> room >> placed >> x >> y >> z >> yaw >> width)
  {
    if (room < 0 || room > 1 || !placed || !std::isfinite(x + y + z + yaw + width))
      continue;
    s_screens[room] = FreeScreen{true, {x, y, z}, yaw, std::clamp(width, 0.5f, 40.0f)};
  }
}

// Caller holds s_screen_mutex.
void SaveScreens()
{
  std::ofstream out(File::GetUserPath(D_USER_IDX) + "street_screens.txt", std::ios::trunc);
  for (int room = 0; room < 2; ++room)
  {
    const FreeScreen& s = s_screens[room];
    out << fmt::format("{} {} {:.3f} {:.3f} {:.3f} {:.4f} {:.3f}\n", room, s.placed ? 1 : 0,
                       s.position[0], s.position[1], s.position[2], s.yaw, s.width);
  }
}

// The left controller's aim ray hits the screen (with a 10% margin). Caller holds s_screen_mutex.
bool AimHitsScreen(const FreeScreen& s, const Common::VR::OpenXRPoseState& aim)
{
  if (!s.placed || !aim.valid)
    return false;
  const auto& q = aim.orientation;  // x, y, z, w; the aim ray is the pose's -Z
  const Vec3 d{-2.0f * (q[0] * q[2] + q[3] * q[1]), -2.0f * (q[1] * q[2] - q[3] * q[0]),
               -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]))};
  const Vec3 n{std::sin(s.yaw), 0.0f, std::cos(s.yaw)};
  const float denom = Dot(d, n);
  if (std::abs(denom) < 1e-4f)
    return false;
  const Vec3 to{s.position[0] - aim.position[0], s.position[1] - aim.position[1],
                s.position[2] - aim.position[2]};
  const float t = Dot(to, n) / denom;
  if (t <= 0.0f)
    return false;
  const Vec3 hit{aim.position[0] + d[0] * t - s.position[0], aim.position[1] + d[1] * t - s.position[1],
                 aim.position[2] + d[2] * t - s.position[2]};
  const float lx = std::cos(s.yaw) * hit[0] - std::sin(s.yaw) * hit[2];
  const float half_w = 0.55f * s.width, half_h = 0.55f * s.width / s_screen_aspect;
  return std::abs(lx) <= half_w && std::abs(hit[1]) <= half_h;
}
}  // namespace

bool UpdateScreenGrab(const Common::VR::OpenXRControllerState& left,
                      const Common::VR::OpenXRControllerState& right)
{
  std::lock_guard lk(s_screen_mutex);
  FreeScreen& s = s_screens[ScreenRoom()];
  const bool grip_l = left.grip_pose.valid && left.aim_pose.valid &&
                      (left.squeeze_value > 0.6f || (s_screen_grab && left.squeeze_value > 0.4f));
  const bool grip_r = right.grip_pose.valid &&
                      (right.squeeze_value > 0.6f || (s_screen_two_hands && right.squeeze_value > 0.4f));
  int hands = 0;
  if (grip_l && (s_screen_grab != 0 || (!s_screen_prev_grip && AimHitsScreen(s, left.aim_pose))))
    hands = 1;
  if (hands && grip_r)
    hands |= 2;
  s_screen_prev_grip = left.squeeze_value > 0.4f;

  const Vec3 hl = left.aim_pose.position;
  const Vec3 hr = right.grip_pose.position;
  if (hands != s_screen_grab)
  {
    if (hands == 0)
    {
      SaveScreens();
      NOTICE_LOG_FMT(VIDEO, "HitzWorld: screen moved to ({:.2f},{:.2f},{:.2f}) yaw {:.0f}, {:.1f} m wide",
                     s.position[0], s.position[1], s.position[2], s.yaw * 57.2958f, s.width);
    }
    else
    {
      const Vec3 off{s.position[0] - s_head_pos[0], s.position[1] - s_head_pos[1],
                     s.position[2] - s_head_pos[2]};
      s_screen_gain = s_head_valid ? std::clamp(Length(off) / 0.6f, 1.0f, 15.0f) : 1.0f;
    }
    s_sgrab_pos = s.position;
    s_sgrab_hand = hl;
    if (hands & 2)
    {
      const float dx = hr[0] - hl[0], dz = hr[2] - hl[2];
      s_sgrab_dist = std::max(0.05f, std::hypot(dx, dz));
      s_sgrab_angle = std::atan2(dx, dz);
      s_sgrab_width = s.width;
      s_sgrab_yaw = s.yaw;
      s_sgrab_pos2 = s.position;
      s_sgrab_mid = {0.5f * (hl[0] + hr[0]), 0.5f * (hl[1] + hr[1]), 0.5f * (hl[2] + hr[2])};
    }
    s_screen_grab = hands;
  }
  s_screen_two_hands = (s_screen_grab & 2) != 0;
  if (s_screen_grab & 2)
  {
    const float dx = hr[0] - hl[0], dz = hr[2] - hl[2];
    const float dist = std::max(0.05f, std::hypot(dx, dz));
    s.width = std::clamp(s_sgrab_width * dist / s_sgrab_dist, 0.5f, 40.0f);
    s.yaw = s_sgrab_yaw + WrapAngle(std::atan2(dx, dz) - s_sgrab_angle);
    const Vec3 mid{0.5f * (hl[0] + hr[0]), 0.5f * (hl[1] + hr[1]), 0.5f * (hl[2] + hr[2])};
    for (int i = 0; i < 3; ++i)
      s.position[i] = s_sgrab_pos2[i] + (mid[i] - s_sgrab_mid[i]) * s_screen_gain;
  }
  else if (s_screen_grab & 1)
  {
    for (int i = 0; i < 3; ++i)
      s.position[i] = s_sgrab_pos[i] + (hl[i] - s_sgrab_hand[i]) * s_screen_gain;
  }
  return s_screen_grab != 0;
}

void NoteHead(const std::array<float, 3>& position, float yaw)
{
  std::lock_guard lk(s_screen_mutex);
  s_head_pos = position;
  s_head_yaw = yaw;
  s_head_valid = true;
}

void BringScreenHere()
{
  std::lock_guard lk(s_screen_mutex);
  s_screen_bring = true;
}

void SetScreenPreset(float height, float distance)
{
  std::lock_guard lk(s_screen_mutex);
  s_screen_preset_height = height;
  s_screen_preset_distance = distance;
}

void GetScreen(float aspect, float default_height, float default_distance,
               std::array<float, 3>* position, float* yaw, float* width)
{
  std::lock_guard lk(s_screen_mutex);
  if (aspect > 0.1f)
    s_screen_aspect = aspect;
  if (!s_screens_loaded)
    LoadScreens();
  FreeScreen& s = s_screens[ScreenRoom()];
  if (s_screen_preset_height > 0.0f)
  {
    PlaceScreenAhead(s, s_screen_preset_height, s_screen_preset_distance);
    s_screen_preset_height = 0.0f;
    s_screen_bring = false;
    SaveScreens();
  }
  else if (s_screen_bring || !s.placed)
  {
    // In front again at its current size (or the default the first time).
    const float height = s.placed ? s.width / s_screen_aspect : default_height;
    PlaceScreenAhead(s, height, s.placed ? std::max(1.0f, height * 1.5f) : default_distance);
    if (s_screen_bring)
      SaveScreens();
    s_screen_bring = false;
  }
  *position = s.position;
  *yaw = s.yaw;
  *width = s.width;
}

bool OnControllers(const std::array<Common::VR::OpenXRControllerState, 2>& controllers)
{
  const auto& left = controllers[0];
  const auto& right = controllers[1];
  const View view = GetView();
  const std::array<const Common::VR::OpenXRControllerState*, 2> hands{&left, &right};

  // StreetMR r27, like Quest 3 Strikers Charged (Lance: "the same grabbing ... as in charged"): in
  // Tabletop the left grip alone holds the table, anywhere (its game button, a duplicate turbo,
  // is held back); the right grip joins it for turning and resizing (its steal button is held back
  // until it lets go). The right grip alone stays the game's steal. In the Jumbotron the left grip
  // grabs the screen when the left controller points at it (UpdateScreenGrab).
  const auto held = [](const Common::VR::OpenXRControllerState& c, bool was) {
    return c.grip_pose.valid && (c.squeeze_value > 0.6f || (was && c.squeeze_value > 0.4f));
  };
  std::array<bool, 2> wants{};
  wants[0] = view == View::Tabletop && held(left, s_press_grabs[0]);
  wants[1] = wants[0] && held(right, s_press_grabs[1]);
  s_press_grabs[0] = wants[0];
  s_press_grabs[1] = wants[1];
  if (view == View::Jumbotron)
  {
    const bool screen = UpdateScreenGrab(left, right);
    s_left_grab.store(screen, std::memory_order_relaxed);
    if (s_screen_two_hands)
      s_right_hold = true;
    if (right.squeeze_value < 0.4f)
      s_right_hold = false;
    s_right_grab.store(s_right_hold, std::memory_order_relaxed);
    s_grab_mode = GrabMode::None;
    return screen;
  }
  if (wants[1])
    s_right_hold = true;
  if (right.squeeze_value < 0.4f)
    s_right_hold = false;
  s_left_grab.store(wants[0], std::memory_order_relaxed);
  s_right_grab.store(s_right_hold, std::memory_order_relaxed);

  std::lock_guard lk(s_placement_mutex);
  Placement& pl = s_places[static_cast<int>(view)];
  const GrabMode want = !pl.placed            ? GrabMode::None :
                        wants[0] && wants[1]  ? GrabMode::TwoHand :
                        wants[0] || wants[1]  ? GrabMode::OneHand :
                                                GrabMode::None;
  const int one_hand = wants[0] ? 0 : 1;
  if (want == GrabMode::None)
  {
    if (s_grab_mode != GrabMode::None && pl.placed)
    {
      // Log where Lance put it, to tune the defaults.
      NOTICE_LOG_FMT(VIDEO, "HitzWorld: {} moved to ({:.2f},{:.2f},{:.2f}) yaw {:.0f} size 1/{:.1f}",
                     ViewName(view), pl.position[0], pl.position[1], pl.position[2],
                     pl.yaw * 57.2958f, 1.0f / std::max(pl.size, 1e-4f));
    }
    s_grab_mode = GrabMode::None;
    return false;
  }

  const Vec3 l = left.grip_pose.position;
  const Vec3 r = right.grip_pose.position;
  const Vec3 mid{0.5f * (l[0] + r[0]), 0.5f * (l[1] + r[1]), 0.5f * (l[2] + r[2])};
  // Hand-to-hand direction in the room's horizontal plane, measured the way RotY turns vectors:
  // RotY(a) * (sin t, 0, cos t) = (sin(t+a), 0, cos(t+a)).
  const float hx = r[0] - l[0], hz = r[2] - l[2];
  const float flat_len = std::hypot(hx, hz);
  const float angle = std::atan2(hx, hz);
  const float dist = std::max(Length({hx, r[1] - l[1], hz}), 0.05f);

  if (want != s_grab_mode || (want == GrabMode::OneHand && one_hand != s_grab_hand))
  {
    // Re-base on the current pose: switching between one and two hands never jumps.
    s_grab_mode = want;
    s_grab_hand = one_hand;
    s_grab_pos0 = pl.position;
    s_grab_yaw0 = pl.yaw;
    s_grab_size0 = pl.size;
    s_grab_hand0 = hands[one_hand]->grip_pose.position;
    s_grab_mid0 = mid;
    s_grab_angle0 = angle;
    s_grab_dist0 = dist;
  }
  pl.user_moved = true;

  if (s_grab_mode == GrabMode::OneHand)
  {
    // Move only: the table follows the hand (up/down and sideways), never turning.
    const Vec3 hand = hands[s_grab_hand]->grip_pose.position;
    for (int i = 0; i < 3; ++i)
      pl.position[i] = s_grab_pos0[i] + hand[i] - s_grab_hand0[i];
    return true;
  }

  // Two hands: turn about the vertical and resize, both around the midpoint between the hands,
  // which also carries the table along. Hands nearly above each other give no reliable direction:
  // keep the turn as it is then.
  const float dyaw = flat_len > 0.05f ? WrapAngle(angle - s_grab_angle0) : pl.yaw - s_grab_yaw0;
  const float size = std::clamp(s_grab_size0 * dist / s_grab_dist0, 0.002f, 2.0f);
  const float k = size / std::max(s_grab_size0, 1e-6f);
  const Vec3 rel{(s_grab_pos0[0] - s_grab_mid0[0]) * k, (s_grab_pos0[1] - s_grab_mid0[1]) * k,
                 (s_grab_pos0[2] - s_grab_mid0[2]) * k};
  const Vec3 turned = MulVec(RotY(dyaw), rel);
  pl.position = {mid[0] + turned[0], mid[1] + turned[1], mid[2] + turned[2]};
  pl.yaw = s_grab_yaw0 + dyaw;
  pl.size = size;
  return true;
}

bool LeftHandGrabbing()
{
  return s_left_grab.load(std::memory_order_relaxed);
}
}  // namespace HitzWorld
