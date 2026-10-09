// Fork (odisea): interfaz a la resolucion de salida en D3D12.
//
// Con odisea_hud_resolution > 0, los dibujos de la interfaz (odisea::IsDesignHudDraw
// sobre la pantalla principal) no van a la EDRAM: IssueDraw los dibuja con
// pipelines sin ROV (PipelineCache::kOdiseaHudModificationBit) en hud_texture_,
// una textura del host a la resolucion de salida, transparente al empezar cada
// fotograma y con el alfa acumulando la cobertura. La EDRAM, a la escala 3D del
// preset, se queda solo con la escena, que sigue su camino de siempre (resolve,
// rampa de gamma, SMAA) hasta hud_scene_texture_. En el swap, ComposeHud
// reescala la escena a la salida y pone la interfaz encima
// (shaders/odisea_hud_compose.cs.hlsl).
#include <rex/graphics/d3d12/command_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/odisea_dlss.h>
#include <rex/graphics/odisea_watched_draw.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/odisea_hud_compose_cs.h"
}  // namespace shaders

namespace {

constexpr DXGI_FORMAT kHudTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

struct HudComposeConstants {
  float output_size[4];  // ancho, alto, 1/ancho, 1/alto
  uint32_t flags[4];     // x = hay interfaz
};

D3D12_RESOURCE_DESC HudTextureDesc(DXGI_FORMAT format, uint32_t width, uint32_t height,
                                   D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc;
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC HudSrvDesc(DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MostDetailedMip = 0;
  desc.Texture2D.MipLevels = 1;
  desc.Texture2D.PlaneSlice = 0;
  desc.Texture2D.ResourceMinLODClamp = 0.0f;
  return desc;
}

}  // namespace

bool D3D12CommandProcessor::InitializeHud() {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_ROOT_PARAMETER root_parameters[UINT(HudComposeRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants =
        root_parameters[UINT(HudComposeRootParameter::kConstants)];
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 0;
    constants.Constants.RegisterSpace = 0;
    constants.Constants.Num32BitValues = sizeof(HudComposeConstants) / sizeof(uint32_t);
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  // Un descriptor por tabla: RequestOneUseSingleViewDescriptors no los da
  // contiguos.
  D3D12_DESCRIPTOR_RANGE ranges[3];
  for (uint32_t i = 0; i < 3; ++i) {
    D3D12_DESCRIPTOR_RANGE& range = ranges[i];
    range.RangeType = i < 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = i < 2 ? i : 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& table = root_parameters[UINT(HudComposeRootParameter::kScene) + i];
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable.NumDescriptorRanges = 1;
    table.DescriptorTable.pDescriptorRanges = &range;
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC sampler;
  sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MipLODBias = 0.0f;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  sampler.MinLOD = 0.0f;
  sampler.MaxLOD = 0.0f;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
  root_signature_desc.NumParameters = UINT(HudComposeRootParameter::kCount);
  root_signature_desc.pParameters = root_parameters;
  root_signature_desc.NumStaticSamplers = 1;
  root_signature_desc.pStaticSamplers = &sampler;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(hud_compose_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!hud_compose_root_signature_) {
    REXGPU_ERROR("odisea: failed to create the HUD compose root signature");
    ShutdownHud();
    return false;
  }
  *(hud_compose_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::odisea_hud_compose_cs, sizeof(shaders::odisea_hud_compose_cs),
      hud_compose_root_signature_.Get());
  if (!hud_compose_pipeline_) {
    REXGPU_ERROR("odisea: failed to create the HUD compose pipeline");
    ShutdownHud();
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtv_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&hud_rtv_heap_)))) {
    REXGPU_ERROR("odisea: failed to create the HUD RTV heap");
    ShutdownHud();
    return false;
  }
  return true;
}

void D3D12CommandProcessor::ShutdownHud() {
  hud_scene_texture_.Reset();
  hud_scene_width_ = 0;
  hud_scene_height_ = 0;
  hud_texture_.Reset();
  hud_texture_width_ = 0;
  hud_texture_height_ = 0;
  hud_texture_has_content_ = false;
  hud_rtv_heap_.Reset();
  hud_compose_pipeline_.Reset();
  hud_compose_root_signature_.Reset();
}

bool D3D12CommandProcessor::GetHudOutputSize(uint32_t& width, uint32_t& height) const {
  if (!hud_compose_pipeline_ || !hud_rtv_heap_ ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kPixelShaderInterlock) {
    return false;
  }
  height = odisea::HudOutputHeight();
  width = odisea::HudOutputWidth();
  return width && height;
}

bool D3D12CommandProcessor::BeginHudDraw(uint32_t width, uint32_t height) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (!hud_texture_ || hud_texture_width_ != width || hud_texture_height_ != height) {
    if (hud_texture_) {
      if (submission_completed_ < hud_texture_submission_) {
        hud_texture_->AddRef();
        resources_for_deletion_.emplace_back(hud_texture_submission_, hud_texture_.Get());
      }
      hud_texture_.Reset();
    }
    hud_texture_width_ = 0;
    hud_texture_height_ = 0;
    D3D12_RESOURCE_DESC desc = HudTextureDesc(kHudTextureFormat, width, height,
                                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = kHudTextureFormat;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value, IID_PPV_ARGS(&hud_texture_)))) {
      REXGPU_ERROR("odisea: failed to create the {}x{} HUD texture", width, height);
      return false;
    }
    hud_texture_width_ = width;
    hud_texture_height_ = height;
    hud_texture_has_content_ = false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    rtv_desc.Format = kHudTextureFormat;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(hud_texture_.Get(), &rtv_desc,
                                   hud_rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    REXGPU_INFO("odisea: interfaz a {}x{}, fuera de la EDRAM", width, height);
  }
  hud_texture_submission_ = submission_current_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = hud_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  SubmitBarriers();
  if (!hud_texture_has_content_) {
    static const float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    deferred_command_list_.D3DClearRenderTargetView(rtv, kTransparent, 0, nullptr);
    hud_texture_has_content_ = true;
  }
  deferred_command_list_.D3DOMSetRenderTargets(1, &rtv, TRUE, nullptr);
  return true;
}

void D3D12CommandProcessor::EndHudDraw() {
  // Con ROV no hay destinos enlazados: dejarlo igual para los dibujos de la
  // EDRAM que vengan despues.
  deferred_command_list_.D3DOMSetRenderTargets(0, nullptr, FALSE, nullptr);
}

bool D3D12CommandProcessor::EnsureHudSceneTexture(uint32_t width, uint32_t height) {
  if (hud_scene_texture_ && hud_scene_width_ == width && hud_scene_height_ == height) {
    return true;
  }
  if (hud_scene_texture_) {
    if (submission_completed_ < hud_scene_submission_) {
      hud_scene_texture_->AddRef();
      resources_for_deletion_.emplace_back(hud_scene_submission_, hud_scene_texture_.Get());
    }
    hud_scene_texture_.Reset();
  }
  hud_scene_width_ = 0;
  hud_scene_height_ = 0;
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  D3D12_RESOURCE_DESC desc =
      HudTextureDesc(ui::d3d12::D3D12Presenter::kGuestOutputFormat, width, height,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          ui::d3d12::D3D12Presenter::kGuestOutputInternalState, nullptr,
          IID_PPV_ARGS(&hud_scene_texture_)))) {
    REXGPU_ERROR("odisea: failed to create the {}x{} HUD scene texture", width, height);
    return false;
  }
  hud_scene_width_ = width;
  hud_scene_height_ = height;
  return true;
}

bool D3D12CommandProcessor::ComposeHud(ID3D12Resource* scene, ID3D12Resource* dest,
                                       uint32_t width, uint32_t height) {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  bool has_interface = hud_texture_ && hud_texture_has_content_ &&
                       hud_texture_width_ == width && hud_texture_height_ == height;
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[3];
  if (!RequestOneUseSingleViewDescriptors(3, descriptors)) {
    return false;
  }
  // A partir de aqui nada que cambie el heap de descriptores.
  hud_scene_submission_ = submission_current_;
  D3D12_SHADER_RESOURCE_VIEW_DESC scene_srv =
      HudSrvDesc(ui::d3d12::D3D12Presenter::kGuestOutputFormat);
  device->CreateShaderResourceView(scene, &scene_srv, descriptors[0].first);
  // Sin interfaz se enlaza la escena tambien en t1 (el sombreador no la lee).
  if (has_interface) {
    D3D12_SHADER_RESOURCE_VIEW_DESC hud_srv = HudSrvDesc(kHudTextureFormat);
    device->CreateShaderResourceView(hud_texture_.Get(), &hud_srv, descriptors[1].first);
    hud_texture_submission_ = submission_current_;
  } else {
    device->CreateShaderResourceView(scene, &scene_srv, descriptors[1].first);
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
  uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav_desc.Texture2D.MipSlice = 0;
  uav_desc.Texture2D.PlaneSlice = 0;
  device->CreateUnorderedAccessView(dest, nullptr, &uav_desc, descriptors[2].first);

  PushTransitionBarrier(scene, ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (has_interface) {
    PushTransitionBarrier(hud_texture_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  PushTransitionBarrier(dest, ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  HudComposeConstants constants;
  constants.output_size[0] = float(width);
  constants.output_size[1] = float(height);
  constants.output_size[2] = 1.0f / float(width);
  constants.output_size[3] = 1.0f / float(height);
  constants.flags[0] = has_interface ? 1 : 0;
  float sharpness = odisea::dlss::Sharpness();
  std::memcpy(&constants.flags[1], &sharpness, sizeof(float));
  constants.flags[2] = constants.flags[3] = 0;
  deferred_command_list_.D3DSetComputeRootSignature(hud_compose_root_signature_.Get());
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(HudComposeRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t), &constants,
      0);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(UINT(HudComposeRootParameter::kScene),
                                                          descriptors[0].second);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(HudComposeRootParameter::kInterface), descriptors[1].second);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(HudComposeRootParameter::kDestination), descriptors[2].second);
  SetExternalPipeline(hud_compose_pipeline_.Get());
  SubmitBarriers();
  deferred_command_list_.D3DDispatch((width + 15) / 16, (height + 7) / 8, 1);

  PushTransitionBarrier(scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
  if (has_interface) {
    PushTransitionBarrier(hud_texture_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  PushTransitionBarrier(dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
  // El siguiente fotograma empieza con la interfaz vacia.
  hud_texture_has_content_ = false;
  return true;
}

}  // namespace rex::graphics::d3d12
