// Fork (odisea): NVIDIA DLSS para la escena 3D en D3D12.
//
// Recorrido de un fotograma con odisea_dlss y la interfaz aparte:
//   1. IssueDraw desplaza los dibujos de la escena con el jitter del fotograma
//      (odisea::dlss::GetJitter) y guarda la ViewProjection (c233-c236).
//   2. Cuando el juego resuelve la profundidad de la escena, DlssOnResolve lee la
//      EDRAM con shaders/odisea_dlss_inputs.cs.hlsl: profundidad R32F y vectores
//      de movimiento de la camara R16G16F, a la escala 3D.
//   3. En el swap, la escena ya pasada por la rampa de gamma (hud_scene_texture_)
//      va a DLSS (NGX, grabado con DeferredCommandList::OdiseaCall porque NGX
//      necesita la lista de comandos real) y su salida, a la resolucion de
//      salida, sustituye al reescalado bilineal de ComposeHud.
// El efecto de DLSS se crea en una llamada de un envio y se usa desde el
// siguiente; el modo sale de la relacion entre la escala 3D y la salida.
// SDK NGX de NVIDIA (../sdk_externos/DLSS, ODISEA_DLSS); la dll nvngx_dlss.dll
// va junto al ejecutable.
#include <rex/graphics/d3d12/command_processor.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/graphics/odisea_dlss.h>
#include <rex/graphics/odisea_render_scale.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

#ifdef ODISEA_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

REXCVAR_DEFINE_INT32(odisea_dlss_preset, 0, "GPU",
                     "odisea: modelo de DLSS (en caliente): 0 = el de NVIDIA para cada modo "
                     "(Transformer K en Calidad/DLAA), 10 = J (algo menos de estela, algo mas "
                     "de parpadeo), 11 = K, 12 = L, 13 = M.");
REXCVAR_DEFINE_BOOL(odisea_dlss_reactive, false, "GPU",
                    "odisea: mascara reactiva de DLSS (en caliente): marca lo que no cuadra con "
                    "el fotograma anterior segun los vectores de camara (personaje que la camara "
                    "sigue, sombras...) para que DLSS no lo arrastre.");
REXCVAR_DEFINE_DOUBLE(odisea_dlss_reactive_margin, 0.04, "GPU/Debug",
                      "odisea: margen de color de la mascara reactiva (mas = marca menos).");
REXCVAR_DEFINE_DOUBLE(odisea_dlss_reactive_gain, 8.0, "GPU/Debug",
                      "odisea: ganancia de la mascara reactiva (mas = marca con mas fuerza).");
REXCVAR_DEFINE_INT32(odisea_dlss_jitter_sign, 0, "GPU/Debug",
                     "odisea: signo del jitter que se pasa a DLSS (bit 0 = invertir x, bit 1 = "
                     "invertir y). Para probar si la imagen tiembla.");

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/odisea_dlss_inputs_cs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_dlss_mask_cs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_dlss_reactive_cs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_dlss_object_motion_vs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_dlss_object_motion_ps.h"
}  // namespace shaders

namespace {

struct DlssInputsConstants {
  float inverse_view_projection[16];
  float previous_view_projection[16];
  float size_jitter[4];
  uint32_t edram[4];
  uint32_t edram_extra[4];
};

constexpr DXGI_FORMAT kDlssDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kDlssMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDlssMaskFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDlssMaskR8Format = DXGI_FORMAT_R8_UNORM;
// Captura de vertices de los personajes: hasta 2 millones de vertices por
// fotograma (32 MB por bufer, dos) y 8192 dibujos.
constexpr uint32_t kDlssSoMaxVertices = 2u << 20;
constexpr uint32_t kDlssSoMaxSlots = 8192;

struct ObjectMotionConstants {
  float size_jitter[4];
  float previous_jitter[4];
  uint32_t bases[4];
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

bool D3D12CommandProcessor::InitializeDlss() {
#ifndef ODISEA_DLSS
  return false;
#else
  if (!odisea::dlss::Requested() ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kPixelShaderInterlock) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_ROOT_PARAMETER root_parameters[UINT(DlssInputsRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants = root_parameters[UINT(DlssInputsRootParameter::kConstants)];
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 0;
    constants.Constants.RegisterSpace = 0;
    constants.Constants.Num32BitValues = sizeof(DlssInputsConstants) / sizeof(uint32_t);
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE ranges[3];
  for (uint32_t i = 0; i < 3; ++i) {
    D3D12_DESCRIPTOR_RANGE& range = ranges[i];
    range.RangeType = i == 0 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = i == 0 ? 0 : i - 1;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& table = root_parameters[UINT(DlssInputsRootParameter::kEdram) + i];
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable.NumDescriptorRanges = 1;
    table.DescriptorTable.pDescriptorRanges = &range;
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
  root_signature_desc.NumParameters = UINT(DlssInputsRootParameter::kCount);
  root_signature_desc.pParameters = root_parameters;
  *(dlss_inputs_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!dlss_inputs_root_signature_) {
    REXGPU_ERROR("odisea DLSS: failed to create the inputs root signature");
    ShutdownDlss();
    return false;
  }
  *(dlss_inputs_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::odisea_dlss_inputs_cs, sizeof(shaders::odisea_dlss_inputs_cs),
      dlss_inputs_root_signature_.Get());
  if (!dlss_inputs_pipeline_) {
    REXGPU_ERROR("odisea DLSS: failed to create the inputs pipeline");
    ShutdownDlss();
    return false;
  }
  // Mascara reactiva: constantes + 4 SRV + 1 UAV.
  {
    D3D12_ROOT_PARAMETER reactive_parameters[6];
    D3D12_DESCRIPTOR_RANGE reactive_ranges[5];
    reactive_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    reactive_parameters[0].Constants.ShaderRegister = 0;
    reactive_parameters[0].Constants.RegisterSpace = 0;
    reactive_parameters[0].Constants.Num32BitValues = 4;
    reactive_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (uint32_t i = 0; i < 5; ++i) {
      D3D12_DESCRIPTOR_RANGE& range = reactive_ranges[i];
      range.RangeType = i < 4 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      range.NumDescriptors = 1;
      range.BaseShaderRegister = i < 4 ? i : 0;
      range.RegisterSpace = 0;
      range.OffsetInDescriptorsFromTableStart = 0;
      D3D12_ROOT_PARAMETER& table = reactive_parameters[1 + i];
      table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      table.DescriptorTable.NumDescriptorRanges = 1;
      table.DescriptorTable.pDescriptorRanges = &range;
      table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC reactive_desc = {};
    reactive_desc.NumParameters = 6;
    reactive_desc.pParameters = reactive_parameters;
    *(dlss_reactive_root_signature_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(provider, reactive_desc);
    if (dlss_reactive_root_signature_) {
      *(dlss_reactive_pipeline_.ReleaseAndGetAddressOf()) =
          ui::d3d12::util::CreateComputePipeline(device, shaders::odisea_dlss_reactive_cs,
                                                 sizeof(shaders::odisea_dlss_reactive_cs),
                                                 dlss_reactive_root_signature_.Get());
    }
  }
  // Movimiento de los personajes (opcional): buferes de captura, contadores y
  // el pipeline que dibuja sus vectores.
  {
    bool ok = true;
    auto buffer_desc = [](uint64_t size) {
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = size;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      return desc;
    };
    for (uint32_t i = 0; i < 2 && ok; ++i) {
      D3D12_RESOURCE_DESC desc = buffer_desc(uint64_t(kDlssSoMaxVertices) * 16);
      ok = SUCCEEDED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&dlss_so_buffers_[i])));
      dlss_so_buffer_states_[i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      desc = buffer_desc(uint64_t(kDlssSoMaxSlots) * 4);
      ok = ok && SUCCEEDED(device->CreateCommittedResource(
                     &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                     &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&dlss_so_counters_[i])));
      dlss_so_counter_states_[i] = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (ok) {
      D3D12_RESOURCE_DESC desc = buffer_desc(uint64_t(kDlssSoMaxSlots) * 4);
      ok = SUCCEEDED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, provider.GetHeapFlagCreateNotZeroed(), &desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&dlss_so_zero_)));
      void* mapping = nullptr;
      D3D12_RANGE no_read = {0, 0};
      if (ok && SUCCEEDED(dlss_so_zero_->Map(0, &no_read, &mapping)) && mapping) {
        std::memset(mapping, 0, size_t(kDlssSoMaxSlots) * 4);
        dlss_so_zero_->Unmap(0, nullptr);
      } else {
        ok = false;
      }
    }
    if (ok) {
      D3D12_ROOT_PARAMETER om_parameters[4];
      D3D12_DESCRIPTOR_RANGE om_ranges[3];
      om_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      om_parameters[0].Constants.ShaderRegister = 0;
      om_parameters[0].Constants.RegisterSpace = 0;
      om_parameters[0].Constants.Num32BitValues = sizeof(ObjectMotionConstants) / sizeof(uint32_t);
      om_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      for (uint32_t i = 0; i < 3; ++i) {
        D3D12_DESCRIPTOR_RANGE& range = om_ranges[i];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = i;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER& table = om_parameters[1 + i];
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable.NumDescriptorRanges = 1;
        table.DescriptorTable.pDescriptorRanges = &range;
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      D3D12_ROOT_SIGNATURE_DESC om_desc = {};
      om_desc.NumParameters = 4;
      om_desc.pParameters = om_parameters;
      *(dlss_om_root_signature_.ReleaseAndGetAddressOf()) =
          ui::d3d12::util::CreateRootSignature(provider, om_desc);
      ok = dlss_om_root_signature_ != nullptr;
    }
    if (ok) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
      pso.pRootSignature = dlss_om_root_signature_.Get();
      pso.VS.pShaderBytecode = shaders::odisea_dlss_object_motion_vs;
      pso.VS.BytecodeLength = sizeof(shaders::odisea_dlss_object_motion_vs);
      pso.PS.pShaderBytecode = shaders::odisea_dlss_object_motion_ps;
      pso.PS.BytecodeLength = sizeof(shaders::odisea_dlss_object_motion_ps);
      pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pso.SampleMask = UINT_MAX;
      pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pso.RasterizerState.DepthClipEnable = TRUE;
      pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pso.NumRenderTargets = 1;
      pso.RTVFormats[0] = kDlssMotionFormat;
      pso.SampleDesc.Count = 1;
      ok = SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&dlss_om_pipeline_)));
    }
    if (ok) {
      D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
      rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      rtv_heap_desc.NumDescriptors = 1;
      ok = SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc,
                                                  IID_PPV_ARGS(&dlss_motion_rtv_heap_)));
    }
    dlss_object_motion_ready_ = ok;
    if (!ok) {
      REXGPU_WARN("odisea DLSS: sin movimiento propio de los personajes");
    }
  }
  // La mascara de efectos es opcional: sin ella DLSS funciona igual.
  *(dlss_mask_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::odisea_dlss_mask_cs, sizeof(shaders::odisea_dlss_mask_cs),
      dlss_inputs_root_signature_.Get());
  D3D12_DESCRIPTOR_HEAP_DESC mask_rtv_heap_desc = {};
  mask_rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  mask_rtv_heap_desc.NumDescriptors = 1;
  if (FAILED(device->CreateDescriptorHeap(&mask_rtv_heap_desc,
                                          IID_PPV_ARGS(&dlss_mask_rtv_heap_)))) {
    dlss_mask_pipeline_.Reset();
  }

  // NGX busca nvngx_dlss.dll junto al ejecutable; sus registros van ahi mismo.
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring data_path = std::filesystem::path(exe_path).parent_path().wstring();
  NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
      "6f1c3b52-9d4e-4a7b-8e21-3c5d0a9f7b14", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
      data_path.c_str(), device);
  if (NVSDK_NGX_FAILED(result)) {
    REXGPU_WARN("odisea DLSS: NGX no disponible (0x{:08X}): tarjeta o controlador sin DLSS",
                uint32_t(result));
    ShutdownDlss();
    return false;
  }
  NVSDK_NGX_Parameter* parameters = nullptr;
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);
  if (NVSDK_NGX_FAILED(result) || !parameters) {
    REXGPU_WARN("odisea DLSS: sin parametros de NGX (0x{:08X})", uint32_t(result));
    NVSDK_NGX_D3D12_Shutdown1(device);
    ShutdownDlss();
    return false;
  }
  dlss_parameters_ = parameters;
  int available = 0;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(
          parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
      !available) {
    REXGPU_WARN("odisea DLSS: DLSS no disponible en esta tarjeta o con este controlador "
                "(falta nvngx_dlss.dll junto al ejecutable?)");
    ShutdownDlss();
    return false;
  }
  dlss_available_ = true;
  dlss_reset_ = true;
  REXGPU_INFO("odisea DLSS: listo");
  return true;
#endif
}

void D3D12CommandProcessor::ShutdownDlss() {
#ifdef ODISEA_DLSS
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (dlss_feature_) {
    NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(dlss_feature_));
    dlss_feature_ = nullptr;
  }
  if (dlss_parameters_) {
    NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(dlss_parameters_));
    dlss_parameters_ = nullptr;
    NVSDK_NGX_D3D12_Shutdown1(device);
  }
#endif
  dlss_available_ = false;
  dlss_frame_active_ = false;
  dlss_inputs_ready_ = false;
  dlss_output_texture_.Reset();
  dlss_mask_texture_.Reset();
  dlss_mask_r8_texture_.Reset();
  dlss_mask_rtv_heap_.Reset();
  dlss_mask_pipeline_.Reset();
  dlss_mask_has_content_ = false;
  dlss_previous_color_texture_.Reset();
  dlss_previous_color_valid_ = false;
  dlss_reactive_pipeline_.Reset();
  dlss_reactive_root_signature_.Reset();
  dlss_object_motion_ready_ = false;
  for (uint32_t i = 0; i < 2; ++i) {
    dlss_so_buffers_[i].Reset();
    dlss_so_counters_[i].Reset();
  }
  dlss_so_zero_.Reset();
  dlss_om_pipeline_.Reset();
  dlss_om_root_signature_.Reset();
  dlss_motion_rtv_heap_.Reset();
  dlss_so_entries_.clear();
  dlss_so_previous_.clear();
  dlss_so_targets_.clear();
  dlss_so_frame_started_ = false;
  dlss_motion_texture_.Reset();
  dlss_depth_texture_.Reset();
  dlss_inputs_pipeline_.Reset();
  dlss_inputs_root_signature_.Reset();
}

bool D3D12CommandProcessor::EnsureDlssTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                                              DXGI_FORMAT format, uint32_t width,
                                              uint32_t height, D3D12_RESOURCE_STATES state,
                                              D3D12_RESOURCE_FLAGS extra_flags) {
  if (texture) {
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Width == width && desc.Height == height) return true;
    if (submission_completed_ < dlss_textures_submission_) {
      texture->AddRef();
      resources_for_deletion_.emplace_back(dlss_textures_submission_, texture.Get());
    }
    texture.Reset();
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | extra_flags;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          state, nullptr, IID_PPV_ARGS(&texture)))) {
    REXGPU_ERROR("odisea DLSS: failed to create a {}x{} texture", width, height);
    return false;
  }
  return true;
}

bool D3D12CommandProcessor::BeginDlssMaskDraw(uint32_t width, uint32_t height) {
  if (!dlss_mask_rtv_heap_) return false;
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  bool recreated = false;
  if (dlss_mask_texture_) {
    D3D12_RESOURCE_DESC desc = dlss_mask_texture_->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      if (submission_completed_ < dlss_textures_submission_) {
        dlss_mask_texture_->AddRef();
        resources_for_deletion_.emplace_back(dlss_textures_submission_, dlss_mask_texture_.Get());
      }
      dlss_mask_texture_.Reset();
    }
  }
  if (!dlss_mask_texture_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kDlssMaskFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = kDlssMaskFormat;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            IID_PPV_ARGS(&dlss_mask_texture_)))) {
      REXGPU_ERROR("odisea DLSS: failed to create the {}x{} effects mask", width, height);
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = kDlssMaskFormat;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(dlss_mask_texture_.Get(), &rtv_desc,
                                   dlss_mask_rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    recreated = true;
  }
  dlss_textures_submission_ = submission_current_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = dlss_mask_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  SubmitBarriers();
  if (!dlss_mask_has_content_ || recreated) {
    static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    deferred_command_list_.D3DClearRenderTargetView(rtv, kZero, 0, nullptr);
    dlss_mask_has_content_ = true;
  }
  deferred_command_list_.D3DOMSetRenderTargets(1, &rtv, TRUE, nullptr);
  return true;
}

void D3D12CommandProcessor::DlssStreamOutCallback(void* context,
                                                  ID3D12GraphicsCommandList* command_list) {
  auto& target = *static_cast<DlssSoTarget*>(context);
  if (target.set) {
    command_list->SOSetTargets(0, 1, &target.view);
  } else {
    command_list->SOSetTargets(0, 0, nullptr);
  }
}

bool D3D12CommandProcessor::BeginDlssStreamOut(uint32_t vertex_count, uint64_t key) {
  if (!dlss_object_motion_ready_ || !vertex_count) return false;
  uint32_t current = dlss_so_current_;
  if (!dlss_so_frame_started_) {
    // Primer personaje del fotograma: los contextos del anterior ya se han
    // ejecutado (el envio del swap termina antes de volver).
    dlss_so_targets_.clear();
    dlss_so_entries_.clear();
    dlss_so_occurrences_.clear();
    dlss_so_used_vertices_ = 0;
    dlss_so_used_slots_ = 0;
    PushTransitionBarrier(dlss_so_counters_[current].Get(), dlss_so_counter_states_[current],
                          D3D12_RESOURCE_STATE_COPY_DEST);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(dlss_so_counters_[current].Get(), 0,
                                               dlss_so_zero_.Get(), 0,
                                               uint64_t(kDlssSoMaxSlots) * 4);
    PushTransitionBarrier(dlss_so_counters_[current].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_STREAM_OUT);
    dlss_so_counter_states_[current] = D3D12_RESOURCE_STATE_STREAM_OUT;
    dlss_so_frame_started_ = true;
  }
  if (dlss_so_used_vertices_ + vertex_count > kDlssSoMaxVertices ||
      dlss_so_used_slots_ >= kDlssSoMaxSlots) {
    return false;
  }
  PushTransitionBarrier(dlss_so_buffers_[current].Get(), dlss_so_buffer_states_[current],
                        D3D12_RESOURCE_STATE_STREAM_OUT);
  dlss_so_buffer_states_[current] = D3D12_RESOURCE_STATE_STREAM_OUT;
  // El mismo dibujo puede repetirse en un fotograma: la clave lleva su orden.
  uint32_t occurrence = dlss_so_occurrences_[key]++;
  uint64_t full_key = key ^ (uint64_t(occurrence + 1) * 0xC2B2AE3D27D4EB4Full);
  DlssSoTarget& target = dlss_so_targets_.emplace_back();
  target.set = true;
  target.view.BufferLocation =
      dlss_so_buffers_[current]->GetGPUVirtualAddress() + uint64_t(dlss_so_used_vertices_) * 16;
  target.view.SizeInBytes = uint64_t(vertex_count) * 16;
  target.view.BufferFilledSizeLocation =
      dlss_so_counters_[current]->GetGPUVirtualAddress() + uint64_t(dlss_so_used_slots_) * 4;
  SubmitBarriers();
  deferred_command_list_.OdiseaCall(&DlssStreamOutCallback, &target);
  dlss_so_entries_.push_back({full_key, dlss_so_used_vertices_, vertex_count});
  dlss_so_used_vertices_ += vertex_count;
  ++dlss_so_used_slots_;
  return true;
}

void D3D12CommandProcessor::EndDlssStreamOut() {
  static DlssSoTarget unset = {{}, false};
  deferred_command_list_.OdiseaCall(&DlssStreamOutCallback, &unset);
}

void D3D12CommandProcessor::DlssDrawObjectMotion(uint32_t width, uint32_t height) {
  if (!dlss_object_motion_ready_ || dlss_so_entries_.empty() || dlss_so_previous_.empty()) {
    return;
  }
  uint32_t current = dlss_so_current_, previous = current ^ 1;
  if (dlss_so_buffer_states_[previous] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
    return;
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[3];
  if (!RequestOneUseSingleViewDescriptors(3, descriptors)) return;
  // A partir de aqui nada que cambie el heap de descriptores.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  for (uint32_t i = 0; i < 2; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kDlssSoMaxVertices;
    device->CreateShaderResourceView(dlss_so_buffers_[i == 0 ? current : previous].Get(), &srv,
                                     descriptors[i].first);
  }
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = kDlssDepthFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(dlss_depth_texture_.Get(), &srv, descriptors[2].first);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = dlss_motion_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  {
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = kDlssMotionFormat;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(dlss_motion_texture_.Get(), &rtv_desc, rtv);
  }

  PushTransitionBarrier(dlss_so_buffers_[current].Get(), dlss_so_buffer_states_[current],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dlss_so_buffer_states_[current] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  PushTransitionBarrier(dlss_motion_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  PushTransitionBarrier(dlss_depth_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  SetExternalGraphicsRootSignature(dlss_om_root_signature_.Get());
  for (uint32_t i = 0; i < 3; ++i) {
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(1 + i, descriptors[i].second);
  }
  SetExternalPipeline(dlss_om_pipeline_.Get());
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  SetViewport(viewport);
  D3D12_RECT scissor = {0, 0, LONG(width), LONG(height)};
  SetScissorRect(scissor);
  SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  deferred_command_list_.D3DOMSetRenderTargets(1, &rtv, TRUE, nullptr);
  SubmitBarriers();
  ObjectMotionConstants constants = {};
  constants.size_jitter[0] = float(width);
  constants.size_jitter[1] = float(height);
  constants.size_jitter[2] = dlss_jitter_ndc_[0];
  constants.size_jitter[3] = dlss_jitter_ndc_[1];
  constants.previous_jitter[0] = dlss_previous_jitter_ndc_[0];
  constants.previous_jitter[1] = dlss_previous_jitter_ndc_[1];
  constants.previous_jitter[2] = dlss_depth_inverted_ ? 1.0f : 0.0f;
  uint32_t drawn = 0;
  for (const DlssSoEntry& entry : dlss_so_entries_) {
    auto it = dlss_so_previous_.find(entry.key);
    if (it == dlss_so_previous_.end() || it->second.count != entry.count) continue;
    constants.bases[0] = entry.base;
    constants.bases[1] = it->second.base;
    deferred_command_list_.D3DSetGraphicsRoot32BitConstants(
        0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
    deferred_command_list_.D3DDrawInstanced(entry.count, 1, 0, 0);
    ++drawn;
  }
  deferred_command_list_.D3DOMSetRenderTargets(0, nullptr, FALSE, nullptr);
  PushTransitionBarrier(dlss_motion_texture_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  PushTransitionBarrier(dlss_depth_texture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  static uint32_t logged = 0;
  if (logged < 3 && drawn) {
    ++logged;
    REXGPU_INFO("odisea DLSS: movimiento propio de {} de {} dibujos de personajes", drawn,
                dlss_so_entries_.size());
  }
}

void D3D12CommandProcessor::DlssObjectMotionEndFrame() {
  dlss_previous_jitter_ndc_[0] = dlss_jitter_ndc_[0];
  dlss_previous_jitter_ndc_[1] = dlss_jitter_ndc_[1];
  if (!dlss_so_frame_started_) {
    // Fotograma sin personajes: nada que emparejar en el siguiente.
    dlss_so_previous_.clear();
    return;
  }
  uint32_t current = dlss_so_current_;
  // El bufer de este fotograma sera el "anterior": en reposo para leerlo.
  PushTransitionBarrier(dlss_so_buffers_[current].Get(), dlss_so_buffer_states_[current],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dlss_so_buffer_states_[current] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  dlss_so_previous_.clear();
  for (const DlssSoEntry& entry : dlss_so_entries_) {
    dlss_so_previous_[entry.key] = entry;
  }
  dlss_so_current_ = current ^ 1;
  dlss_so_frame_started_ = false;
}

void D3D12CommandProcessor::DlssOnResolve() {
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
  dlss_textures_submission_ = submission_current_;
  if (!EnsureDlssTexture(dlss_depth_texture_, kDlssDepthFormat, width, height,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
      !EnsureDlssTexture(dlss_motion_texture_, kDlssMotionFormat, width, height,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
    return;
  }

  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[3];
  if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                          bindless_resources_used_ ? descriptors + 1
                                                                   : descriptors)) {
    return;
  }
  // A partir de aqui nada que cambie el heap de descriptores.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (bindless_resources_used_) {
    descriptors[0] = GetSystemBindlessViewHandlePair(SystemBindlessView::kEdramR32UintSRV);
  } else {
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(descriptors[0].first, 0);
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav_desc.Format = kDlssDepthFormat;
  device->CreateUnorderedAccessView(dlss_depth_texture_.Get(), nullptr, &uav_desc,
                                    descriptors[1].first);
  uav_desc.Format = kDlssMotionFormat;
  device->CreateUnorderedAccessView(dlss_motion_texture_.Get(), nullptr, &uav_desc,
                                    descriptors[2].first);

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

  render_target_cache_->OdiseaPrepareEdramRead();
  deferred_command_list_.D3DSetComputeRootSignature(dlss_inputs_root_signature_.Get());
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(DlssInputsRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t), &constants,
      0);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(DlssInputsRootParameter::kEdram),
                                                          descriptors[0].second);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(DlssInputsRootParameter::kDepth),
                                                          descriptors[1].second);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(DlssInputsRootParameter::kMotion),
                                                          descriptors[2].second);
  SetExternalPipeline(dlss_inputs_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  dlss_inputs_ready_ = true;
  dlss_inputs_size_[0] = width;
  dlss_inputs_size_[1] = height;
  dlss_jitter_ndc_[0] = 2.0f * jitter_x / float(width);
  dlss_jitter_ndc_[1] = -2.0f * jitter_y / float(height);
  // Los personajes ya dibujados: su movimiento real sobre el de la camara.
  DlssDrawObjectMotion(width, height);
}

void D3D12CommandProcessor::DlssCommandListCallback(void* context,
                                                    ID3D12GraphicsCommandList* command_list) {
#ifdef ODISEA_DLSS
  auto& request = *static_cast<DlssCallbackRequest*>(context);
  D3D12CommandProcessor& processor = *request.processor;
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
        NGX_D3D12_CREATE_DLSS_EXT(command_list, 1, 1, &handle, parameters, &create);
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
    REXGPU_INFO("odisea DLSS: efecto creado, {}x{} -> {}x{} (modo {}, modelo {}, profundidad {})",
                request.size[0], request.size[1], request.size[2], request.size[3], int(quality),
                request.preset, request.depth_inverted ? "invertida" : "normal");
    return;
  }
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = request.color;
  eval.Feature.pInOutput = request.output;
  eval.pInDepth = request.depth;
  eval.pInMotionVectors = request.motion;
  eval.InJitterOffsetX = request.jitter[0];
  eval.InJitterOffsetY = request.jitter[1];
  eval.InRenderSubrectDimensions.Width = request.size[0];
  eval.InRenderSubrectDimensions.Height = request.size[1];
  eval.InReset = request.reset ? 1 : 0;
  eval.InMVScaleX = 1.0f;
  eval.InMVScaleY = 1.0f;
  eval.pInBiasCurrentColorMask = request.mask;
  NVSDK_NGX_Result result =
      NGX_D3D12_EVALUATE_DLSS_EXT(command_list, static_cast<NVSDK_NGX_Handle*>(processor.dlss_feature_),
                                  parameters, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    static int errors = 0;
    if (errors++ < 5) {
      REXGPU_ERROR("odisea DLSS: fallo al evaluar (0x{:08X})", uint32_t(result));
    }
  }
#endif
}

ID3D12Resource* D3D12CommandProcessor::DlssEvaluate(ID3D12Resource* scene, uint32_t scene_width,
                                                    uint32_t scene_height, uint32_t width,
                                                    uint32_t height) {
  ID3D12Resource* output = nullptr;
  float jitter_x, jitter_y;
  odisea::dlss::GetJitter(jitter_x, jitter_y);
  int32_t preset = REXCVAR_GET(odisea_dlss_preset);
  bool feature_matches = dlss_feature_ && dlss_feature_preset_ == preset &&
                         dlss_feature_depth_inverted_ == dlss_depth_inverted_ &&
                         dlss_feature_size_[0] == scene_width &&
                         dlss_feature_size_[1] == scene_height &&
                         dlss_feature_size_[2] == width && dlss_feature_size_[3] == height;
  if (dlss_available_ && !feature_matches) {
#ifdef ODISEA_DLSS
    if (dlss_feature_) {
      // Cambio de tamano (raro): esperar a la GPU antes de soltar el efecto.
      AwaitAllQueueOperationsCompletion();
      NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(dlss_feature_));
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
    SubmitBarriers();
    deferred_command_list_.OdiseaCall(&DlssCommandListCallback, &dlss_request_);
    current_external_pipeline_ = nullptr;
    current_guest_pipeline_ = nullptr;
    dlss_reset_ = true;
    dlss_previous_color_valid_ = false;
  } else if (dlss_available_ && dlss_frame_active_ && dlss_inputs_ready_ &&
             dlss_inputs_size_[0] == scene_width && dlss_inputs_size_[1] == scene_height &&
             EnsureDlssTexture(dlss_output_texture_, ui::d3d12::D3D12Presenter::kGuestOutputFormat,
                               width, height,
                               ui::d3d12::D3D12Presenter::kGuestOutputInternalState)) {
    dlss_textures_submission_ = submission_current_;
    const D3D12_RESOURCE_STATES internal = ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
    PushTransitionBarrier(scene, internal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PushTransitionBarrier(dlss_depth_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PushTransitionBarrier(dlss_motion_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PushTransitionBarrier(dlss_output_texture_.Get(), internal,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Mascara para DLSS: reactiva (lo que no cuadra con el fotograma anterior
    // segun los vectores de camara: personaje, sombras...) y de efectos.
    ID3D12Resource* mask = nullptr;
    bool effects = dlss_mask_has_content_ && dlss_mask_texture_ &&
                   dlss_mask_texture_->GetDesc().Width == scene_width &&
                   dlss_mask_texture_->GetDesc().Height == scene_height;
    bool previous = REXCVAR_GET(odisea_dlss_reactive) && dlss_previous_color_valid_ &&
                    dlss_previous_color_texture_ &&
                    dlss_previous_color_texture_->GetDesc().Width == scene_width &&
                    dlss_previous_color_texture_->GetDesc().Height == scene_height;
    ui::d3d12::util::DescriptorCpuGpuHandlePair reactive_descriptors[5];
    if (dlss_reactive_pipeline_ && (effects || previous) &&
        EnsureDlssTexture(dlss_mask_r8_texture_, kDlssMaskR8Format, scene_width, scene_height,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
        RequestOneUseSingleViewDescriptors(5, reactive_descriptors)) {
      ID3D12Device* device = GetD3D12Provider().GetDevice();
      auto srv = [&](ID3D12Resource* resource, DXGI_FORMAT format,
                     D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &srv_desc, handle);
      };
      const DXGI_FORMAT color_format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
      srv(scene, color_format, reactive_descriptors[0].first);
      srv(previous ? dlss_previous_color_texture_.Get() : scene, color_format,
          reactive_descriptors[1].first);
      srv(dlss_motion_texture_.Get(), kDlssMotionFormat, reactive_descriptors[2].first);
      if (effects) {
        srv(dlss_mask_texture_.Get(), kDlssMaskFormat, reactive_descriptors[3].first);
        PushTransitionBarrier(dlss_mask_texture_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      } else {
        srv(scene, color_format, reactive_descriptors[3].first);
      }
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = kDlssMaskR8Format;
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device->CreateUnorderedAccessView(dlss_mask_r8_texture_.Get(), nullptr, &uav_desc,
                                        reactive_descriptors[4].first);
      float params[4] = {float(REXCVAR_GET(odisea_dlss_reactive_margin)),
                         float(REXCVAR_GET(odisea_dlss_reactive_gain)), previous ? 1.0f : 0.0f,
                         effects ? 1.0f : 0.0f};
      deferred_command_list_.D3DSetComputeRootSignature(dlss_reactive_root_signature_.Get());
      deferred_command_list_.D3DSetComputeRoot32BitConstants(0, 4, params, 0);
      for (uint32_t i = 0; i < 5; ++i) {
        deferred_command_list_.D3DSetComputeRootDescriptorTable(1 + i,
                                                                reactive_descriptors[i].second);
      }
      SetExternalPipeline(dlss_reactive_pipeline_.Get());
      SubmitBarriers();
      deferred_command_list_.D3DDispatch((scene_width + 7) / 8, (scene_height + 7) / 8, 1);
      if (effects) {
        PushTransitionBarrier(dlss_mask_texture_.Get(),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      PushTransitionBarrier(dlss_mask_r8_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      mask = dlss_mask_r8_texture_.Get();
    }
    dlss_mask_has_content_ = false;
    SubmitBarriers();
    int32_t sign = REXCVAR_GET(odisea_dlss_jitter_sign);
    dlss_request_ = {};
    dlss_request_.processor = this;
    dlss_request_.create = false;
    dlss_request_.size[0] = scene_width;
    dlss_request_.size[1] = scene_height;
    dlss_request_.size[2] = width;
    dlss_request_.size[3] = height;
    dlss_request_.color = scene;
    dlss_request_.depth = dlss_depth_texture_.Get();
    dlss_request_.motion = dlss_motion_texture_.Get();
    dlss_request_.output = dlss_output_texture_.Get();
    dlss_request_.jitter[0] = (sign & 1) ? -jitter_x : jitter_x;
    dlss_request_.jitter[1] = (sign & 2) ? -jitter_y : jitter_y;
    dlss_request_.reset = dlss_reset_;
    dlss_request_.mask = mask;
    deferred_command_list_.OdiseaCall(&DlssCommandListCallback, &dlss_request_);
    // NGX cambia el pipeline, las firmas raiz y los heaps (la lista diferida
    // reenlaza los heaps): olvidar lo que hay enlazado.
    current_external_pipeline_ = nullptr;
    current_guest_pipeline_ = nullptr;
    dlss_reset_ = false;
    // La escena de este fotograma sera la "anterior" de la mascara reactiva.
    if (EnsureDlssTexture(dlss_previous_color_texture_,
                          ui::d3d12::D3D12Presenter::kGuestOutputFormat, scene_width,
                          scene_height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
      PushTransitionBarrier(scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
      PushTransitionBarrier(dlss_previous_color_texture_.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
      SubmitBarriers();
      deferred_command_list_.D3DCopyResource(dlss_previous_color_texture_.Get(), scene);
      PushTransitionBarrier(dlss_previous_color_texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      PushTransitionBarrier(scene, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      dlss_previous_color_valid_ = true;
    }
    PushTransitionBarrier(scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, internal);
    PushTransitionBarrier(dlss_depth_texture_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PushTransitionBarrier(dlss_motion_texture_.Get(),
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PushTransitionBarrier(dlss_output_texture_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          internal);
    if (mask) {
      PushTransitionBarrier(mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    output = dlss_output_texture_.Get();
  }

  // Fotograma siguiente: con el efecto creado (o creandose en este envio), jitter.
  DlssObjectMotionEndFrame();
  odisea::dlss::EndFrame();
  dlss_inputs_ready_ = false;
  dlss_frame_active_ = dlss_available_;
  odisea::dlss::SetFrame(dlss_frame_active_, scene_width, scene_height, width, height);
  return output;
}

}  // namespace rex::graphics::d3d12
