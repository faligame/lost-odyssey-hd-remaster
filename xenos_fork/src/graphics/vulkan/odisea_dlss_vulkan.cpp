// Fork (odisea): NVIDIA DLSS para la escena 3D en Vulkan (el mismo esquema que
// d3d12/odisea_dlss_d3d12.cpp, sin las ayudas opcionales: mascaras y movimiento
// propio de los personajes, que en D3D12 no hicieron falta).
//
// Recorrido de un fotograma con odisea_dlss y la interfaz aparte:
//   1. IssueDraw desplaza los dibujos de la escena con el jitter del fotograma
//      (odisea::dlss::GetJitter) y guarda la ViewProjection (c233-c236).
//   2. Cuando el juego resuelve la profundidad de la escena, DlssOnResolve lee la
//      EDRAM (FSI) con shaders/odisea_dlss_inputs.cs.hlsl (variante SPIR-V):
//      profundidad R32F y vectores de movimiento de la camara RG16F, a la escala
//      3D.
//   3. En el swap, la escena ya pasada por la rampa de gamma (hud_scene_image_)
//      va a DLSS (NGX, grabado con DeferredCommandBuffer::OdiseaCall porque NGX
//      necesita el VkCommandBuffer real) y su salida, a la resolucion de salida,
//      sustituye al reescalado bilineal de ComposeHud.
// NGX pide extensiones del dispositivo (VK_NVX_binary_import,
// VK_NVX_image_view_handle, VK_KHR_push_descriptor, bufferDeviceAddress): las
// activa el runtime (parche rexglue-0.10.0-vulkan-extensiones-dlss.patch del
// SDK). Entradas en SHADER_READ_ONLY_OPTIMAL y salida en GENERAL, como pide
// NVIDIA para Vulkan.
#include <rex/graphics/vulkan/command_processor.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/graphics/odisea_dlss.h>
#include <rex/graphics/odisea_render_scale.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#ifdef ODISEA_DLSS
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

REXCVAR_DECLARE(int32_t, odisea_dlss_preset);
REXCVAR_DECLARE(int32_t, odisea_dlss_jitter_sign);

namespace rex::graphics::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/odisea_dlss_inputs_cs.h"
}  // namespace shaders

namespace {

struct DlssInputsConstants {
  float inverse_view_projection[16];
  float previous_view_projection[16];
  float size_jitter[4];
  uint32_t edram[4];
  uint32_t edram_extra[4];
};

constexpr VkFormat kDlssDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kDlssMotionFormat = VK_FORMAT_R16G16_SFLOAT;

enum : uint32_t {
  kDlssBindingEdram,
  kDlssBindingDepth,
  kDlssBindingMotion,
  kDlssBindingCount,
};

// Inversa de una matriz 4x4 (orden fila, como las constantes del juego).
bool Invert4x4(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(std::abs(det) > 1e-20f)) return false;
  float inv_det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
  return true;
}

}  // namespace

bool VulkanCommandProcessor::InitializeDlss() {
#ifndef ODISEA_DLSS
  return false;
#else
  if (!odisea::dlss::Requested() || hud_compose_pipeline_ == VK_NULL_HANDLE ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kPixelShaderInterlock) {
    return false;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanInstance* const vulkan_instance = vulkan_device->vulkan_instance();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPhysicalDeviceProperties physical_properties = {};
  vulkan_instance->functions().vkGetPhysicalDeviceProperties(vulkan_device->physical_device(),
                                                             &physical_properties);
  if (physical_properties.limits.maxPushConstantsSize < sizeof(DlssInputsConstants)) {
    REXGPU_WARN("odisea DLSS: la tarjeta admite solo {} bytes de push constants",
                physical_properties.limits.maxPushConstantsSize);
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[kDlssBindingCount];
  for (uint32_t i = 0; i < kDlssBindingCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i == kDlssBindingEdram ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].pImmutableSamplers = nullptr;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_create_info = {};
  set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_create_info.bindingCount = kDlssBindingCount;
  set_layout_create_info.pBindings = bindings;
  VkDescriptorPoolSize pool_sizes[2];
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_sizes[0].descriptorCount = kMaxFramesInFlight;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = kMaxFramesInFlight * 2;
  VkDescriptorPoolCreateInfo pool_create_info = {};
  pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_create_info.maxSets = kMaxFramesInFlight;
  pool_create_info.poolSizeCount = 2;
  pool_create_info.pPoolSizes = pool_sizes;
  bool ok = dfn.vkCreateDescriptorSetLayout(device, &set_layout_create_info, nullptr,
                                            &dlss_descriptor_set_layout_) == VK_SUCCESS &&
            dfn.vkCreateDescriptorPool(device, &pool_create_info, nullptr,
                                       &dlss_descriptor_pool_) == VK_SUCCESS;
  if (ok) {
    VkDescriptorSetAllocateInfo set_allocate_info = {};
    set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_allocate_info.descriptorPool = dlss_descriptor_pool_;
    set_allocate_info.descriptorSetCount = 1;
    set_allocate_info.pSetLayouts = &dlss_descriptor_set_layout_;
    for (VkDescriptorSet& set : dlss_descriptor_sets_) {
      ok = ok && dfn.vkAllocateDescriptorSets(device, &set_allocate_info, &set) == VK_SUCCESS;
    }
  }
  if (ok) {
    VkPushConstantRange push_constant_range;
    push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(DlssInputsConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {};
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = 1;
    pipeline_layout_create_info.pSetLayouts = &dlss_descriptor_set_layout_;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &push_constant_range;
    ok = dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                    &dlss_pipeline_layout_) == VK_SUCCESS;
  }
  if (ok) {
    dlss_inputs_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, dlss_pipeline_layout_, shaders::odisea_dlss_inputs_cs,
        sizeof(shaders::odisea_dlss_inputs_cs));
    ok = dlss_inputs_pipeline_ != VK_NULL_HANDLE;
  }
  if (!ok) {
    REXGPU_ERROR("odisea DLSS: no se pudo crear el compute de las entradas (Vulkan)");
    ShutdownDlss();
    return false;
  }

  // NGX busca nvngx_dlss.dll junto al ejecutable; sus registros van ahi mismo.
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring data_path = std::filesystem::path(exe_path).parent_path().wstring();
  NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      "6f1c3b52-9d4e-4a7b-8e21-3c5d0a9f7b14", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
      data_path.c_str(), vulkan_instance->instance(), vulkan_device->physical_device(), device,
      vulkan_instance->functions().vkGetInstanceProcAddr, nullptr);
  if (NVSDK_NGX_FAILED(result)) {
    REXGPU_WARN("odisea DLSS: NGX no disponible en Vulkan (0x{:08X}): tarjeta o controlador "
                "sin DLSS",
                uint32_t(result));
    ShutdownDlss();
    return false;
  }
  NVSDK_NGX_Parameter* parameters = nullptr;
  result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
  if (NVSDK_NGX_FAILED(result) || !parameters) {
    REXGPU_WARN("odisea DLSS: sin parametros de NGX (0x{:08X})", uint32_t(result));
    NVSDK_NGX_VULKAN_Shutdown1(device);
    ShutdownDlss();
    return false;
  }
  dlss_parameters_ = parameters;
  int available = 0;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(
          parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
      !available) {
    int needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             &needs_driver);
    REXGPU_WARN("odisea DLSS: DLSS no disponible en Vulkan con esta tarjeta o controlador "
                "(controlador antiguo: {}; falta nvngx_dlss.dll junto al ejecutable?)",
                needs_driver != 0);
    ShutdownDlss();
    return false;
  }
  dlss_available_ = true;
  dlss_reset_ = true;
  REXGPU_INFO("odisea DLSS: listo (Vulkan)");
  return true;
#endif
}

void VulkanCommandProcessor::ShutdownDlss() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
#ifdef ODISEA_DLSS
  if (dlss_feature_) {
    NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(dlss_feature_));
    dlss_feature_ = nullptr;
  }
  if (dlss_parameters_) {
    NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(dlss_parameters_));
    dlss_parameters_ = nullptr;
    NVSDK_NGX_VULKAN_Shutdown1(device);
  }
#endif
  dlss_available_ = false;
  dlss_frame_active_ = false;
  dlss_inputs_ready_ = false;
  for (HudImage* image : {&dlss_depth_image_, &dlss_motion_image_, &dlss_output_image_}) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image->view);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image->image);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image->memory);
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->width = image->height = 0;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, dlss_inputs_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dlss_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         dlss_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         dlss_descriptor_set_layout_);
  dlss_descriptor_sets_.fill(VK_NULL_HANDLE);
}

void VulkanCommandProcessor::DlssOnResolve() {
  if (!dlss_available_ || !dlss_frame_active_ || dlss_inputs_ready_) return;
  const RegisterFile& regs = *register_file_;
  // Copia de profundidad (seleccion 4) de la superficie principal sin MSAA.
  if ((regs[XE_GPU_REG_RB_COPY_CONTROL] & 7) < 4) return;
  uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  uint32_t pitch = surface_info & 0x3FFF;
  if (pitch < 1024 || pitch > 1280 || ((surface_info >> 16) & 3) != 0) return;
  uint32_t dest_height = (regs[XE_GPU_REG_RB_COPY_DEST_PITCH] >> 16) & 0x3FFF;
  if (dest_height < 256 || dest_height > 720) return;
  float current[16], previous[16], inverse[16];
  if (!odisea::dlss::GetMatrices(current, previous) || !Invert4x4(current, inverse)) return;

  uint32_t scale_q = odisea::RenderScaleQuarters(texture_cache_->draw_resolution_scale_x());
  uint32_t width = pitch * scale_q / 4, height = dest_height * scale_q / 4;
  constexpr VkImageUsageFlags kInputUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!EnsureHudImage(dlss_depth_image_, kDlssDepthFormat, width, height, kInputUsage) ||
      !EnsureHudImage(dlss_motion_image_, kDlssMotionFormat, width, height, kInputUsage)) {
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  // Se sobrescriben enteras: lo de antes da igual (pero DLSS pudo leerlas en
  // el fotograma anterior).
  for (HudImage* image : {&dlss_depth_image_, &dlss_motion_image_}) {
    PushImageMemoryBarrier(image->image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL);
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
  }
  render_target_cache_->OdiseaPrepareEdramRead();
  SubmitBarriers(true);

  VkDescriptorSet set = dlss_descriptor_sets_[frame_current_ % kMaxFramesInFlight];
  VkDescriptorBufferInfo buffer_info;
  buffer_info.buffer = render_target_cache_->edram_buffer();
  buffer_info.offset = 0;
  buffer_info.range = VK_WHOLE_SIZE;
  VkDescriptorImageInfo image_infos[2] = {};
  image_infos[0].imageView = dlss_depth_image_.view;
  image_infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  image_infos[1].imageView = dlss_motion_image_.view;
  image_infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[kDlssBindingCount];
  for (uint32_t i = 0; i < kDlssBindingCount; ++i) {
    writes[i] = {};
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    if (i == kDlssBindingEdram) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffer_info;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &image_infos[i - 1];
    }
  }
  dfn.vkUpdateDescriptorSets(device, kDlssBindingCount, writes, 0, nullptr);

  DlssInputsConstants constants;
  std::memcpy(constants.inverse_view_projection, inverse, sizeof(inverse));
  std::memcpy(constants.previous_view_projection, previous, sizeof(previous));
  float jitter_x, jitter_y;
  odisea::dlss::GetJitter(jitter_x, jitter_y);
  constants.size_jitter[0] = float(width);
  constants.size_jitter[1] = float(height);
  constants.size_jitter[2] = jitter_x;
  constants.size_jitter[3] = jitter_y;
  uint32_t depth_info = regs[XE_GPU_REG_RB_DEPTH_INFO];
  constants.edram[0] = depth_info & 0xFFF;
  constants.edram[1] = (pitch + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
  constants.edram[2] = scale_q;
  constants.edram[3] = (depth_info >> 16) & 1;  // D24FS8
  constants.edram_extra[0] = xenos::kEdramTileCount;
  float depth_scale, depth_offset;
  odisea::dlss::GetDepthMapping(depth_scale, depth_offset);
  std::memcpy(&constants.edram_extra[1], &depth_scale, sizeof(float));
  std::memcpy(&constants.edram_extra[2], &depth_offset, sizeof(float));
  constants.edram_extra[3] = 0;
  dlss_depth_inverted_ = depth_scale < 0.0f;

  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   dlss_pipeline_layout_, 0, 1, &set, 0, nullptr);
  deferred_command_buffer_.CmdVkPushConstants(dlss_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                                              0, sizeof(constants), &constants);
  BindExternalComputePipeline(dlss_inputs_pipeline_);
  deferred_command_buffer_.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO("odisea DLSS: primeras entradas (Vulkan) {}x{}, profundidad {}", width, height,
                dlss_depth_inverted_ ? "invertida" : "normal");
  }
  dlss_inputs_ready_ = true;
  dlss_inputs_size_[0] = width;
  dlss_inputs_size_[1] = height;
}

void VulkanCommandProcessor::DlssCommandBufferCallback(void* context,
                                                       VkCommandBuffer command_buffer) {
#ifdef ODISEA_DLSS
  auto& request = *static_cast<DlssCallbackRequest*>(context);
  VulkanCommandProcessor& processor = *request.processor;
  auto* parameters = static_cast<NVSDK_NGX_Parameter*>(processor.dlss_parameters_);
  if (!parameters) return;
  if (request.create) {
    float ratio = float(request.size[2]) / float(std::max(request.size[0], 1u));
    NVSDK_NGX_PerfQuality_Value quality =
        ratio <= 1.01f   ? NVSDK_NGX_PerfQuality_Value_DLAA
        : ratio <= 1.55f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
        : ratio <= 1.80f ? NVSDK_NGX_PerfQuality_Value_Balanced
        : ratio <= 2.20f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                         : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    // Modelo pedido (0 = el de NVIDIA para cada modo).
    unsigned int preset = unsigned(std::clamp(request.preset, 0, 15));
    for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance}) {
      NVSDK_NGX_Parameter_SetUI(parameters, name, preset);
    }
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = request.size[0];
    create.Feature.InHeight = request.size[1];
    create.Feature.InTargetWidth = request.size[2];
    create.Feature.InTargetHeight = request.size[3];
    create.Feature.InPerfQualityValue = quality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (request.depth_inverted) {
      create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    }
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSS_EXT(command_buffer, 1, 1, &handle, parameters, &create);
    if (NVSDK_NGX_FAILED(result) || !handle) {
      REXGPU_ERROR("odisea DLSS: no se pudo crear el efecto {}x{} -> {}x{} (0x{:08X})",
                   request.size[0], request.size[1], request.size[2], request.size[3],
                   uint32_t(result));
      processor.dlss_available_ = false;
      return;
    }
    processor.dlss_feature_ = handle;
    processor.dlss_feature_preset_ = request.preset;
    processor.dlss_feature_depth_inverted_ = request.depth_inverted;
    std::memcpy(processor.dlss_feature_size_, request.size, sizeof(request.size));
    REXGPU_INFO(
        "odisea DLSS: efecto creado (Vulkan), {}x{} -> {}x{} (modo {}, modelo {}, profundidad "
        "{})",
        request.size[0], request.size[1], request.size[2], request.size[3], int(quality),
        request.preset, request.depth_inverted ? "invertida" : "normal");
    return;
  }
  if (!processor.dlss_feature_) return;
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  auto resource = [&range](const DlssImageRef& ref, bool read_write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(ref.view, ref.image, range, ref.format,
                                                  ref.width, ref.height, read_write);
  };
  NVSDK_NGX_Resource_VK color = resource(request.color, false);
  NVSDK_NGX_Resource_VK depth = resource(request.depth, false);
  NVSDK_NGX_Resource_VK motion = resource(request.motion, false);
  NVSDK_NGX_Resource_VK output = resource(request.output, true);
  NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = &color;
  eval.Feature.pInOutput = &output;
  eval.pInDepth = &depth;
  eval.pInMotionVectors = &motion;
  eval.InJitterOffsetX = request.jitter[0];
  eval.InJitterOffsetY = request.jitter[1];
  eval.InRenderSubrectDimensions.Width = request.size[0];
  eval.InRenderSubrectDimensions.Height = request.size[1];
  eval.InReset = request.reset ? 1 : 0;
  eval.InMVScaleX = 1.0f;
  eval.InMVScaleY = 1.0f;
  NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(
      command_buffer, static_cast<NVSDK_NGX_Handle*>(processor.dlss_feature_), parameters, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    static int errors = 0;
    if (errors++ < 5) {
      REXGPU_ERROR("odisea DLSS: fallo al evaluar en Vulkan (0x{:08X})", uint32_t(result));
    }
  }
#endif
}

VulkanCommandProcessor::HudImage* VulkanCommandProcessor::DlssEvaluate(uint32_t scene_width,
                                                                       uint32_t scene_height,
                                                                       uint32_t width,
                                                                       uint32_t height) {
  HudImage* output = nullptr;
  bool ngx_called = false;
  float jitter_x, jitter_y;
  odisea::dlss::GetJitter(jitter_x, jitter_y);
  int32_t preset = REXCVAR_GET(odisea_dlss_preset);
  bool feature_matches = dlss_feature_ && dlss_feature_preset_ == preset &&
                         dlss_feature_depth_inverted_ == dlss_depth_inverted_ &&
                         dlss_feature_size_[0] == scene_width &&
                         dlss_feature_size_[1] == scene_height &&
                         dlss_feature_size_[2] == width && dlss_feature_size_[3] == height;
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  if (dlss_available_ && !feature_matches) {
#ifdef ODISEA_DLSS
    if (dlss_feature_) {
      // Cambio de tamano o de modelo (raro): esperar a la GPU antes de soltar
      // el efecto (el ultimo uso esta en un envio anterior).
      AwaitAllQueueOperationsCompletion();
      NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(dlss_feature_));
      dlss_feature_ = nullptr;
    }
#endif
    dlss_request_ = {};
    dlss_request_.processor = this;
    dlss_request_.create = true;
    dlss_request_.preset = preset;
    dlss_request_.depth_inverted = dlss_depth_inverted_;
    dlss_request_.size[0] = scene_width;
    dlss_request_.size[1] = scene_height;
    dlss_request_.size[2] = width;
    dlss_request_.size[3] = height;
    SubmitBarriers(true);
    deferred_command_buffer_.OdiseaCall(&DlssCommandBufferCallback, &dlss_request_);
    ngx_called = true;
    dlss_reset_ = true;
  } else if (dlss_available_ && dlss_frame_active_ && dlss_inputs_ready_ &&
             dlss_inputs_size_[0] == scene_width && dlss_inputs_size_[1] == scene_height &&
             hud_scene_image_.image != VK_NULL_HANDLE && hud_scene_image_.width == scene_width &&
             hud_scene_image_.height == scene_height &&
             EnsureHudImage(dlss_output_image_, ui::vulkan::VulkanPresenter::kGuestOutputFormat,
                            width, height,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
    // Entradas para lectura; la salida, en GENERAL para que DLSS escriba.
    for (HudImage* image : {&hud_scene_image_, &dlss_depth_image_, &dlss_motion_image_}) {
      if (image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        PushImageMemoryBarrier(image->image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, image->layout,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
    }
    PushImageMemoryBarrier(dlss_output_image_.image, range, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL);
    dlss_output_image_.layout = VK_IMAGE_LAYOUT_GENERAL;
    SubmitBarriers(true);
    auto ref = [](const HudImage& image, VkFormat format) {
      return DlssImageRef{image.image, image.view, format, image.width, image.height};
    };
    int32_t sign = REXCVAR_GET(odisea_dlss_jitter_sign);
    dlss_request_ = {};
    dlss_request_.processor = this;
    dlss_request_.create = false;
    dlss_request_.size[0] = scene_width;
    dlss_request_.size[1] = scene_height;
    dlss_request_.size[2] = width;
    dlss_request_.size[3] = height;
    dlss_request_.color = ref(hud_scene_image_, ui::vulkan::VulkanPresenter::kGuestOutputFormat);
    dlss_request_.depth = ref(dlss_depth_image_, kDlssDepthFormat);
    dlss_request_.motion = ref(dlss_motion_image_, kDlssMotionFormat);
    dlss_request_.output =
        ref(dlss_output_image_, ui::vulkan::VulkanPresenter::kGuestOutputFormat);
    dlss_request_.jitter[0] = (sign & 1) ? -jitter_x : jitter_x;
    dlss_request_.jitter[1] = (sign & 2) ? -jitter_y : jitter_y;
    dlss_request_.reset = dlss_reset_;
    deferred_command_buffer_.OdiseaCall(&DlssCommandBufferCallback, &dlss_request_);
    ngx_called = true;
    dlss_reset_ = false;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXGPU_INFO("odisea DLSS: primera evaluacion (Vulkan)");
    }
    output = &dlss_output_image_;
  }
  if (ngx_called) {
    // NGX enlaza sus propios pipelines, sets y constantes: olvidar lo enlazado.
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;
  }

  // Fotograma siguiente: con el efecto creado (o creandose en este envio), jitter.
  odisea::dlss::EndFrame();
  dlss_inputs_ready_ = false;
  dlss_frame_active_ = dlss_available_;
  odisea::dlss::SetFrame(dlss_frame_active_, scene_width, scene_height, width, height);
  return output;
}

void VulkanCommandProcessor::DlssEndFrameWithoutScene() {
  if (!dlss_frame_active_) return;
  // DLSS necesita la escena aparte: sin ella, nada de jitter.
  dlss_frame_active_ = false;
  dlss_inputs_ready_ = false;
  odisea::dlss::EndFrame();
  odisea::dlss::SetFrame(false, 0, 0, 0, 0);
}

}  // namespace rex::graphics::vulkan
