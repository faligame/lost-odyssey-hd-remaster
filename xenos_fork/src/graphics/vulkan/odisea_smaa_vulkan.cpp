// Fork (odisea): SMAA 1x en el swap de Vulkan.
//
// IssueSwap aplica la rampa de gamma (la variante sin luma) a smaa_color_, que
// tiene el formato de la salida del presentador porque es el que declara ese
// shader, y ApplySmaa hace desde ahi las tres pasadas hasta la salida:
//   bordes  color                -> smaa_edges_
//   pesos   bordes, area, search -> smaa_weights_
//   mezcla  color, pesos         -> salida
// Los shaders son shaders/odisea_smaa.cs.hlsl (tools/build_smaa_shaders.py).
//
// A diferencia de D3D12, aqui todo lo que puede fallar (imagenes intermedias y
// subida de las texturas de consulta) se prepara en IssueSwap ANTES de decidir
// a donde escribe la rampa de gamma, asi que ApplySmaa ya no puede fallar y la
// salida nunca se queda sin escribir.
#include <rex/graphics/vulkan/command_processor.h>

#include <rex/graphics/odisea_smaa.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/odisea_smaa_blend_cs.h"
#include "../shaders/vulkan_spirv/odisea_smaa_edges_cs.h"
#include "../shaders/vulkan_spirv/odisea_smaa_weights_cs.h"
}  // namespace shaders

namespace {

// Mismo orden que odisea_smaa.cs.hlsl.
enum SmaaBinding : uint32_t {
  kSmaaBindingInput0,
  kSmaaBindingInput1,
  kSmaaBindingInput2,
  kSmaaBindingSampler,
  kSmaaBindingDestination,

  kSmaaBindingCount,
};

// RGBA8 para bordes y pesos: es el formato que cualquier dispositivo admite como
// storage image (RG8 no esta garantizado).
constexpr VkFormat kSmaaIntermediateFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct SmaaConstants {
  float metrics[4];  // 1/ancho, 1/alto, ancho, alto
};

VkDescriptorType SmaaBindingType(uint32_t binding) {
  switch (binding) {
    case kSmaaBindingSampler:
      return VK_DESCRIPTOR_TYPE_SAMPLER;
    case kSmaaBindingDestination:
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    default:
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  }
}

bool CreateSmaaImage(const ui::vulkan::VulkanDevice* vulkan_device, VkFormat format,
                     uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImage& image,
                     VkDeviceMemory& memory, VkImageView& view) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
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
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image,
          memory)) {
    return false;
  }
  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
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

bool VulkanCommandProcessor::InitializeSmaa() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSetLayoutBinding bindings[kSmaaBindingCount];
  for (uint32_t i = 0; i < kSmaaBindingCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = SmaaBindingType(i);
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].pImmutableSamplers = nullptr;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_create_info;
  set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_create_info.pNext = nullptr;
  set_layout_create_info.flags = 0;
  set_layout_create_info.bindingCount = kSmaaBindingCount;
  set_layout_create_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_create_info, nullptr,
                                      &smaa_descriptor_set_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the SMAA descriptor set layout");
    ShutdownSmaa();
    return false;
  }

  // Un set por pasada y por frame en vuelo, como los del swap: un set no se
  // puede actualizar mientras lo usa un envio que aun no ha terminado.
  constexpr uint32_t kSetCount = kMaxFramesInFlight * 3;
  VkDescriptorPoolSize pool_sizes[3];
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  pool_sizes[0].descriptorCount = kSetCount * 3;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  pool_sizes[1].descriptorCount = kSetCount;
  pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[2].descriptorCount = kSetCount;
  VkDescriptorPoolCreateInfo pool_create_info;
  pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_create_info.pNext = nullptr;
  pool_create_info.flags = 0;
  pool_create_info.maxSets = kSetCount;
  pool_create_info.poolSizeCount = 3;
  pool_create_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_create_info, nullptr, &smaa_descriptor_pool_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the SMAA descriptor pool");
    ShutdownSmaa();
    return false;
  }
  VkDescriptorSetAllocateInfo set_allocate_info;
  set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_allocate_info.pNext = nullptr;
  set_allocate_info.descriptorPool = smaa_descriptor_pool_;
  set_allocate_info.descriptorSetCount = 1;
  set_allocate_info.pSetLayouts = &smaa_descriptor_set_layout_;
  for (auto& frame_sets : smaa_descriptor_sets_) {
    for (VkDescriptorSet& set : frame_sets) {
      if (dfn.vkAllocateDescriptorSets(device, &set_allocate_info, &set) != VK_SUCCESS) {
        REXGPU_ERROR("odisea: failed to allocate the SMAA descriptor sets");
        ShutdownSmaa();
        return false;
      }
    }
  }

  VkPushConstantRange push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(SmaaConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = 1;
  pipeline_layout_create_info.pSetLayouts = &smaa_descriptor_set_layout_;
  pipeline_layout_create_info.pushConstantRangeCount = 1;
  pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &smaa_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("odisea: failed to create the SMAA pipeline layout");
    ShutdownSmaa();
    return false;
  }

  struct PipelineSource {
    const uint32_t* code;
    size_t size_bytes;
  };
  const PipelineSource pipeline_sources[3] = {
      {shaders::odisea_smaa_edges_cs, sizeof(shaders::odisea_smaa_edges_cs)},
      {shaders::odisea_smaa_weights_cs, sizeof(shaders::odisea_smaa_weights_cs)},
      {shaders::odisea_smaa_blend_cs, sizeof(shaders::odisea_smaa_blend_cs)},
  };
  for (uint32_t i = 0; i < 3; ++i) {
    smaa_pipelines_[i] = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, smaa_pipeline_layout_, pipeline_sources[i].code,
        pipeline_sources[i].size_bytes);
    if (smaa_pipelines_[i] == VK_NULL_HANDLE) {
      REXGPU_ERROR("odisea: failed to create SMAA compute pipeline {}", i);
      ShutdownSmaa();
      return false;
    }
  }

  constexpr VkImageUsageFlags kLookupUsage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!CreateSmaaImage(vulkan_device, VK_FORMAT_R8G8_UNORM, odisea::kSmaaAreaTexWidth,
                       odisea::kSmaaAreaTexHeight, kLookupUsage, smaa_area_image_,
                       smaa_area_image_memory_, smaa_area_image_view_) ||
      !CreateSmaaImage(vulkan_device, VK_FORMAT_R8_UNORM, odisea::kSmaaSearchTexWidth,
                       odisea::kSmaaSearchTexHeight, kLookupUsage, smaa_search_image_,
                       smaa_search_image_memory_, smaa_search_image_view_)) {
    REXGPU_ERROR("odisea: failed to create the SMAA lookup images");
    ShutdownSmaa();
    return false;
  }
  smaa_lookup_uploaded_ = false;
  return true;
}

void VulkanCommandProcessor::ShutdownSmaa() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  for (SmaaImage* smaa_image : {&smaa_color_, &smaa_edges_, &smaa_weights_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, smaa_image->view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, smaa_image->image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, smaa_image->memory);
    smaa_image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  smaa_images_width_ = 0;
  smaa_images_height_ = 0;
  smaa_images_submission_ = 0;

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, smaa_search_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, smaa_search_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, smaa_search_image_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, smaa_area_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, smaa_area_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, smaa_area_image_memory_);
  smaa_lookup_uploaded_ = false;

  for (VkPipeline& pipeline : smaa_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         smaa_pipeline_layout_);
  // Destruir el pool libera tambien sus sets.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         smaa_descriptor_pool_);
  smaa_descriptor_sets_ = {};
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         smaa_descriptor_set_layout_);
}

bool VulkanCommandProcessor::EnsureSmaaImages(uint32_t width, uint32_t height) {
  assert_true(submission_open_);
  if (smaa_color_.image != VK_NULL_HANDLE && smaa_edges_.image != VK_NULL_HANDLE &&
      smaa_weights_.image != VK_NULL_HANDLE &&
      smaa_images_width_ == width && smaa_images_height_ == height) {
    return true;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Cambio de tamano: las viejas pueden seguir en uso por un envio que aun no ha
  // terminado; se destruyen igual que la fuente del FXAA.
  const bool defer = submission_completed_ < smaa_images_submission_;
  const uint64_t destroy_submission = GetCurrentSubmission();
  for (SmaaImage* smaa_image : {&smaa_color_, &smaa_edges_, &smaa_weights_}) {
    if (defer) {
      if (smaa_image->view != VK_NULL_HANDLE) {
        destroy_image_views_.emplace_back(destroy_submission, smaa_image->view);
        smaa_image->view = VK_NULL_HANDLE;
      }
      if (smaa_image->image != VK_NULL_HANDLE) {
        destroy_images_.emplace_back(destroy_submission, smaa_image->image);
        smaa_image->image = VK_NULL_HANDLE;
      }
      if (smaa_image->memory != VK_NULL_HANDLE) {
        destroy_memory_.emplace_back(destroy_submission, smaa_image->memory);
        smaa_image->memory = VK_NULL_HANDLE;
      }
    } else {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, smaa_image->view);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, smaa_image->image);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, smaa_image->memory);
    }
    smaa_image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  smaa_images_width_ = 0;
  smaa_images_height_ = 0;
  smaa_images_submission_ = 0;

  constexpr VkImageUsageFlags kIntermediateUsage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  for (SmaaImage* smaa_image : {&smaa_color_, &smaa_edges_, &smaa_weights_}) {
    VkFormat format = smaa_image == &smaa_color_
                          ? ui::vulkan::VulkanPresenter::kGuestOutputFormat
                          : kSmaaIntermediateFormat;
    if (!CreateSmaaImage(vulkan_device, format, width, height,
                         kIntermediateUsage, smaa_image->image, smaa_image->memory,
                         smaa_image->view)) {
      REXGPU_ERROR("odisea: failed to create the {}x{} SMAA images", width, height);
      for (SmaaImage* created : {&smaa_color_, &smaa_edges_, &smaa_weights_}) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, created->view);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, created->image);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, created->memory);
      }
      return false;
    }
  }
  smaa_images_width_ = width;
  smaa_images_height_ = height;
  return true;
}

bool VulkanCommandProcessor::UploadSmaaLookupImages() {
  assert_true(submission_open_);
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  struct Lookup {
    VkImage image;
    const uint8_t* bytes;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
  };
  const Lookup lookups[2] = {
      {smaa_area_image_, odisea::SmaaAreaTexBytes(), odisea::kSmaaAreaTexWidth,
       odisea::kSmaaAreaTexHeight, 2},
      {smaa_search_image_, odisea::SmaaSearchTexBytes(), odisea::kSmaaSearchTexWidth,
       odisea::kSmaaSearchTexHeight, 1},
  };
  VkDeviceSize upload_size = 0;
  for (const Lookup& lookup : lookups) {
    upload_size += VkDeviceSize(lookup.width) * lookup.height * lookup.bytes_per_pixel;
  }

  VkBuffer upload_buffer;
  VkDeviceMemory upload_memory;
  uint32_t upload_memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, upload_buffer, upload_memory,
          &upload_memory_type)) {
    REXGPU_ERROR("odisea: failed to create the SMAA lookup upload buffer");
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS ||
      !mapping) {
    REXGPU_ERROR("odisea: failed to map the SMAA lookup upload buffer");
    dfn.vkDestroyBuffer(device, upload_buffer, nullptr);
    dfn.vkFreeMemory(device, upload_memory, nullptr);
    return false;
  }
  VkDeviceSize offset = 0;
  for (const Lookup& lookup : lookups) {
    const size_t size = size_t(lookup.width) * lookup.height * lookup.bytes_per_pixel;
    std::memcpy(static_cast<uint8_t*>(mapping) + offset, lookup.bytes, size);
    offset += size;
  }
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, upload_memory, upload_memory_type, 0,
                                           upload_size, upload_size);
  dfn.vkUnmapMemory(device, upload_memory);

  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  for (const Lookup& lookup : lookups) {
    PushImageMemoryBarrier(lookup.image, range, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  SubmitBarriers(true);
  offset = 0;
  for (const Lookup& lookup : lookups) {
    VkBufferImageCopy* copy = deferred_command_buffer_.CmdCopyBufferToImageEmplace(
        upload_buffer, lookup.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    copy->bufferOffset = offset;
    copy->bufferRowLength = 0;
    copy->bufferImageHeight = 0;
    copy->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy->imageSubresource.mipLevel = 0;
    copy->imageSubresource.baseArrayLayer = 0;
    copy->imageSubresource.layerCount = 1;
    copy->imageOffset.x = 0;
    copy->imageOffset.y = 0;
    copy->imageOffset.z = 0;
    copy->imageExtent.width = lookup.width;
    copy->imageExtent.height = lookup.height;
    copy->imageExtent.depth = 1;
    offset += VkDeviceSize(lookup.width) * lookup.height * lookup.bytes_per_pixel;
    PushImageMemoryBarrier(lookup.image, range, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  // El buffer de subida vive hasta que termina el envio que copia de el.
  destroy_buffers_.emplace_back(GetCurrentSubmission(), upload_buffer);
  destroy_memory_.emplace_back(GetCurrentSubmission(), upload_memory);
  smaa_lookup_uploaded_ = true;
  return true;
}

void VulkanCommandProcessor::BeginSmaaColorWrite() {
  // La rampa sobrescribe la imagen entera: desde UNDEFINED vale si aun no existia.
  const bool was_read = smaa_color_.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  PushImageMemoryBarrier(
      smaa_color_.image, ui::vulkan::util::InitializeSubresourceRange(),
      was_read ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, was_read ? VK_ACCESS_SHADER_READ_BIT : 0,
      VK_ACCESS_SHADER_WRITE_BIT, smaa_color_.layout, VK_IMAGE_LAYOUT_GENERAL);
  smaa_color_.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void VulkanCommandProcessor::ApplySmaa(VkImageView dest_view, uint32_t width, uint32_t height,
                                       uint32_t frame_index) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  smaa_images_submission_ = GetCurrentSubmission();
  const SmaaConstants constants = {
      {1.0f / float(width), 1.0f / float(height), float(width), float(height)}};
  uint32_t group_count_x = (width + 15) / 16;
  uint32_t group_count_y = (height + 7) / 8;
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();

  // La rampa de gamma acaba de escribir el color en smaa_color_: pasa a lectura.
  PushImageMemoryBarrier(smaa_color_.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  smaa_color_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  const VkImageView color_view = smaa_color_.view;

  struct Pass {
    VkPipeline pipeline;
    VkImageView inputs[3];
    VkImageView output;
    // Nullptr para la ultima pasada: la salida del presentador ya esta en GENERAL.
    SmaaImage* output_image;
  };
  const Pass passes[3] = {
      {smaa_pipelines_[0], {color_view, color_view, color_view}, smaa_edges_.view, &smaa_edges_},
      {smaa_pipelines_[1],
       {smaa_edges_.view, smaa_area_image_view_, smaa_search_image_view_},
       smaa_weights_.view,
       &smaa_weights_},
      {smaa_pipelines_[2],
       {color_view, smaa_weights_.view, smaa_weights_.view},
       dest_view,
       nullptr},
  };

  for (uint32_t pass_index = 0; pass_index < 3; ++pass_index) {
    const Pass& pass = passes[pass_index];
    if (pass.output_image) {
      // Se sobrescribe entera, asi que desde UNDEFINED vale si aun no existia.
      const bool was_read = pass.output_image->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      PushImageMemoryBarrier(
          pass.output_image->image, range,
          was_read ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, was_read ? VK_ACCESS_SHADER_READ_BIT : 0,
          VK_ACCESS_SHADER_WRITE_BIT, pass.output_image->layout, VK_IMAGE_LAYOUT_GENERAL);
      pass.output_image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    SubmitBarriers(true);

    VkDescriptorSet set = smaa_descriptor_sets_[frame_index][pass_index];
    VkDescriptorImageInfo image_infos[kSmaaBindingCount];
    VkWriteDescriptorSet writes[kSmaaBindingCount];
    for (uint32_t i = 0; i < kSmaaBindingCount; ++i) {
      VkDescriptorImageInfo& image_info = image_infos[i];
      if (i == kSmaaBindingSampler) {
        image_info.sampler = swap_sampler_linear_clamp_;
        image_info.imageView = VK_NULL_HANDLE;
        image_info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      } else if (i == kSmaaBindingDestination) {
        image_info.sampler = VK_NULL_HANDLE;
        image_info.imageView = pass.output;
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      } else {
        image_info.sampler = VK_NULL_HANDLE;
        image_info.imageView = pass.inputs[i];
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      VkWriteDescriptorSet& write = writes[i];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.pNext = nullptr;
      write.dstSet = set;
      write.dstBinding = i;
      write.dstArrayElement = 0;
      write.descriptorCount = 1;
      write.descriptorType = SmaaBindingType(i);
      write.pImageInfo = &image_info;
      write.pBufferInfo = nullptr;
      write.pTexelBufferView = nullptr;
    }
    dfn.vkUpdateDescriptorSets(device, kSmaaBindingCount, writes, 0, nullptr);

    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_COMPUTE, smaa_pipeline_layout_, 0, 1, &set, 0, nullptr);
    deferred_command_buffer_.CmdVkPushConstants(smaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                                                0, sizeof(constants), &constants);
    BindExternalComputePipeline(pass.pipeline);
    deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

    if (pass.output_image) {
      PushImageMemoryBarrier(pass.output_image->image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      pass.output_image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }

  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO("odisea: SMAA 1x active on the Vulkan swap output ({}x{})", width, height);
  }
}

}  // namespace rex::graphics::vulkan
