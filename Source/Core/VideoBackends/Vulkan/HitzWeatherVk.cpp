// Copyright 2026 HitzMR
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/HitzWeatherVk.h"

#ifdef ENABLE_VR

// VulkanOpenXR.h first: it pulls in the Vulkan loader (VK_NO_PROTOTYPES) and the OpenXR
// platform headers with XR_USE_GRAPHICS_API_VULKAN.
#include "VideoBackends/Vulkan/VulkanOpenXR.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/VR/HitzModes.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace Vulkan::HitzWeather
{
namespace
{
constexpr u32 MAX_PARTICLES = 3000;
constexpr u32 RING = 3;
// The particle field is a box around the head, in metres.
constexpr float FIELD_RADIUS = 7.0f;
constexpr float FIELD_ABOVE = 4.0f;
constexpr float FIELD_BELOW = 2.2f;
// Keep particles out of the player's face.
constexpr float NEAR_CLEAR = 0.35f;

struct GpuInstance
{
  float px, py, pz, size;   // position, snowflake radius
  float dx, dy, dz, alpha;  // rain streak vector (tail = pos - d), opacity
};

struct PushConstants
{
  float view_proj[16];
  float eye[4];
  float params[4];  // x: 0 rain / 1 snow, y: streak half width, z: global alpha
};

constexpr std::string_view VERTEX_SHADER = R"(
layout(push_constant) uniform PC { mat4 view_proj; vec4 eye; vec4 params; } pc;
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_dir;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;
void main()
{
  int corner = gl_VertexIndex & 3;
  float side = (corner & 1) == 0 ? -1.0 : 1.0;
  float along = (corner & 2) == 0 ? 0.0 : 1.0;
  vec3 p = a_pos.xyz;
  vec3 to_eye = normalize(pc.eye.xyz - p);
  vec3 world;
  if (pc.params.x < 0.5)
  {
    // Rain: a thin streak from the drop back along its motion, facing the eye.
    vec3 dir = a_dir.xyz;
    vec3 right = normalize(cross(dir, to_eye)) * pc.params.y;
    world = p - dir * along + right * side;
  }
  else
  {
    // Snow: a round billboard.
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), to_eye));
    vec3 up = cross(to_eye, right);
    world = p + (right * side + up * (along * 2.0 - 1.0)) * a_pos.w;
  }
  v_uv = vec2(side, along * 2.0 - 1.0);
  v_alpha = a_dir.w * pc.params.z;
  gl_Position = pc.view_proj * vec4(world, 1.0);
}
)";

constexpr std::string_view FRAGMENT_SHADER = R"(
layout(push_constant) uniform PC { mat4 view_proj; vec4 eye; vec4 params; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;
layout(location = 0) out vec4 o_color;
void main()
{
  float a;
  vec3 color;
  if (pc.params.x < 0.5)
  {
    // Bright head, fading tail, soft edges.
    a = (1.0 - abs(v_uv.x)) * (0.35 + 0.65 * (1.0 - (v_uv.y * 0.5 + 0.5))) * v_alpha;
    color = vec3(0.80, 0.86, 0.96);
  }
  else
  {
    float d = clamp(1.0 - length(v_uv), 0.0, 1.0);
    a = d * d * v_alpha;
    color = vec3(1.0);
  }
  o_color = vec4(color * a, a);  // premultiplied
}
)";

struct Resources
{
  XrSwapchain swapchain = XR_NULL_HANDLE;
  u32 width = 0;
  u32 height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> images;
  std::vector<std::array<VkImageView, 2>> views;
  std::vector<std::array<VkFramebuffer, 2>> framebuffers;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, RING> command_buffers{};
  std::array<VkFence, RING> fences{};
  std::array<VkBuffer, RING> buffers{};
  std::array<VmaAllocation, RING> allocations{};
  std::array<GpuInstance*, RING> mapped{};
  u32 ring_index = 0;
};

struct Simulation
{
  HitzModes::Weather kind = HitzModes::Weather::Off;
  std::vector<std::array<float, 3>> pos;
  std::vector<std::array<float, 3>> vel;
  std::vector<float> size;
  std::vector<float> alpha;
  std::vector<float> phase;
  float intensity = 0.0f;  // fades in and out
  XrTime last_time = 0;
  float clock = 0.0f;
  std::mt19937 rng{20030};
};

std::mutex s_mutex;
std::optional<Resources> s_res;
bool s_failed = false;
Simulation s_sim;
XrCompositionLayerProjection s_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
std::array<XrCompositionLayerProjectionView, 2> s_layer_views{};

float Rand(float lo, float hi)
{
  return std::uniform_real_distribution<float>(lo, hi)(s_sim.rng);
}

void DestroyResources()
{
  if (!s_res)
    return;
  Resources& r = *s_res;
  const VkDevice device = g_vulkan_context->GetDevice();

  for (VkFence fence : r.fences)
  {
    if (fence != VK_NULL_HANDLE)
    {
      vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
      vkDestroyFence(device, fence, nullptr);
    }
  }
  for (u32 i = 0; i < RING; ++i)
  {
    if (r.buffers[i] != VK_NULL_HANDLE)
      vmaDestroyBuffer(g_vulkan_context->GetMemoryAllocator(), r.buffers[i], r.allocations[i]);
  }
  if (r.command_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool(device, r.command_pool, nullptr);
  for (auto& fbs : r.framebuffers)
  {
    for (VkFramebuffer fb : fbs)
    {
      if (fb != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, fb, nullptr);
    }
  }
  for (auto& views : r.views)
  {
    for (VkImageView view : views)
    {
      if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    }
  }
  if (r.pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(device, r.pipeline, nullptr);
  if (r.pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device, r.pipeline_layout, nullptr);
  if (r.render_pass != VK_NULL_HANDLE)
    vkDestroyRenderPass(device, r.render_pass, nullptr);
  if (r.swapchain != XR_NULL_HANDLE)
    xrDestroySwapchain(r.swapchain);
  s_res.reset();
}

VkShaderModule CreateShaderModule(std::string_view source, bool vertex)
{
  const auto spirv = vertex ? ShaderCompiler::CompileVertexShader(source, nullptr) :
                              ShaderCompiler::CompileFragmentShader(source, nullptr);
  if (!spirv)
    return VK_NULL_HANDLE;
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = spirv->size() * sizeof(u32);
  info.pCode = spirv->data();
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(g_vulkan_context->GetDevice(), &info, nullptr, &module);
  return module;
}

bool CreateResources()
{
  if (!VR::g_openxr || !g_vulkan_context)
    return false;
  const XrSession session = VR::g_openxr->GetSession();
  const VkDevice device = g_vulkan_context->GetDevice();
  s_res.emplace();
  Resources& r = *s_res;

  // Format: same family the game's eye swapchains use.
  u32 format_count = 0;
  xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
  std::vector<int64_t> formats(format_count);
  xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
  for (VkFormat wanted : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                          VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
  {
    if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(wanted)) != formats.end())
    {
      r.format = wanted;
      break;
    }
  }
  if (r.format == VK_FORMAT_UNDEFINED)
    return false;

  // Weather doesn't need full resolution: half the recommended eye size.
  const auto& view_config = VR::g_openxr->GetViewConfigViews()[0];
  r.width = std::max<u32>(256, view_config.recommendedImageRectWidth / 2);
  r.height = std::max<u32>(256, view_config.recommendedImageRectHeight / 2);

  XrSwapchainCreateInfo sc_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sc_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  sc_info.format = r.format;
  sc_info.sampleCount = 1;
  sc_info.width = r.width;
  sc_info.height = r.height;
  sc_info.faceCount = 1;
  sc_info.arraySize = 2;
  sc_info.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(session, &sc_info, &r.swapchain)))
    return false;

  u32 image_count = 0;
  xrEnumerateSwapchainImages(r.swapchain, 0, &image_count, nullptr);
  std::vector<XrSwapchainImageVulkanKHR> xr_images(image_count,
                                                   {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if (XR_FAILED(xrEnumerateSwapchainImages(
          r.swapchain, image_count, &image_count,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data()))))
  {
    return false;
  }

  // Render pass: clear to transparent, leave the image as the runtime expects it.
  VkAttachmentDescription attachment{};
  attachment.format = r.format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp_info.attachmentCount = 1;
  rp_info.pAttachments = &attachment;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = 1;
  rp_info.pDependencies = &dependency;
  if (vkCreateRenderPass(device, &rp_info, nullptr, &r.render_pass) != VK_SUCCESS)
    return false;

  // One view + framebuffer per eye layer of every swapchain image.
  for (const auto& xr_image : xr_images)
  {
    r.images.push_back(xr_image.image);
    auto& views = r.views.emplace_back();
    auto& fbs = r.framebuffers.emplace_back();
    views.fill(VK_NULL_HANDLE);
    fbs.fill(VK_NULL_HANDLE);
    for (u32 eye = 0; eye < 2; ++eye)
    {
      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = xr_image.image;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = r.format;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
      if (vkCreateImageView(device, &view_info, nullptr, &views[eye]) != VK_SUCCESS)
        return false;
      VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb_info.renderPass = r.render_pass;
      fb_info.attachmentCount = 1;
      fb_info.pAttachments = &views[eye];
      fb_info.width = r.width;
      fb_info.height = r.height;
      fb_info.layers = 1;
      if (vkCreateFramebuffer(device, &fb_info, nullptr, &fbs[eye]) != VK_SUCCESS)
        return false;
    }
  }

  // Pipeline.
  VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &r.pipeline_layout) != VK_SUCCESS)
    return false;

  const VkShaderModule vs = CreateShaderModule(VERTEX_SHADER, true);
  const VkShaderModule fs = CreateShaderModule(FRAGMENT_SHADER, false);
  if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
  {
    if (vs != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, vs, nullptr);
    if (fs != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, fs, nullptr);
    return false;
  }
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{0, sizeof(GpuInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
  std::array<VkVertexInputAttributeDescription, 2> attributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
  }};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
  vertex_input.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &blend;

  const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = static_cast<u32>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();

  VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.stageCount = static_cast<u32>(stages.size());
  pipeline_info.pStages = stages.data();
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = r.pipeline_layout;
  pipeline_info.renderPass = r.render_pass;
  const VkResult pipeline_result =
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &r.pipeline);
  vkDestroyShaderModule(device, vs, nullptr);
  vkDestroyShaderModule(device, fs, nullptr);
  if (pipeline_result != VK_SUCCESS)
    return false;

  // Commands, fences and per-frame instance buffers (host-visible, persistently mapped).
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  if (vkCreateCommandPool(device, &pool_info, nullptr, &r.command_pool) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cb_info.commandPool = r.command_pool;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = RING;
  if (vkAllocateCommandBuffers(device, &cb_info, r.command_buffers.data()) != VK_SUCCESS)
    return false;

  for (u32 i = 0; i < RING; ++i)
  {
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fence_info, nullptr, &r.fences[i]) != VK_SUCCESS)
      return false;

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = sizeof(GpuInstance) * MAX_PARTICLES;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    VmaAllocationInfo mapped_info{};
    if (vmaCreateBuffer(g_vulkan_context->GetMemoryAllocator(), &buffer_info, &alloc_info,
                        &r.buffers[i], &r.allocations[i], &mapped_info) != VK_SUCCESS ||
        mapped_info.pMappedData == nullptr)
    {
      return false;
    }
    r.mapped[i] = static_cast<GpuInstance*>(mapped_info.pMappedData);
  }

  INFO_LOG_FMT(VIDEO, "HitzWeather: overlay ready ({}x{}, {} images)", r.width, r.height,
               r.images.size());
  return true;
}

void Respawn(size_t i, const std::array<float, 3>& head, bool anywhere)
{
  auto& p = s_sim.pos[i];
  p[0] = head[0] + Rand(-FIELD_RADIUS, FIELD_RADIUS);
  p[2] = head[2] + Rand(-FIELD_RADIUS, FIELD_RADIUS);
  p[1] = anywhere ? head[1] + Rand(-FIELD_BELOW, FIELD_ABOVE) :
                    head[1] + FIELD_ABOVE - Rand(0.0f, 0.5f);
  if (s_sim.kind == HitzModes::Weather::Rain)
  {
    s_sim.vel[i] = {0.6f + Rand(-0.2f, 0.2f), -8.5f + Rand(-1.0f, 1.0f), 0.3f + Rand(-0.2f, 0.2f)};
    s_sim.size[i] = 0.0f;
    s_sim.alpha[i] = Rand(0.45f, 1.0f);
  }
  else
  {
    s_sim.vel[i] = {Rand(-0.15f, 0.15f), -Rand(0.6f, 1.1f), Rand(-0.15f, 0.15f)};
    s_sim.size[i] = Rand(0.012f, 0.03f);
    s_sim.alpha[i] = Rand(0.7f, 1.0f);
  }
  s_sim.phase[i] = Rand(0.0f, 6.2831853f);
}

void Reseed(HitzModes::Weather kind, const std::array<float, 3>& head)
{
  s_sim.kind = kind;
  // Kept modest: in r8 the 3D modes were already GPU-bound before weather was added.
  const size_t count = kind == HitzModes::Weather::Rain ? 1800 : 1500;
  s_sim.pos.assign(count, {});
  s_sim.vel.assign(count, {});
  s_sim.size.assign(count, 0.0f);
  s_sim.alpha.assign(count, 0.0f);
  s_sim.phase.assign(count, 0.0f);
  for (size_t i = 0; i < count; ++i)
    Respawn(i, head, true);
}

// Steps the particles and writes the visible ones. Returns how many were written.
u32 Simulate(float dt, const std::array<float, 3>& head, GpuInstance* out)
{
  s_sim.clock += dt;
  const bool rain = s_sim.kind == HitzModes::Weather::Rain;
  u32 written = 0;
  for (size_t i = 0; i < s_sim.pos.size(); ++i)
  {
    auto& p = s_sim.pos[i];
    const auto& v = s_sim.vel[i];
    float sway_x = 0.0f, sway_z = 0.0f;
    if (!rain)
    {
      sway_x = 0.25f * std::sin(s_sim.clock * 1.3f + s_sim.phase[i]);
      sway_z = 0.25f * std::cos(s_sim.clock * 1.1f + s_sim.phase[i]);
    }
    p[0] += (v[0] + sway_x) * dt;
    p[1] += v[1] * dt;
    p[2] += (v[2] + sway_z) * dt;

    // Keep the field centred on the head: wrap sideways, respawn on top when it lands.
    for (int axis : {0, 2})
    {
      const float rel = p[axis] - head[axis];
      if (rel > FIELD_RADIUS)
        p[axis] -= 2.0f * FIELD_RADIUS;
      else if (rel < -FIELD_RADIUS)
        p[axis] += 2.0f * FIELD_RADIUS;
    }
    if (p[1] < head[1] - FIELD_BELOW || p[1] > head[1] + FIELD_ABOVE + 1.0f)
      Respawn(i, head, false);

    const float dx = p[0] - head[0], dy = p[1] - head[1], dz = p[2] - head[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < NEAR_CLEAR || written >= MAX_PARTICLES)
      continue;
    // Fade near the edge of the field so the box never shows.
    const float edge = std::clamp((FIELD_RADIUS - std::sqrt(dx * dx + dz * dz)) / 2.0f, 0.0f, 1.0f);
    const float streak = rain ? 0.032f : 0.0f;
    out[written++] = {p[0], p[1], p[2], s_sim.size[i], v[0] * streak, v[1] * streak,
                      v[2] * streak, s_sim.alpha[i] * edge};
  }
  return written;
}

// Column-major view-projection for one eye (Vulkan clip space: y down, z 0..1).
void EyeViewProj(const XrView& view, float* out)
{
  const float near_z = 0.05f, far_z = 100.0f;
  const float tl = std::tan(view.fov.angleLeft), tr = std::tan(view.fov.angleRight);
  const float tu = std::tan(view.fov.angleUp), td = std::tan(view.fov.angleDown);
  const float w = tr - tl, h = td - tu;
  float proj[16] = {};
  proj[0] = 2.0f / w;
  proj[8] = (tr + tl) / w;
  proj[5] = 2.0f / h;
  proj[9] = (tu + td) / h;
  proj[10] = -far_z / (far_z - near_z);
  proj[14] = -(far_z * near_z) / (far_z - near_z);
  proj[11] = -1.0f;

  // View = inverse(eye pose): transpose the rotation, rotate the negated position.
  const XrQuaternionf q = view.pose.orientation;
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  // Rotation matrix R (row-major r[row][col]).
  const float r[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                         {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                         {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
  const XrVector3f p = view.pose.position;
  float viewm[16] = {};
  for (int row = 0; row < 3; ++row)
  {
    for (int col = 0; col < 3; ++col)
      viewm[col * 4 + row] = r[col][row];  // R^T
    viewm[12 + row] = -(r[0][row] * p.x + r[1][row] * p.y + r[2][row] * p.z);
  }
  viewm[15] = 1.0f;

  for (int col = 0; col < 4; ++col)
  {
    for (int row = 0; row < 4; ++row)
    {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k)
        sum += proj[k * 4 + row] * viewm[col * 4 + k];
      out[col * 4 + row] = sum;
    }
  }
}

const XrCompositionLayerBaseHeader* RenderLayer(XrTime display_time)
{
  std::lock_guard lk(s_mutex);
  if (!VR::g_openxr || !g_vulkan_context || s_failed)
    return nullptr;

  const HitzModes::Weather weather = HitzModes::GetWeather();
  const float dt =
      s_sim.last_time != 0 ?
          std::clamp(static_cast<float>(display_time - s_sim.last_time) * 1e-9f, 0.0f, 0.1f) :
          0.0f;
  s_sim.last_time = display_time;
  const float target = weather != HitzModes::Weather::Off ? 1.0f : 0.0f;
  s_sim.intensity += std::clamp(target - s_sim.intensity, -dt * 0.8f, dt * 0.8f);
  if (weather == HitzModes::Weather::Off && s_sim.intensity <= 0.0f)
    return nullptr;

  if (!s_res)
  {
    if (!CreateResources())
    {
      ERROR_LOG_FMT(VIDEO, "HitzWeather: could not create the overlay; weather disabled.");
      DestroyResources();
      s_failed = true;
      return nullptr;
    }
  }
  Resources& r = *s_res;

  // Where the eyes will be at display time.
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = display_time;
  locate_info.space = VR::g_openxr->GetReferenceSpace();
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
  u32 view_count = 0;
  if (XR_FAILED(xrLocateViews(VR::g_openxr->GetSession(), &locate_info, &view_state, 2,
                              &view_count, views.data())) ||
      view_count != 2 ||
      (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
  {
    return nullptr;
  }
  const std::array<float, 3> head{
      0.5f * (views[0].pose.position.x + views[1].pose.position.x),
      0.5f * (views[0].pose.position.y + views[1].pose.position.y),
      0.5f * (views[0].pose.position.z + views[1].pose.position.z)};

  const HitzModes::Weather kind = weather != HitzModes::Weather::Off ? weather : s_sim.kind;
  if (kind != s_sim.kind || s_sim.pos.empty())
    Reseed(kind, head);

  const VkDevice device = g_vulkan_context->GetDevice();
  const u32 slot = r.ring_index;
  r.ring_index = (r.ring_index + 1) % RING;
  vkWaitForFences(device, 1, &r.fences[slot], VK_TRUE, UINT64_MAX);
  const u32 count = Simulate(dt, head, r.mapped[slot]);

  // BaseballMR: acquire and wait under the graphics queue lock (see VulkanOpenXR::AcquireAndWait).
  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  u32 image_index = 0;
  {
    auto queue_lock = g_openxr_vk->AcquireGraphicsQueueLock();
    if (XR_FAILED(xrAcquireSwapchainImage(r.swapchain, &acquire_info, &image_index)))
      return nullptr;
  }
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = 50'000'000;  // ns
  XrResult wait_result;
  do
  {
    auto queue_lock = g_openxr_vk->AcquireGraphicsQueueLock();
    wait_result = xrWaitSwapchainImage(r.swapchain, &wait_info);
  } while (wait_result == XR_TIMEOUT_EXPIRED);

  const VkCommandBuffer cmd = r.command_buffers[slot];
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin_info);
  for (u32 eye = 0; eye < 2; ++eye)
  {
    VkClearValue clear{};
    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = r.render_pass;
    rp_begin.framebuffer = r.framebuffers[image_index][eye];
    rp_begin.renderArea = {{0, 0}, {r.width, r.height}};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    if (count > 0)
    {
      const VkViewport viewport{0.0f, 0.0f, static_cast<float>(r.width),
                                static_cast<float>(r.height), 0.0f, 1.0f};
      const VkRect2D scissor{{0, 0}, {r.width, r.height}};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
      PushConstants pc{};
      EyeViewProj(views[eye], pc.view_proj);
      pc.eye[0] = views[eye].pose.position.x;
      pc.eye[1] = views[eye].pose.position.y;
      pc.eye[2] = views[eye].pose.position.z;
      pc.params[0] = kind == HitzModes::Weather::Snow ? 1.0f : 0.0f;
      pc.params[1] = 0.0045f;
      pc.params[2] = s_sim.intensity;
      vkCmdPushConstants(cmd, r.pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                         &pc);
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &r.buffers[slot], &offset);
      vkCmdDraw(cmd, 4, count, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
  }
  vkEndCommandBuffer(cmd);

  vkResetFences(device, 1, &r.fences[slot]);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  {
    // Dolphin's command buffer worker submits on the same queue.
    auto queue_lock = g_openxr_vk->AcquireGraphicsQueueLock();
    vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 1, &submit, r.fences[slot]);
    xrReleaseSwapchainImage(r.swapchain, &release_info);
  }

  for (u32 eye = 0; eye < 2; ++eye)
  {
    XrCompositionLayerProjectionView& v = s_layer_views[eye];
    v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    v.pose = views[eye].pose;
    v.fov = views[eye].fov;
    v.subImage.swapchain = r.swapchain;
    v.subImage.imageRect = {{0, 0},
                            {static_cast<int32_t>(r.width), static_cast<int32_t>(r.height)}};
    v.subImage.imageArrayIndex = eye;
  }
  s_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  s_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  s_layer.space = VR::g_openxr->GetReferenceSpace();
  s_layer.viewCount = 2;
  s_layer.views = s_layer_views.data();
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&s_layer);
}
}  // namespace

void Initialize()
{
  std::lock_guard lk(s_mutex);
  s_failed = false;
  s_sim = {};
  HitzModes::SetOverlayLayerProvider(&RenderLayer);
}

void Shutdown()
{
  HitzModes::SetOverlayLayerProvider(nullptr);
  std::lock_guard lk(s_mutex);
  DestroyResources();
}
}  // namespace Vulkan::HitzWeather

#endif  // ENABLE_VR
