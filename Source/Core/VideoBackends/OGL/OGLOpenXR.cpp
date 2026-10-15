// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

// The graphics-API and platform macros must be defined before including
// openxr_platform.h, and the native headers each binding needs must come first.
#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <unknwn.h>  // IUnknown, required by openxr_platform.h's WIN32 section
#include "Common/GL/GLInterface/WGL.h"
#elif defined(ANDROID)
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>  // jobject, required by openxr_platform.h's Android section
#include <EGL/egl.h>
#include "Common/GL/GLInterface/EGL.h"
#elif HAVE_X11
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <GL/glx.h>
#include "Common/GL/GLInterface/GLX.h"
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "VideoBackends/OGL/OGLOpenXR.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Assert.h"
#include "Common/Buffer.h"
#include "Common/FileUtil.h"
#include "Common/GL/GLContext.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/Image.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR/HitzWorld.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace OGL
{
std::unique_ptr<OGLOpenXR> g_openxr_gl;

// Defined here so the unique_ptr deleters see complete OGLTexture/OGLFramebuffer types.
XREyeSwapchainGL::XREyeSwapchainGL() = default;
XREyeSwapchainGL::~XREyeSwapchainGL() = default;
XREyeSwapchainGL::XREyeSwapchainGL(XREyeSwapchainGL&&) noexcept = default;
XREyeSwapchainGL& XREyeSwapchainGL::operator=(XREyeSwapchainGL&&) noexcept = default;

namespace
{
AbstractTextureFormat GetAbstractFormatForGLFormat(int64_t format)
{
  switch (format)
  {
  case GL_SRGB8_ALPHA8:
  case GL_RGBA8:
    // Declaring sRGB storage as RGBA8 gives the same semantics as the D3D11
    // MUTABLE_FORMAT/UNORM-RTV trick: BlitFromTexture writes raw (already
    // gamma-encoded) bytes, while the swapchain's declared XR format tells the
    // compositor to decode.
    return AbstractTextureFormat::RGBA8;
  case GL_RGB10_A2:
    return AbstractTextureFormat::RGB10_A2;
  default:
    return AbstractTextureFormat::Undefined;
  }
}
}  // Anonymous namespace

OGLOpenXR::OGLOpenXR() = default;

OGLOpenXR::~OGLOpenXR()
{
  Shutdown();
}

bool OGLOpenXR::Initialize(GLContext* context)
{
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Starting initialization...");
  ASSERT_MSG(VIDEO, !VR::g_openxr, "OpenXRManager already initialized.");

  m_context = context;
  m_gles = context->IsGLES();

#if defined(ANDROID)
  if (!m_gles)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: only GLES contexts are supported on Android.");
    return false;
  }
#else
  if (m_gles)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: GLES on desktop is not supported for OpenXR.");
    return false;
  }
#endif

  // Per-eye rendering uses the geometry-shader gl_Layer path; without geometry
  // shaders (e.g. GLES < 3.2 without AEP) the stereo XFB is never produced.
  if (!g_backend_info.bSupportsGeometryShaders)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: geometry shaders are unavailable (desktop GL 3.2+ or "
                         "OpenGL ES 3.2 required); cannot enable VR.");
    return false;
  }

  auto mgr = std::make_unique<VR::OpenXRManager>();

  // The GL graphics binding extension is mandatory. The name macros only exist under
  // their respective XR_USE_GRAPHICS_API_* gates, so this is a compile-time choice
  // (the m_gles checks above guarantee the context matches).
  // Also enable optional controller profile extensions (Meta, Pico, etc.) when available.
#if defined(ANDROID)
  std::vector<const char*> extensions = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
#else
  std::vector<const char*> extensions = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
#endif
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR: Enabling XR_FB_display_refresh_rate.");
  }
#if defined(ANDROID)
  // Fixed foveated rendering: on GLES the runtime applies the QCOM scaled-bin foveation
  // to the swapchain textures itself, so requesting the extensions is all we need here.
  const auto foveation_exts = VR::OpenXRManager::GetAvailableFoveationExtensions(false);
  extensions.insert(extensions.end(), foveation_exts.begin(), foveation_exts.end());
  // BaseballMR r40: the same Android performance extensions VulkanOpenXR enables. Without them
  // the GL build never registered its CPU/Video threads with the runtime (big-core scheduling)
  // and never requested SUSTAINED_HIGH CPU/GPU levels: Lance's r38 session ran at GPU level 1
  // (350 MHz) where Vulkan got level 3 (492 MHz), and stuttered.
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(
          XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
  {
    extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR GL: Enabling XR_KHR_android_thread_settings.");
  }
  if (VR::OpenXRManager::IsRuntimeExtensionSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    INFO_LOG_FMT(VIDEO, "OpenXR GL: Enabling XR_EXT_performance_settings.");
  }
#endif
  const auto controller_exts = VR::OpenXRManager::GetAvailableControllerExtensions();
  extensions.insert(extensions.end(), controller_exts.begin(), controller_exts.end());
  if (!mgr->CreateInstance(extensions))
    return false;

  if (!mgr->InitializeSystem())
    return false;

  if (!mgr->EnumerateViewConfigurations())
    return false;

  // BaseballMR r36: keep VR passthrough coverage only when the runtime can composite it
  // (XR_FB_passthrough or an ALPHA_BLEND environment), as VulkanOpenXR does.
  g_backend_info.bSupportsVRPassthroughCoverage &= mgr->SupportsPassthrough();

  // Publish the manager globally so CreateSessionGL can access instance/system IDs.
  VR::g_openxr = std::move(mgr);

  if (!CreateSessionGL())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!VR::g_openxr->CreateReferenceSpace())
  {
    VR::g_openxr.reset();
    return false;
  }

  if (!CreateSwapchains())
  {
    VR::g_openxr.reset();
    return false;
  }

  // Register this object as the swapchain provider so Presenter can acquire eye images.
  VR::g_openxr->SetSwapchain(this);

  INFO_LOG_FMT(VIDEO, "OpenXR GL: Initialization complete.");
  return true;
}

void OGLOpenXR::Shutdown()
{
  // Clear swapchain pointer before destroying swapchains so no dangling use occurs.
  if (VR::g_openxr)
    VR::g_openxr->SetSwapchain(nullptr);

  DestroySwapchains();
  VR::g_openxr.reset();
  glFlush();
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Shut down.");
}

bool OGLOpenXR::CreateSessionGL()
{
  ASSERT(m_context != nullptr);
  ASSERT(VR::g_openxr != nullptr);

  const XrInstance instance = VR::g_openxr->GetInstance();
  const XrSystemId system_id = VR::g_openxr->GetSystemId();

  // The runtime requires the graphics-requirements query before xrCreateSession.
#if defined(ANDROID)
  const char* const requirements_fn = "xrGetOpenGLESGraphicsRequirementsKHR";
  PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
  XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
#else
  const char* const requirements_fn = "xrGetOpenGLGraphicsRequirementsKHR";
  PFN_xrGetOpenGLGraphicsRequirementsKHR get_requirements = nullptr;
  XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
#endif
  if (XR_FAILED(xrGetInstanceProcAddr(instance, requirements_fn,
                                      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) ||
      !get_requirements)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Failed to resolve {}.", requirements_fn);
    return false;
  }
  if (XR_FAILED(get_requirements(instance, system_id, &requirements)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: {} failed.", requirements_fn);
    return false;
  }

  // GLExtensions::Version() returns e.g. 460 for GL 4.6 / 320 for GLES 3.2.
  const u32 gl_version = GLExtensions::Version();
  const XrVersion context_version =
      XR_MAKE_VERSION(gl_version / 100, (gl_version % 100) / 10, 0);
  INFO_LOG_FMT(VIDEO, "OpenXR GL: runtime requires {} {}.{} - {}.{}, context is {}.{}.",
               m_gles ? "GLES" : "GL",
               XR_VERSION_MAJOR(requirements.minApiVersionSupported),
               XR_VERSION_MINOR(requirements.minApiVersionSupported),
               XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
               XR_VERSION_MINOR(requirements.maxApiVersionSupported), gl_version / 100,
               (gl_version % 100) / 10);
  if (context_version < requirements.minApiVersionSupported)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: The context version is below the runtime's minimum.");
    return false;
  }

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.systemId = system_id;

#if defined(_WIN32)
  if (m_context->GetNativeContextType() != GLContext::NativeContextType::WGL)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Only WGL contexts are supported on Windows.");
    return false;
  }
  const auto* wgl = static_cast<const GLContextWGL*>(m_context);
  XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  binding.hDC = wgl->GetDCHandle();
  binding.hGLRC = wgl->GetRCHandle();
  session_info.next = &binding;
#elif defined(ANDROID)
  if (m_context->GetNativeContextType() != GLContext::NativeContextType::EGL)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Only EGL contexts are supported on Android.");
    return false;
  }
  const auto* egl = static_cast<const GLContextEGL*>(m_context);
  XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  binding.display = egl->GetEGLDisplay();
  binding.config = egl->GetEGLConfig();
  binding.context = egl->GetEGLContext();
  session_info.next = &binding;
#elif HAVE_X11
  if (m_context->GetNativeContextType() != GLContext::NativeContextType::GLX)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: Only GLX (X11) contexts are supported on Linux for now; "
                         "Wayland/EGL contexts are not yet supported. Run under X11/XWayland "
                         "without the EGL/GLES preference, or use the Vulkan backend.");
    return false;
  }
  const auto* glx = static_cast<const GLContextGLX*>(m_context);
  XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
  binding.xDisplay = glx->GetDisplay();
  binding.glxFBConfig = glx->GetFBConfig();
  binding.glxDrawable = glx->GetDrawable();
  binding.glxContext = glx->GetContext();
  int visual_id = 0;
  glXGetFBConfigAttrib(glx->GetDisplay(), glx->GetFBConfig(), GLX_VISUAL_ID, &visual_id);
  binding.visualid = static_cast<uint32_t>(visual_id);
  session_info.next = &binding;
#else
  ERROR_LOG_FMT(VIDEO, "OpenXR GL: No supported platform binding available in this build.");
  return false;
#endif

  XrSession session = XR_NULL_HANDLE;
  const XrResult result = xrCreateSession(instance, &session_info, &session);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrCreateSession failed ({}).", static_cast<int>(result));
    return false;
  }

  VR::g_openxr->SetSession(session);
  INFO_LOG_FMT(VIDEO, "OpenXR GL: Session created successfully.");
  return true;
}

bool OGLOpenXR::SelectSwapchainFormat(int64_t* out_format) const
{
  const XrSession session = VR::g_openxr->GetSession();

  uint32_t format_count = 0;
  if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr)) ||
      format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrEnumerateSwapchainFormats failed.");
    return false;
  }
  std::vector<int64_t> formats(format_count);
  if (XR_FAILED(
          xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data())))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: xrEnumerateSwapchainFormats failed.");
    return false;
  }

  // Dolphin's output is already gamma-encoded, so an sRGB swapchain is only usable if
  // GL will store our bytes verbatim: always true on desktop GL (GL_FRAMEBUFFER_SRGB is
  // never enabled), but on GLES render-to-sRGB always encodes unless
  // GL_EXT_sRGB_write_control is available to turn it off.
  const bool raw_srgb_writes_ok = !m_gles || g_ogl_config.bSupportsSRGBWriteControl;
  std::array<int64_t, 3> preferred{};
  if (raw_srgb_writes_ok)
    preferred = {GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGB10_A2};
  else
    preferred = {GL_RGBA8, GL_RGB10_A2, GL_SRGB8_ALPHA8};

  for (const int64_t candidate : preferred)
  {
    if (std::find(formats.begin(), formats.end(), candidate) != formats.end())
    {
      if (m_gles && !raw_srgb_writes_ok)
      {
        WARN_LOG_FMT(VIDEO, "OpenXR GL: GL_EXT_sRGB_write_control is unavailable; colors may be "
                            "slightly off (adjust with the VR gamma setting).");
      }
      INFO_LOG_FMT(VIDEO, "OpenXR GL: Using swapchain format {:#x}.",
                   static_cast<uint64_t>(candidate));
      *out_format = candidate;
      return true;
    }
  }

  ERROR_LOG_FMT(VIDEO, "OpenXR GL: No supported swapchain format found ({} offered).",
                format_count);
  return false;
}

bool OGLOpenXR::CreateSwapchains()
{
  ASSERT(VR::g_openxr != nullptr);

  const auto& view_cfgs = VR::g_openxr->GetViewConfigViews();
  int64_t swapchain_format = 0;
  if (!SelectSwapchainFormat(&swapchain_format))
    return false;
  m_swapchain_format = swapchain_format;  // StreetMR VR 360: the backdrop cubes use it too

  const AbstractTextureFormat abstract_format = GetAbstractFormatForGLFormat(swapchain_format);
  if (abstract_format == AbstractTextureFormat::Undefined)
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR GL: No AbstractTextureFormat for swapchain format {:#x}.",
                  static_cast<uint64_t>(swapchain_format));
    return false;
  }

  if (swapchain_format == GL_SRGB8_ALPHA8 && (!m_gles || g_ogl_config.bSupportsSRGBWriteControl))
  {
    // Make sure GL stores our already-gamma-encoded bytes verbatim when rendering
    // into the sRGB swapchain images. Disabled is the desktop default; on GLES this
    // needs GL_EXT_sRGB_write_control (same token value).
    glDisable(GL_FRAMEBUFFER_SRGB);
  }

  // Fixed foveated rendering: only for the stereo projection path — the flat panel quad
  // reuses eye swapchain #0 and a foveated panel would blur its edges for no gain.
  const bool use_foveation = g_ActiveConfig.stereo_mode == StereoMode::OpenXR &&
                             VR::g_openxr->IsFoveationUsable();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];
    sc.width = view_cfgs[eye].recommendedImageRectWidth;
    sc.height = view_cfgs[eye].recommendedImageRectHeight;

    // Format must come from xrEnumerateSwapchainFormats.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.arraySize = 1;
    info.format = swapchain_format;
    info.width = sc.width;
    info.height = sc.height;
    info.mipCount = 1;
    info.faceCount = 1;
    info.sampleCount = 1;
    // We render into this swapchain image as a color target, but never sample from it.
    // Unlike D3D11, no MUTABLE_FORMAT is needed to write raw bytes to sRGB storage:
    // GL only applies the sRGB encode when GL_FRAMEBUFFER_SRGB is enabled (see above).
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

    // GLES foveation uses the Adreno scaled-bin method: the driver renders peripheral
    // bins at reduced resolution and upscales them when the render pass resolves.
    XrSwapchainCreateInfoFoveationFB foveation_info{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    bool foveated = use_foveation;
    if (foveated)
    {
      foveation_info.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
      // XrSwapchainCreateInfoFoveationFB::next is (non-const) void*.
      foveation_info.next = const_cast<void*>(info.next);
      info.next = &foveation_info;
    }

    XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    if (XR_FAILED(result) && foveated)
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: Foveated swapchain creation failed for eye {} ({}); retrying "
                   "without foveation.",
                   eye, static_cast<int>(result));
      info.next = foveation_info.next;
      foveated = false;
      result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc.swapchain);
    }
    if (XR_FAILED(result))
    {
      ERROR_LOG_FMT(VIDEO, "OpenXR: xrCreateSwapchain failed for eye {} ({}).", eye,
                    static_cast<int>(result));
      return false;
    }

    if (foveated)
      VR::g_openxr->ApplyFoveationToSwapchain(sc.swapchain);

    // Enumerate the GL textures backing this swapchain.
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(sc.swapchain, 0, &image_count, nullptr);

#if defined(ANDROID)
    std::vector<XrSwapchainImageOpenGLESKHR> images(image_count,
                                                    {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
    std::vector<XrSwapchainImageOpenGLKHR> images(image_count,
                                                  {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
    xrEnumerateSwapchainImages(sc.swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    sc.textures.resize(image_count);
    sc.framebuffers.resize(image_count);

    const TextureConfig config(sc.width, sc.height, 1, 1, 1, abstract_format,
                               AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);

    for (uint32_t i = 0; i < image_count; ++i)
    {
      // Adopt the runtime-owned GL texture; OGLTexture will not delete it.
      sc.textures[i] = OGLTexture::CreateAdopted(static_cast<GLuint>(images[i].image), config);
      if (!sc.textures[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: OGLTexture::CreateAdopted failed for eye {}, image {}.", eye,
                      i);
        return false;
      }

      // No depth attachment; the eye image only receives the final XFB blit.
      sc.framebuffers[i] = OGLFramebuffer::Create(sc.textures[i].get(), nullptr, {});
      if (!sc.framebuffers[i])
      {
        ERROR_LOG_FMT(VIDEO, "OpenXR: OGLFramebuffer::Create failed for eye {}, image {}.", eye,
                      i);
        return false;
      }
    }

    INFO_LOG_FMT(VIDEO, "OpenXR: Eye {} swapchain ready: {}x{}, {} images.", eye, sc.width,
                 sc.height, image_count);
  }

  return true;
}

void OGLOpenXR::DestroySwapchains()
{
  DestroyBackdrops();  // StreetMR VR 360

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& sc = m_eye_swapchains[eye];

    if (m_image_acquired[eye] && sc.swapchain != XR_NULL_HANDLE)
    {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
      if (XR_FAILED(release_result))
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: xrReleaseSwapchainImage during shutdown failed for eye {} ({}).",
                     eye, static_cast<int>(release_result));
      }
      m_image_acquired[eye] = false;
    }

    // Release Dolphin wrappers before destroying the swapchain so the
    // runtime's textures are only freed after our references are gone.
    sc.framebuffers.clear();
    sc.textures.clear();

    if (sc.swapchain != XR_NULL_HANDLE)
    {
      const XrResult destroy_result = xrDestroySwapchain(sc.swapchain);
      if (XR_FAILED(destroy_result))
      {
        WARN_LOG_FMT(VIDEO, "OpenXR: xrDestroySwapchain failed for eye {} ({}).", eye,
                     static_cast<int>(destroy_result));
      }
      sc.swapchain = XR_NULL_HANDLE;
    }
  }
}

AbstractFramebuffer* OGLOpenXR::AcquireEyeFramebuffer(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  auto& sc = m_eye_swapchains[eye_index];

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acquire_info,
                                        &m_acquired_image_index[eye_index])))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrAcquireSwapchainImage failed for eye {}.", eye_index);
    return nullptr;
  }
  m_image_acquired[eye_index] = true;

  // Block until the acquired image is safe to write.
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.swapchain, &wait_info)))
  {
    ERROR_LOG_FMT(VIDEO, "OpenXR: xrWaitSwapchainImage failed for eye {}.", eye_index);

    // Ensure we don't leak an acquired image if waiting fails.
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult release_result = xrReleaseSwapchainImage(sc.swapchain, &release_info);
    if (XR_FAILED(release_result))
    {
      WARN_LOG_FMT(VIDEO,
                   "OpenXR: xrReleaseSwapchainImage after wait failure failed for eye {} ({}).",
                   eye_index, static_cast<int>(release_result));
    }
    m_image_acquired[eye_index] = false;
    return nullptr;
  }

  return sc.framebuffers[m_acquired_image_index[eye_index]].get();
}

void OGLOpenXR::ReleaseEyeTexture(uint32_t eye_index)
{
  ASSERT(eye_index < 2);
  if (!m_image_acquired[eye_index])
    return;

  // Per XR_KHR_opengl_enable the runtime is responsible for GL synchronization at
  // release time, but some runtimes' GL paths have historically missed pending work;
  // one flush per eye per frame is cheap insurance.
  glFlush();

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result =
      xrReleaseSwapchainImage(m_eye_swapchains[eye_index].swapchain, &release_info);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "OpenXR: xrReleaseSwapchainImage failed for eye {} ({}).", eye_index,
                 static_cast<int>(result));
  }
  m_image_acquired[eye_index] = false;
}

bool OGLOpenXR::SubmitFrame()
{
  ASSERT(VR::g_openxr != nullptr);

  // Use the pose the presented XFB was rendered with (stamped at its XFB copy and
  // selected in FetchXFB). Live m_eye_views — or even the live submit snapshot — can
  // already belong to the NEXT frame when presentation is deferred to VI time, and a
  // pose/content mismatch makes ATW reproject the frame to the wrong place.
  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    auto& pv = m_projection_views[eye];
    pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    pv.pose = eye_views[eye].pose;
    pv.fov = eye_views[eye].fov;
    pv.subImage.swapchain = m_eye_swapchains[eye].swapchain;
    pv.subImage.imageArrayIndex = 0;
    pv.subImage.imageRect = {{0, 0},
                             {static_cast<int32_t>(m_eye_swapchains[eye].width),
                              static_cast<int32_t>(m_eye_swapchains[eye].height)}};
  }

  m_projection_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  m_projection_layer.space = VR::g_openxr->GetReferenceSpace();
  m_projection_layer.viewCount = 2;
  m_projection_layer.views = m_projection_views.data();
  m_projection_layer.layerFlags = VR::g_openxr->GetProjectionLayerExtraFlags();

  const std::vector<XrCompositionLayerBaseHeader*> layers = {
      reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projection_layer)};

  return VR::g_openxr->EndFrame(layers);
}

// ---- StreetMR VR 360 ----

namespace
{
std::optional<OGLOpenXR::Panorama> DecodePanorama(const std::string& path)
{
  std::string file;
  if (!File::ReadFileToString(path, file))
    return std::nullopt;
  Common::UniqueBuffer<u8> rgba;
  u32 width = 0, height = 0;
  if (!Common::LoadPNG(std::span(reinterpret_cast<const u8*>(file.data()), file.size()), &rgba,
                       &width, &height) ||
      height == 0 || height > 4096 || width != 6 * height)
  {
    return std::nullopt;
  }
  OGLOpenXR::Panorama p;
  p.face = height;
  p.rgba.assign(rgba.data(), rgba.data() + static_cast<size_t>(width) * height * 4);
  return p;
}

// The direction a cube texel faces (GL cube map convention, which the cube layer samples with):
// face 0..5 = +X -X +Y -Y +Z -Z; s, t in [-1, 1] along the face's row and column.
std::array<float, 3> CubeDirection(int face, float s, float t)
{
  switch (face)
  {
  case 0:
    return {1.0f, -t, -s};
  case 1:
    return {-1.0f, -t, s};
  case 2:
    return {s, 1.0f, t};
  case 3:
    return {s, -1.0f, -t};
  case 4:
    return {s, -t, 1.0f};
  default:
    return {-s, -t, -1.0f};
  }
}

// r35j: the searchlight beam (it used to sit in the old sprite atlas next to the cartoon airliner,
// helicopter and birds, all gone - Lance: only Hoop City and the gradients). A soft cool-white
// column, brightest at its lamp (the last rows), premultiplied for the compositor; same shape as
// the atlas one (alpha 138 at the lamp, a 4-pixel Gaussian across, rising as v^1.57).
std::vector<u8> MakeBeamSprite(u32 w, u32 h)
{
  std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
  const float colour[3] = {215, 232, 255};
  const float centre = 0.5f * static_cast<float>(w) - 0.5f;
  for (u32 y = 0; y < h; ++y)
  {
    const float along = std::pow((y + 0.5f) / static_cast<float>(h), 1.57f);
    for (u32 x = 0; x < w; ++x)
    {
      const float dx = static_cast<float>(x) - centre;
      const float alpha = 138.0f * along * std::exp(-dx * dx / 35.0f);
      u8* px = &rgba[(static_cast<size_t>(y) * w + x) * 4];
      for (int c = 0; c < 3; ++c)
        px[c] = static_cast<u8>(std::clamp(colour[c] * alpha / 255.0f + 0.5f, 0.0f, 255.0f));
      px[3] = static_cast<u8>(std::clamp(alpha + 0.5f, 0.0f, 255.0f));
    }
  }
  return rgba;
}

// StreetMR r35c (Lance: "gradient colored scenes so if people dont want the vegas scenery, they
// can choose the plain gradient color background"): a plain colour gradient all round, picked on
// the start screen ("streetmr_gradient_<name>"). Overhead, sky, horizon and floor colours (sRGB),
// blended in linear light by height, with a little noise so 8 bits don't band.
std::vector<u8> MakeGradientCube(u32 n, std::string_view name)
{
  struct Palette
  {
    std::string_view name;
    float top[3], sky[3], horizon[3], floor[3];
  };
  static constexpr Palette PALETTES[] = {
      {"sunset", {28, 24, 72}, {120, 50, 120}, {255, 150, 90}, {40, 24, 40}},
      {"ocean", {8, 30, 70}, {20, 90, 150}, {90, 200, 220}, {8, 24, 40}},
      {"purple", {24, 8, 48}, {90, 30, 140}, {230, 110, 220}, {20, 8, 30}},
      {"midnight", {4, 4, 14}, {14, 18, 48}, {50, 60, 120}, {4, 4, 10}},
      {"mint", {16, 70, 80}, {60, 160, 150}, {190, 245, 220}, {14, 50, 50}},
      {"fire", {40, 6, 10}, {150, 30, 20}, {255, 150, 40}, {30, 8, 8}},
      {"gray", {40, 40, 46}, {90, 90, 98}, {170, 170, 178}, {30, 30, 34}},
  };
  const Palette* p = &PALETTES[0];
  for (const Palette& candidate : PALETTES)
  {
    if (candidate.name == name)
      p = &candidate;
  }
  const auto lin = [](float c) { return std::pow(c / 255.0f, 2.2f); };
  const auto enc = [](float c) { return 255.0f * std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f); };
  const auto smooth = [](float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
  };
  std::vector<u8> rgba(static_cast<size_t>(6) * n * n * 4);
  u32 noise = 0x12345678u;
  for (int f = 0; f < 6; ++f)
  {
    for (u32 j = 0; j < n; ++j)
    {
      for (u32 i = 0; i < n; ++i)
      {
        const auto d = CubeDirection(f, (i + 0.5f) / n * 2.0f - 1.0f, (j + 0.5f) / n * 2.0f - 1.0f);
        const float y = d[1] / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        noise = noise * 1664525u + 1013904223u;
        const float dither = (static_cast<float>(noise >> 8) / 16777216.0f - 0.5f);
        u8* px = &rgba[((static_cast<size_t>(j) * 6 * n) + f * n + i) * 4];
        for (int c = 0; c < 3; ++c)
        {
          float v;
          if (y >= 0.0f)
          {
            const float a = smooth(y / 0.35f);  // horizon -> sky
            const float b = smooth((y - 0.35f) / 0.65f);  // sky -> overhead
            v = lin(p->horizon[c]) + (lin(p->sky[c]) - lin(p->horizon[c])) * a;
            v = v + (lin(p->top[c]) - v) * b;
          }
          else
          {
            v = lin(p->horizon[c]) + (lin(p->floor[c]) - lin(p->horizon[c])) * smooth(-y / 0.25f);
          }
          px[c] = static_cast<u8>(std::clamp(enc(v) + dither, 0.0f, 255.0f));
        }
        px[3] = 255;
      }
    }
  }
  return rgba;
}
}  // namespace

XrSwapchain OGLOpenXR::CreateCubeSwapchain(u32 face, u32* out_image)
{
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  // As Meta's cube-layer sample: a static, sampled image filled once with glTexSubImage2D.
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  info.format = m_swapchain_format;
  info.sampleCount = 1;
  info.width = face;
  info.height = face;
  info.faceCount = 6;
  info.arraySize = 1;
  info.mipCount = 1;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  XrResult result = xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &swapchain);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "VR 360: cube swapchain {}x{} failed ({}).", face, face,
                  static_cast<int>(result));
    return XR_NULL_HANDLE;
  }

  uint32_t image_count = 0;
  xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
#if defined(ANDROID)
  std::vector<XrSwapchainImageOpenGLESKHR> images(image_count,
                                                  {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
  std::vector<XrSwapchainImageOpenGLKHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
  xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  if (image_count == 0 || XR_FAILED(xrAcquireSwapchainImage(swapchain, &acquire_info, &index)) ||
      XR_FAILED(xrWaitSwapchainImage(swapchain, &wait_info)) || index >= image_count)
  {
    ERROR_LOG_FMT(VIDEO, "VR 360: cube swapchain image unavailable.");
    xrDestroySwapchain(swapchain);
    return XR_NULL_HANDLE;
  }
  *out_image = static_cast<u32>(images[index].image);
  return swapchain;
}

void OGLOpenXR::UploadCubeFaces(u32 image, const u8* rgba, u32 face, u32 first, u32 last,
                                u32 row0, u32 rows)
{
  if (rows == 0 || row0 + rows > face)
    rows = face - std::min(row0, face);
  // Straight from the side-by-side strip, leaving Dolphin's GL state as it was (unpack buffer,
  // alignment, the cube binding).
  GLint prev_unpack_buffer = 0, prev_alignment = 4, prev_cube = 0;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_unpack_buffer);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
  glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &prev_cube);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glBindTexture(GL_TEXTURE_CUBE_MAP, static_cast<GLuint>(image));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(6 * face));
  glPixelStorei(GL_UNPACK_SKIP_ROWS, static_cast<GLint>(row0));
  for (u32 f = first; f < last && f < 6; ++f)
  {
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, static_cast<GLint>(f * face));
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, static_cast<GLint>(row0),
                    static_cast<GLsizei>(face), static_cast<GLsizei>(rows), GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba);
  }
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
  glBindTexture(GL_TEXTURE_CUBE_MAP, static_cast<GLuint>(prev_cube));
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prev_unpack_buffer));
}

void OGLOpenXR::FinishCubeSwapchain(XrSwapchain swapchain)
{
  glFlush();
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(swapchain, &release_info);
}

void OGLOpenXR::DestroyBackdrops()
{
  if (m_backdrop_load.valid())
    m_backdrop_load.wait();
  m_backdrop_load = {};
  for (auto& [key, sc] : m_backdrop_kept)
  {
    if (sc == m_backdrop_court)
      m_backdrop_court = XR_NULL_HANDLE;
    if (sc != XR_NULL_HANDLE)
      xrDestroySwapchain(sc);
  }
  m_backdrop_kept.clear();
  m_backdrop_key.clear();
  if (m_lights_load.valid())
    m_lights_load.wait();
  m_lights_load = {};
  m_lights_pending = {};
  m_lights_key.clear();
  for (auto& [key, cubes] : m_backdrop_lights)
  {
    for (XrSwapchain sc : cubes)
    {
      if (sc != XR_NULL_HANDLE)
        xrDestroySwapchain(sc);
    }
  }
  m_backdrop_lights.clear();
  m_backdrop_lights_tried.clear();
  if (m_ambient_atlas != XR_NULL_HANDLE)
    xrDestroySwapchain(m_ambient_atlas);
  m_ambient_atlas = XR_NULL_HANDLE;
  m_ambient_tried = false;
  for (XrSwapchain* sc :
       {&m_backdrop_court, &m_backdrop_court_sky, &m_backdrop_sky, &m_backdrop_filling})
  {
    if (*sc != XR_NULL_HANDLE)
      xrDestroySwapchain(*sc);
    *sc = XR_NULL_HANDLE;
  }
  m_backdrop_fill_data.reset();
  m_backdrop_fill_face = 0;
  m_backdrop_sky_failed = false;
  m_backdrop_gen = 0;
  HitzWorld::SetCourtPanoramaReady(false);
}

void OGLOpenXR::PollBackdrop()
{
  const u32 gen = HitzWorld::CourtKeyGeneration();
  if (gen != m_backdrop_gen)
  {
    m_backdrop_gen = gen;
    // A kept pre-match environment stays alive for next time; anything else goes.
    const auto kept = m_backdrop_kept.find(m_backdrop_key);
    if (kept != m_backdrop_kept.end() && kept->second == m_backdrop_court)
      m_backdrop_court = XR_NULL_HANDLE;
    for (XrSwapchain* sc : {&m_backdrop_court, &m_backdrop_court_sky, &m_backdrop_filling})
    {
      if (*sc != XR_NULL_HANDLE)
        xrDestroySwapchain(*sc);
      *sc = XR_NULL_HANDLE;
    }
    m_backdrop_fill_data.reset();
    HitzWorld::SetCourtPanoramaReady(false);
    m_backdrop_load = {};  // waits for a load still running for the previous court
    const std::string key = HitzWorld::CourtKey();
    m_backdrop_key = key;
    const std::string base = File::GetUserPath(D_LOAD_IDX) + "StreetVR360/" + key;
    if (const auto again = m_backdrop_kept.find(key); again != m_backdrop_kept.end())
    {
      m_backdrop_court = again->second;
      HitzWorld::SetCourtPanoramaReady(true);
      NOTICE_LOG_FMT(VIDEO, "VR 360: {} (kept)", key);
    }
    else if (key.rfind("streetmr_gradient_", 0) == 0)
    {
      // r35c: a plain gradient, made here (a few ms at 512 px faces), kept like the scenes.
      constexpr u32 n = 512;
      const std::vector<u8> cube = MakeGradientCube(n, std::string_view(key).substr(18));
      u32 image = 0;
      m_backdrop_court = CreateCubeSwapchain(n, &image);
      if (m_backdrop_court != XR_NULL_HANDLE)
      {
        UploadCubeFaces(image, cube.data(), n, 0, 6);
        FinishCubeSwapchain(m_backdrop_court);
        m_backdrop_kept[key] = m_backdrop_court;
        HitzWorld::SetCourtPanoramaReady(true);
      }
      NOTICE_LOG_FMT(VIDEO, "VR 360: {}", key);
    }
    else if (!key.empty() && (File::Exists(base + ".png") || File::Exists(base + "_sky.png")))
    {
      m_backdrop_load = std::async(std::launch::async, [base] {
        CourtPictures p;
        if (File::Exists(base + "_sky.png"))
          p.sky = DecodePanorama(base + "_sky.png");
        if (File::Exists(base + ".png"))
          p.court = DecodePanorama(base + ".png");
        return p;
      });
      NOTICE_LOG_FMT(VIDEO, "VR 360: loading the pictures of court {}", key);
    }
    else if (!key.empty())
    {
      NOTICE_LOG_FMT(VIDEO, "VR 360: no pictures for court {} yet", key);
    }
  }

  // Decoded: the calm sky (small) goes up at once; the whole court (16 MB per 2048 px face) fills a
  // few rows per frame below.
  if (m_backdrop_load.valid() &&
      m_backdrop_load.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    CourtPictures pictures = m_backdrop_load.get();
    m_backdrop_load = {};
    if (pictures.sky)
    {
      u32 image = 0;
      m_backdrop_court_sky = CreateCubeSwapchain(pictures.sky->face, &image);
      if (m_backdrop_court_sky != XR_NULL_HANDLE)
      {
        UploadCubeFaces(image, pictures.sky->rgba.data(), pictures.sky->face, 0, 6);
        FinishCubeSwapchain(m_backdrop_court_sky);
      }
    }
    m_backdrop_fill_data = std::move(pictures.court);
    if (m_backdrop_fill_data)
    {
      m_backdrop_filling = CreateCubeSwapchain(m_backdrop_fill_data->face, &m_backdrop_fill_image);
      m_backdrop_fill_face = 0;
      if (m_backdrop_filling == XR_NULL_HANDLE)
        m_backdrop_fill_data.reset();
    }
    NOTICE_LOG_FMT(VIDEO, "VR 360: court sky {}, whole court {}",
                   m_backdrop_court_sky != XR_NULL_HANDLE ? "ready" : "missing",
                   m_backdrop_filling != XR_NULL_HANDLE ? "filling" : "missing");
    HitzWorld::SetCourtPanoramaReady(m_backdrop_court_sky != XR_NULL_HANDLE);
  }
  // A quarter-million pixels (1 MB) per frame: 48 frames for 2048 px faces, no hitch.
  if (m_backdrop_filling != XR_NULL_HANDLE && m_backdrop_fill_data)
  {
    const u32 face = m_backdrop_fill_data->face;
    const u32 rows = std::max<u32>(1, (256u * 1024u) / face);
    const u32 face_index = m_backdrop_fill_face / face, row0 = m_backdrop_fill_face % face;
    UploadCubeFaces(m_backdrop_fill_image, m_backdrop_fill_data->rgba.data(), face, face_index,
                    face_index + 1, row0, std::min(rows, face - row0));
    m_backdrop_fill_face += std::min(rows, face - row0);
    if (m_backdrop_fill_face >= 6 * face)
    {
      FinishCubeSwapchain(m_backdrop_filling);
      m_backdrop_court = m_backdrop_filling;
      m_backdrop_filling = XR_NULL_HANDLE;
      if (m_backdrop_key.rfind("streetmr_", 0) == 0)
        m_backdrop_kept[m_backdrop_key] = m_backdrop_court;
      NOTICE_LOG_FMT(VIDEO, "VR 360: court picture ready ({} px faces)", m_backdrop_fill_data->face);
      m_backdrop_fill_data.reset();
      HitzWorld::SetCourtPanoramaReady(true);
    }
    return;  // light groups wait until the scene itself is up
  }
  PollBackdropLights();
}

void OGLOpenXR::LoadAmbient()
{
  m_ambient_tried = true;
  // r35j: only the searchlight beam is left, made here (no picture file).
  constexpr u32 w = 64, h = 128;
  const std::vector<u8> atlas = MakeBeamSprite(w, h);
  {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = m_swapchain_format;
    info.sampleCount = 1;
    info.width = w;
    info.height = h;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrSwapchain sc = XR_NULL_HANDLE;
    if (XR_SUCCEEDED(xrCreateSwapchain(VR::g_openxr->GetSession(), &info, &sc)))
    {
      uint32_t count = 0;
      xrEnumerateSwapchainImages(sc, 0, &count, nullptr);
#if defined(ANDROID)
      std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
      std::vector<XrSwapchainImageOpenGLKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
      xrEnumerateSwapchainImages(sc, count, &count,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait_info.timeout = XR_INFINITE_DURATION;
      if (count > 0 && XR_SUCCEEDED(xrAcquireSwapchainImage(sc, &acquire_info, &index)) &&
          XR_SUCCEEDED(xrWaitSwapchainImage(sc, &wait_info)) && index < count)
      {
        GLint prev_unpack_buffer = 0, prev_alignment = 4, prev_tex = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_unpack_buffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(images[index].image));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                        GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prev_unpack_buffer));
        FinishCubeSwapchain(sc);
        m_ambient_atlas = sc;
      }
      else
      {
        xrDestroySwapchain(sc);
      }
    }
  }

  NOTICE_LOG_FMT(VIDEO, "VR 360: searchlight beam {}",
                 m_ambient_atlas != XR_NULL_HANDLE ? "ready" : "missing");
}

void OGLOpenXR::PollBackdropLights()
{
  // Decoded: one cube a frame to the GPU (6 x 768^2 texels, a few ms), no hitch.
  if (!m_lights_key.empty() && !m_lights_load.valid())
  {
    LightCubes& cubes = m_backdrop_lights[m_lights_key];
    for (size_t i = 0; i < m_lights_pending.size(); ++i)
    {
      if (!m_lights_pending[i])
        continue;
      Panorama& p = *m_lights_pending[i];
      // Light only: the compositor adds it (premultiplied colour, alpha 0).
      for (size_t k = 3; k < p.rgba.size(); k += 4)
        p.rgba[k] = 0;
      u32 image = 0;
      cubes[i] = CreateCubeSwapchain(p.face, &image);
      if (cubes[i] != XR_NULL_HANDLE)
      {
        UploadCubeFaces(image, p.rgba.data(), p.face, 0, 6);
        FinishCubeSwapchain(cubes[i]);
      }
      m_lights_pending[i].reset();
      return;
    }
    NOTICE_LOG_FMT(VIDEO, "VR 360: light groups of {} ready", m_lights_key);
    m_lights_key.clear();
  }
  if (m_lights_load.valid())
  {
    if (m_lights_load.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      m_lights_pending = m_lights_load.get();
    return;
  }
  // Only the scene on show keeps its light groups (2K scenes, r35c: ~24 MB a group); another
  // scene's go, and load again on its next visit.
  const std::string& key = m_backdrop_key;
  for (auto it = m_backdrop_lights.begin(); it != m_backdrop_lights.end();)
  {
    if (it->first == key)
    {
      ++it;
      continue;
    }
    for (XrSwapchain sc : it->second)
    {
      if (sc != XR_NULL_HANDLE)
        xrDestroySwapchain(sc);
    }
    m_backdrop_lights_tried.erase(it->first);
    it = m_backdrop_lights.erase(it);
  }
  // A pre-match scene with light groups not loaded yet: decode them off this thread.
  if (key.rfind("streetmr_", 0) != 0 || m_backdrop_lights.contains(key) ||
      m_backdrop_lights_tried.contains(key))
  {
    return;
  }
  m_backdrop_lights_tried.insert(key);
  const std::string base = File::GetUserPath(D_LOAD_IDX) + "StreetVR360/" + key + "_lights";
  if (!File::Exists(base + "1.png"))
    return;
  m_lights_key = key;
  m_lights_load = std::async(std::launch::async, [base] {
    std::array<std::optional<Panorama>, 6> out;
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = DecodePanorama(base + std::to_string(i + 1) + ".png");
    return out;
  });
  NOTICE_LOG_FMT(VIDEO, "VR 360: loading the light groups of {}", key);
}

std::array<XrSwapchain, 6> OGLOpenXR::GetBackdropLights()
{
  const auto it = m_backdrop_lights.find(m_backdrop_key);
  // Only once all of a scene's groups are up (a half-loaded set would chase with gaps).
  if (it == m_backdrop_lights.end() || m_lights_key == m_backdrop_key)
    return {};
  return it->second;
}

XrSwapchain OGLOpenXR::GetAmbientAtlas()
{
  if (!m_ambient_tried)
    LoadAmbient();
  return m_ambient_atlas;
}

// r35j (Lance: "delete every picture/backdrop that aint the gradient ones or the vegas one"): the
// scene on show; while it loads, a Hoop City scene that is already up; before either, the midnight
// gradient (the old plain blue sky is gone). Same in every view.
XrSwapchain OGLOpenXR::GetBackdropSwapchain(bool)
{
  if (m_backdrop_court != XR_NULL_HANDLE)
    return m_backdrop_court;
  for (const char* key : {"streetmr_menu", "streetmr_loading"})
  {
    const auto it = m_backdrop_kept.find(key);
    if (it != m_backdrop_kept.end() && it->second != XR_NULL_HANDLE)
      return it->second;
  }
  if (m_backdrop_sky == XR_NULL_HANDLE && !m_backdrop_sky_failed)
  {
    constexpr u32 n = 256;
    const std::vector<u8> cube = MakeGradientCube(n, "midnight");
    u32 image = 0;
    m_backdrop_sky = CreateCubeSwapchain(n, &image);
    if (m_backdrop_sky != XR_NULL_HANDLE)
    {
      UploadCubeFaces(image, cube.data(), n, 0, 6);
      FinishCubeSwapchain(m_backdrop_sky);
    }
    m_backdrop_sky_failed = m_backdrop_sky == XR_NULL_HANDLE;
  }
  return m_backdrop_sky;
}

}  // namespace OGL

#endif  // ENABLE_VR
