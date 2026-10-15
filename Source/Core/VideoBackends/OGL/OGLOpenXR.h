// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if defined(ANDROID)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <openxr/openxr.h>

#include "VideoCommon/VR/OpenXRManager.h"

class GLContext;

namespace OGL
{
class OGLTexture;
class OGLFramebuffer;

// Holds the swapchain images for one eye.
// The GL texture names are owned by the OpenXR runtime; OGLTexture adopts them
// (without taking ownership) and OGLFramebuffer provides the FBO for rendering.
// Special members are out-of-line so the unique_ptr deleters instantiate in the
// .cpp where OGLTexture/OGLFramebuffer are complete types.
struct XREyeSwapchainGL
{
  XREyeSwapchainGL();
  ~XREyeSwapchainGL();
  XREyeSwapchainGL(XREyeSwapchainGL&&) noexcept;
  XREyeSwapchainGL& operator=(XREyeSwapchainGL&&) noexcept;

  XREyeSwapchainGL(const XREyeSwapchainGL&) = delete;
  XREyeSwapchainGL& operator=(const XREyeSwapchainGL&) = delete;

  XrSwapchain swapchain = XR_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;

  // One entry per swapchain image.
  std::vector<std::unique_ptr<OGLTexture>> textures;
  std::vector<std::unique_ptr<OGLFramebuffer>> framebuffers;
};

// OpenGL/GLES-specific OpenXR backend. Implements VR::IOpenXRSwapchain so that
// Presenter::RenderXFBToScreen() can acquire/release eye images and submit
// frames using only VideoCommon-visible types (AbstractFramebuffer*).
//
// Unlike Vulkan, the GL context is single-threaded on the GPU thread (like D3D11),
// so no queue locking or asynchronous frame finalization is needed. All methods
// (including Initialize/Shutdown) must be called with the main GL context current.
class OGLOpenXR : public VR::IOpenXRSwapchain
{
public:
  OGLOpenXR();
  ~OGLOpenXR() override;

  OGLOpenXR(const OGLOpenXR&) = delete;
  OGLOpenXR& operator=(const OGLOpenXR&) = delete;

  // StreetMR r35m: GLES may use the XR pacing thread. Unlike XR_KHR_opengl_enable, the GLES
  // binding (XR_KHR_opengl_es_enable) doesn't require the app's context to be current for
  // xrWaitFrame / xrBeginFrame / xrEndFrame, so they run on the pacing thread at the display rate
  // while every GL call stays on the render thread: swapchain image acquire / blit / release (as
  // before) and the VR 360 backdrop (BackdropNeedsRenderThread). [VR] UseXRPacingThread = False
  // brings back the old inline flow (one XR frame per game frame: 60 on 72 Hz, Prd ~50 ms).
  bool SupportsDetachedFrameLoop() const override { return true; }
  bool BackdropNeedsRenderThread() const override { return true; }
  std::unique_lock<std::mutex> LockImageRelease() override;
  void WaitForReleasedImages(bool locked) override;

  // Full initialization: creates XrInstance + system, GL-bound XrSession,
  // reference space, and per-eye swapchains.
  // context must be the main GL context, current on the calling (GPU) thread.
  bool Initialize(GLContext* context);

  // Tears down swapchains and resets g_openxr.
  void Shutdown();

  // ---- IOpenXRSwapchain ----

  // Acquire the next swapchain image for the given eye.
  // Returns AbstractFramebuffer* (actually OGLFramebuffer*) to render into.
  AbstractFramebuffer* AcquireEyeFramebuffer(uint32_t eye_index) override;

  // Release the current swapchain image back to the runtime.
  void ReleaseEyeTexture(uint32_t eye_index) override;

  // Build the XrCompositionLayerProjection and call xrEndFrame.
  bool SubmitFrame() override;

  uint32_t GetEyeWidth() const override { return m_eye_swapchains[0].width; }
  uint32_t GetEyeHeight() const override { return m_eye_swapchains[0].height; }

  // Flat mono panel path reuses eye swapchain #0; the base class handles acquire/release/submit.
  // StreetMR r35n: with the pacing thread the big screen alternates between the two eye
  // swapchains (both idle in flat mode), each release fenced; the pacing thread shows whichever
  // holds a finished image (PickReadyFlatSwapchain) instead of waiting for the GPU. Inline flow:
  // eye 0 only, as before.
  XrSwapchain GetFlatSwapchain() const override
  {
    return m_eye_swapchains[m_flat_last].swapchain;
  }
  AbstractFramebuffer* AcquireFlatFramebuffer() override;
  void ReleaseFlatTexture() override;
  XrSwapchain PickReadyFlatSwapchain(XrSwapchain requested) override;
  std::string TakeFlatPickStats() override;

  // StreetMR VR 360: the current court's pictures (files/user/Load/StreetVR360/<court>.png = the
  // whole court, <court>_sky.png = its calm sky; six square faces side by side in GL order
  // +X -X +Y -Y +Z -Z) as static cube swapchains, decoded off the video thread. r35j: only Hoop
  // City (streetmr_menu / streetmr_loading) and the gradients are shown.
  bool SupportsBackdrop() const override { return true; }
  void PollBackdrop() override;
  XrSwapchain GetBackdropSwapchain(bool whole_court) override;
  XrSwapchain GetAmbientAtlas() override;
  std::array<XrSwapchain, 6> GetBackdropLights() override;

  struct Panorama
  {
    std::vector<u8> rgba;
    u32 face = 0;
  };
  struct CourtPictures
  {
    std::optional<Panorama> court;
    std::optional<Panorama> sky;
  };

private:
  // Creates XrSession with the platform-specific GL graphics binding
  // (Win32/WGL, Xlib/GLX or Android/EGL).
  bool CreateSessionGL();

  // Picks a swapchain format from xrEnumerateSwapchainFormats (GL internalformat enums).
  bool SelectSwapchainFormat(int64_t* out_format) const;

  // Allocates m_eye_swapchains and wraps images as OGLTexture / OGLFramebuffer.
  bool CreateSwapchains();

  void DestroySwapchains();

  // A static cube swapchain, its image acquired for filling (*image = the GL cube texture);
  // XR_NULL_HANDLE on failure.
  XrSwapchain CreateCubeSwapchain(u32 face, u32* image);
  // Fills faces [first, last) of the cube texture from a side-by-side strip, rows [row0, row0 +
  // rows) of each (default: all).
  void UploadCubeFaces(u32 image, const u8* rgba, u32 face, u32 first, u32 last, u32 row0 = 0,
                       u32 rows = 0);
  // Releases the filled image; the swapchain is then ready for the cube layer.
  void FinishCubeSwapchain(XrSwapchain swapchain);
  void DestroyBackdrops();

  // VR 360 backdrop (video thread).
  int64_t m_swapchain_format = 0;
  XrSwapchain m_backdrop_court = XR_NULL_HANDLE;      // the whole court, ready to show
  XrSwapchain m_backdrop_court_sky = XR_NULL_HANDLE;  // the court's calm sky
  XrSwapchain m_backdrop_sky = XR_NULL_HANDLE;        // midnight gradient, until a scene is up
  bool m_backdrop_sky_failed = false;
  u32 m_backdrop_gen = 0;
  std::future<CourtPictures> m_backdrop_load;
  // The whole-court picture being filled a few rows per frame (no hitch at match start).
  XrSwapchain m_backdrop_filling = XR_NULL_HANDLE;
  u32 m_backdrop_fill_image = 0;
  u32 m_backdrop_fill_face = 0;  // rows filled so far, over all six faces
  std::optional<Panorama> m_backdrop_fill_data;
  // StreetMR r31: the pre-match environments ("streetmr_*" keys) stay loaded once filled, so
  // menus <-> loading screens swap without decoding them again.
  std::string m_backdrop_key;
  std::map<std::string, XrSwapchain> m_backdrop_kept;
  // StreetMR r32: the pre-match scenes' moving things (r35j: just the searchlight beam, made on
  // first use, kept).
  XrSwapchain m_ambient_atlas = XR_NULL_HANDLE;
  bool m_ambient_tried = false;
  void LoadAmbient();
  // StreetMR r35: the pre-match scenes' light groups (kept per key, like the scenes): decoded off
  // the video thread, then (r35k) filled a few rows per frame, one cube after the other.
  using LightCubes = std::array<XrSwapchain, 6>;
  std::map<std::string, LightCubes> m_backdrop_lights;
  std::set<std::string> m_backdrop_lights_tried;
  std::string m_lights_key;  // the key being decoded / uploaded
  std::future<std::array<std::optional<Panorama>, 6>> m_lights_load;
  std::array<std::optional<Panorama>, 6> m_lights_pending;
  size_t m_lights_fill_group = 0;  // the group being filled
  XrSwapchain m_lights_filling = XR_NULL_HANDLE;
  u32 m_lights_fill_image = 0;
  u32 m_lights_fill_rows = 0;  // rows filled so far, over all six faces
  void PollBackdropLights();

  // r35m: the newest image release's EGL fence, waited for by the pacing thread before
  // xrEndFrame (see WaitForReleasedImages). Guarded by m_release_mutex.
  std::mutex m_release_mutex;
  void PutDownReleaseFence();
  // r35n: the big screen's two swapchains (eye 0 / eye 1). m_flat_current: acquired now;
  // m_flat_last: released last. Per swapchain (m_release_mutex): released at least once since
  // flat mode started, a release counter, and the fence of its newest image (none = finished).
  uint32_t m_flat_current = 0;
  uint32_t m_flat_last = 0;
  std::array<bool, 2> m_flat_released{false, false};
  std::array<u64, 2> m_flat_serial{0, 0};
  u64 m_flat_counter = 0;
  // r35n: PickReadyFlatSwapchain outcomes since the last TakeFlatPickStats (m_release_mutex).
  u32 m_pick_newest = 0, m_pick_older = 0, m_pick_waited = 0, m_pick_late = 0;
  bool FlatImageFinishedLocked(uint32_t index, u64 wait_ns);
#if defined(ANDROID)
  bool LoadEglFenceFunctions();
  bool m_egl_fence_tried = false;
  PFNEGLCREATESYNCKHRPROC m_egl_create_sync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC m_egl_destroy_sync = nullptr;
  PFNEGLCLIENTWAITSYNCKHRPROC m_egl_client_wait_sync = nullptr;
  EGLSyncKHR m_release_sync = EGL_NO_SYNC_KHR;
  EGLDisplay m_release_sync_display = EGL_NO_DISPLAY;
  std::array<EGLSyncKHR, 2> m_flat_sync{EGL_NO_SYNC_KHR, EGL_NO_SYNC_KHR};
  EGLDisplay m_flat_sync_display = EGL_NO_DISPLAY;
#endif

  // Non-owning; the context is owned by OGLGfx and outlives this object.
  GLContext* m_context = nullptr;
  bool m_gles = false;

  std::array<XREyeSwapchainGL, 2> m_eye_swapchains{};

  // Image index selected by xrAcquireSwapchainImage for the current frame.
  std::array<uint32_t, 2> m_acquired_image_index{0, 0};
  std::array<bool, 2> m_image_acquired{false, false};

  // Reused per-frame composition data (avoids per-frame heap allocation).
  std::array<XrCompositionLayerProjectionView, 2> m_projection_views{};
  XrCompositionLayerProjection m_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

// Global OpenGL OpenXR instance — valid between VideoBackend::Initialize() and Shutdown().
extern std::unique_ptr<OGLOpenXR> g_openxr_gl;

}  // namespace OGL

#endif  // ENABLE_VR
