// Fork (odisea): interfaz a la resolucion de salida en Vulkan (ver
// d3d12/odisea_hud_d3d12.cpp, el mismo esquema).
//
// Los dibujos de la interfaz sobre la pantalla principal no van a la EDRAM
// (FSI): IssueDraw los dibuja con pipelines sin FSI (bit 63 de las
// modificaciones, VulkanPipelineCache::kOdiseaHudModificationBit) en hud_image_,
// RGBA8 a la resolucion de salida, transparente al empezar cada fotograma y con
// el alfa acumulando la cobertura. En el swap, el ultimo paso de siempre (rampa
// de gamma, FXAA o SMAA) escribe la escena en hud_scene_image_ y ComposeHud la
// reescala a la salida del presentador con la interfaz encima
// (shaders/odisea_hud_compose.cs.hlsl, variante SPIR-V).
#include <rex/graphics/vulkan/command_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/odisea_dlss.h>
#include <rex/graphics/odisea_watched_draw.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/odisea_hud_compose_cs.h"
}  // namespace shaders

namespace {

constexpr VkFormat kHudImageFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct HudComposeConstants {
  float output_size[4];  // ancho, alto, 1/ancho, 1/alto
  uint32_t flags[4];     // x = hay interfaz
};

enum : uint32_t {
  kHudBindingScene,
  kHudBindingInterface,
  kHudBindingSampler,
  kHudBindingDestination,
  kHudBindingCount,
};

bool CreateHudImage(const ui::vulkan::VulkanDevice* vulkan_device, VkFormat format,
                    uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImage& image,
                    VkDeviceMemory& memory, VkImageView& view) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = format;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = usage;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
          memory)) {
    return false;
  }
  VkImageViewCreateInfo view_create_info = {};
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view) != VK_SUCCESS) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, memory);
    return false;
  }
  return true;
}

}  // namespace

bool VulkanCommandProcessor::InitializeHud() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSetLayoutBinding bindings[kHudBindingCount];
  for (uint32_t i = 0; i < kHudBindingCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i == kHudBindingSampler       ? VK_DESCRIPTOR_TYPE_SAMPLER
                                 : i == kHudBindingDestination ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                               : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].pImmutableSamplers = nullptr;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_create_info = {};
  set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_create_info.bindingCount = kHudBindingCount;
  set_layout_create_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_create_info, nullptr,
                                      &hud_descriptor_set_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the HUD descriptor set layout");
    ShutdownHud();
    return false;
  }
  VkDescriptorPoolSize pool_sizes[3];
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  pool_sizes[0].descriptorCount = kMaxFramesInFlight * 2;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  pool_sizes[1].descriptorCount = kMaxFramesInFlight;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[2].descriptorCount = kMaxFramesInFlight;
  VkDescriptorPoolCreateInfo pool_create_info = {};
  pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_create_info.maxSets = kMaxFramesInFlight;
  pool_create_info.poolSizeCount = 3;
  pool_create_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_create_info, nullptr, &hud_descriptor_pool_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the HUD descriptor pool");
    ShutdownHud();
    return false;
  }
  VkDescriptorSetAllocateInfo set_allocate_info = {};
  set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_allocate_info.descriptorPool = hud_descriptor_pool_;
  set_allocate_info.descriptorSetCount = 1;
  set_allocate_info.pSetLayouts = &hud_descriptor_set_layout_;
  for (VkDescriptorSet& set : hud_descriptor_sets_) {
    if (dfn.vkAllocateDescriptorSets(device, &set_allocate_info, &set) != VK_SUCCESS) {
      REXGPU_ERROR("odisea: failed to allocate the HUD descriptor sets");
      ShutdownHud();
      return false;
    }
  }
  VkPushConstantRange push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(HudComposeConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info = {};
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.setLayoutCount = 1;
  pipeline_layout_create_info.pSetLayouts = &hud_descriptor_set_layout_;
  pipeline_layout_create_info.pushConstantRangeCount = 1;
  pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &hud_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the HUD pipeline layout");
    ShutdownHud();
    return false;
  }
  hud_compose_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, hud_pipeline_layout_, shaders::odisea_hud_compose_cs,
      sizeof(shaders::odisea_hud_compose_cs));
  if (hud_compose_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_ERROR("odisea: failed to create the HUD compose pipeline");
    ShutdownHud();
    return false;
  }
  return true;
}

void VulkanCommandProcessor::ShutdownHud() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         hud_framebuffer_.framebuffer);
  for (HudImage* image : {&hud_image_, &hud_scene_image_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image->view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image->image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image->memory);
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->width = image->height = 0;
  }
  hud_has_content_ = false;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, hud_compose_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         hud_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         hud_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         hud_descriptor_set_layout_);
  hud_descriptor_sets_.fill(VK_NULL_HANDLE);
}

bool VulkanCommandProcessor::GetHudOutputSize(uint32_t& width, uint32_t& height) const {
  if (hud_compose_pipeline_ == VK_NULL_HANDLE ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kPixelShaderInterlock ||
      // La escena se compone con el camino de compute del swap.
      swap_apply_gamma_compute_256_entry_table_pipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  height = odisea::HudOutputHeight();
  width = odisea::HudOutputWidth();
  return width && height;
}

bool VulkanCommandProcessor::EnsureHudImage(HudImage& hud_image, VkFormat format,
                                            uint32_t width, uint32_t height,
                                            VkImageUsageFlags usage) {
  if (hud_image.image != VK_NULL_HANDLE && hud_image.width == width &&
      hud_image.height == height) {
    return true;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint64_t destroy_submission = GetCurrentSubmission();
  if (&hud_image == &hud_image_ && hud_framebuffer_.framebuffer != VK_NULL_HANDLE) {
    destroy_framebuffers_.emplace_back(destroy_submission, hud_framebuffer_.framebuffer);
    hud_framebuffer_.framebuffer = VK_NULL_HANDLE;
  }
  if (hud_image.view != VK_NULL_HANDLE) {
    destroy_image_views_.emplace_back(destroy_submission, hud_image.view);
    hud_image.view = VK_NULL_HANDLE;
  }
  if (hud_image.image != VK_NULL_HANDLE) {
    destroy_images_.emplace_back(destroy_submission, hud_image.image);
    hud_image.image = VK_NULL_HANDLE;
  }
  if (hud_image.memory != VK_NULL_HANDLE) {
    destroy_memory_.emplace_back(destroy_submission, hud_image.memory);
    hud_image.memory = VK_NULL_HANDLE;
  }
  hud_image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  hud_image.width = hud_image.height = 0;
  if (!CreateHudImage(vulkan_device, format, width, height, usage, hud_image.image,
                      hud_image.memory, hud_image.view)) {
    REXGPU_ERROR("odisea: failed to create a {}x{} HUD image", width, height);
    return false;
  }
  hud_image.width = width;
  hud_image.height = height;
  if (&hud_image == &hud_image_) {
    REXGPU_INFO("odisea: interfaz a {}x{}, fuera de la EDRAM (Vulkan)", width, height);
  }
  return true;
}

bool VulkanCommandProcessor::BeginHudDraw(uint32_t width, uint32_t height) {
  if (!EnsureHudImage(hud_image_, kHudImageFormat, width, height,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return false;
  }
  VkRenderPass render_pass = pipeline_cache_->GetOdiseaHudRenderPass();
  hud_framebuffer_.host_extent.width = width;
  hud_framebuffer_.host_extent.height = height;
  if (render_pass != VK_NULL_HANDLE && hud_framebuffer_.framebuffer == VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    VkFramebufferCreateInfo framebuffer_create_info = {};
    framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_create_info.renderPass = render_pass;
    framebuffer_create_info.attachmentCount = 1;
    framebuffer_create_info.pAttachments = &hud_image_.view;
    framebuffer_create_info.width = width;
    framebuffer_create_info.height = height;
    framebuffer_create_info.layers = 1;
    if (vulkan_device->functions().vkCreateFramebuffer(vulkan_device->device(),
                                                        &framebuffer_create_info, nullptr,
                                                        &hud_framebuffer_.framebuffer) !=
        VK_SUCCESS) {
      REXGPU_ERROR("odisea: failed to create the HUD framebuffer");
      return false;
    }
  }
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  if (!hud_has_content_) {
    // Primer dibujo despues del swap: transparente.
    PushImageMemoryBarrier(hud_image_.image, range, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    SubmitBarriers(true);
    VkClearColorValue transparent = {};
    deferred_command_buffer_.CmdVkClearColorImage(
        hud_image_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
    PushImageMemoryBarrier(hud_image_.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    hud_image_.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hud_has_content_ = true;
  } else if (hud_image_.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    PushImageMemoryBarrier(hud_image_.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           hud_image_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    hud_image_.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  hud_image_submission_ = GetCurrentSubmission();
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(render_pass, &hud_framebuffer_,
                                                    hud_image_.view, false);
  return true;
}

bool VulkanCommandProcessor::BeginHudSceneWrite(uint32_t width, uint32_t height) {
  if (!EnsureHudImage(hud_scene_image_, ui::vulkan::VulkanPresenter::kGuestOutputFormat, width,
                      height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
    return false;
  }
  // El paso final de la escena la sobrescribe entera.
  const bool was_read = hud_scene_image_.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  PushImageMemoryBarrier(
      hud_scene_image_.image, ui::vulkan::util::InitializeSubresourceRange(),
      was_read ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, was_read ? VK_ACCESS_SHADER_READ_BIT : 0,
      VK_ACCESS_SHADER_WRITE_BIT, hud_scene_image_.layout, VK_IMAGE_LAYOUT_GENERAL);
  hud_scene_image_.layout = VK_IMAGE_LAYOUT_GENERAL;
  hud_scene_image_submission_ = GetCurrentSubmission();
  return true;
}

void VulkanCommandProcessor::ComposeHud(VkImageView dest_view, uint32_t width, uint32_t height,
                                        uint32_t frame_index, HudImage* scene) {
  if (!scene) {
    scene = &hud_scene_image_;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  bool has_interface = hud_has_content_ && hud_image_.image != VK_NULL_HANDLE &&
                       hud_image_.width == width && hud_image_.height == height;

  // La escena viene de un compute (rampa, SMAA o DLSS), en GENERAL.
  if (scene->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    PushImageMemoryBarrier(scene->image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, scene->layout,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    scene->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  if (has_interface) {
    PushImageMemoryBarrier(hud_image_.image, range, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                           hud_image_.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    hud_image_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  SubmitBarriers(true);

  VkDescriptorSet set = hud_descriptor_sets_[frame_index];
  VkDescriptorImageInfo image_infos[kHudBindingCount] = {};
  image_infos[kHudBindingScene].imageView = scene->view;
  image_infos[kHudBindingScene].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // Sin interfaz se enlaza la escena tambien (el sombreador no la lee).
  image_infos[kHudBindingInterface].imageView =
      has_interface ? hud_image_.view : scene->view;
  image_infos[kHudBindingInterface].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  image_infos[kHudBindingSampler].sampler = swap_sampler_linear_clamp_;
  image_infos[kHudBindingDestination].imageView = dest_view;
  image_infos[kHudBindingDestination].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[kHudBindingCount];
  for (uint32_t i = 0; i < kHudBindingCount; ++i) {
    writes[i] = {};
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i == kHudBindingSampler       ? VK_DESCRIPTOR_TYPE_SAMPLER
                               : i == kHudBindingDestination ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                             : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &image_infos[i];
  }
  dfn.vkUpdateDescriptorSets(device, kHudBindingCount, writes, 0, nullptr);

  HudComposeConstants constants;
  constants.output_size[0] = float(width);
  constants.output_size[1] = float(height);
  constants.output_size[2] = 1.0f / float(width);
  constants.output_size[3] = 1.0f / float(height);
  constants.flags[0] = has_interface ? 1 : 0;
  float sharpness = odisea::dlss::Sharpness();
  std::memcpy(&constants.flags[1], &sharpness, sizeof(float));
  constants.flags[2] = constants.flags[3] = 0;
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   hud_pipeline_layout_, 0, 1, &set, 0, nullptr);
  deferred_command_buffer_.CmdVkPushConstants(hud_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                                              0, sizeof(constants), &constants);
  BindExternalComputePipeline(hud_compose_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((width + 15) / 16, (height + 7) / 8, 1);
  // El siguiente fotograma empieza con la interfaz vacia.
  hud_has_content_ = false;
}

}  // namespace rex::graphics::vulkan
