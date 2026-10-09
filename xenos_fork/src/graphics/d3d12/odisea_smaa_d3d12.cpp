// Fork (odisea): SMAA 1x en el swap de D3D12.
//
// IssueSwap aplica la rampa de gamma a fxaa_source_texture_, igual que para el
// FXAA, y ApplySmaa hace desde ahi las tres pasadas hasta la imagen del
// presentador:
//   bordes  color            -> smaa_edges_texture_
//   pesos   bordes, area, search -> smaa_weights_texture_
//   mezcla  color, pesos     -> salida
// Los shaders son shaders/odisea_smaa.cs.hlsl (tools/build_smaa_shaders.py).
#include <rex/graphics/d3d12/command_processor.h>

#include <initializer_list>

#include <rex/graphics/odisea_smaa.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/odisea_smaa_blend_cs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_smaa_edges_cs.h"
#include "../shaders/bytecode/d3d12_5_1/odisea_smaa_weights_cs.h"
}  // namespace shaders

namespace {

// Bordes y pesos en RGBA8: para los bordes bastaria RG8, pero asi las dos
// intermedias son iguales, y tambien iguales a las de Vulkan.
constexpr DXGI_FORMAT kSmaaIntermediateFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kSmaaAreaFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaSearchFormat = DXGI_FORMAT_R8_UNORM;

struct SmaaConstants {
  float metrics[4];  // 1/ancho, 1/alto, ancho, alto
};

D3D12_SHADER_RESOURCE_VIEW_DESC SmaaSrvDesc(DXGI_FORMAT format) {
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

D3D12_RESOURCE_DESC SmaaTextureDesc(DXGI_FORMAT format, uint32_t width, uint32_t height,
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

}  // namespace

bool D3D12CommandProcessor::InitializeSmaa() {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_ROOT_PARAMETER root_parameters[UINT(SmaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants = root_parameters[UINT(SmaaRootParameter::kConstants)];
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 0;
    constants.Constants.RegisterSpace = 0;
    constants.Constants.Num32BitValues = sizeof(SmaaConstants) / sizeof(uint32_t);
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  // Una tabla de un descriptor por recurso, como el resto de pasadas del swap:
  // RequestOneUseSingleViewDescriptors da descriptores sueltos, no contiguos.
  D3D12_DESCRIPTOR_RANGE ranges[4];
  for (uint32_t i = 0; i < 4; ++i) {
    D3D12_DESCRIPTOR_RANGE& range = ranges[i];
    range.RangeType = i < 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = i < 3 ? i : 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& table = root_parameters[UINT(SmaaRootParameter::kInput0) + i];
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
  root_signature_desc.NumParameters = UINT(SmaaRootParameter::kCount);
  root_signature_desc.pParameters = root_parameters;
  root_signature_desc.NumStaticSamplers = 1;
  root_signature_desc.pStaticSamplers = &sampler;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(smaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!smaa_root_signature_) {
    REXGPU_ERROR("odisea: failed to create the SMAA root signature");
    ShutdownSmaa();
    return false;
  }

  struct PipelineSource {
    const BYTE* code;
    size_t size;
  };
  const PipelineSource pipeline_sources[3] = {
      {shaders::odisea_smaa_edges_cs, sizeof(shaders::odisea_smaa_edges_cs)},
      {shaders::odisea_smaa_weights_cs, sizeof(shaders::odisea_smaa_weights_cs)},
      {shaders::odisea_smaa_blend_cs, sizeof(shaders::odisea_smaa_blend_cs)},
  };
  for (uint32_t i = 0; i < 3; ++i) {
    *(smaa_pipelines_[i].ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
        device, pipeline_sources[i].code, pipeline_sources[i].size, smaa_root_signature_.Get());
    if (!smaa_pipelines_[i]) {
      REXGPU_ERROR("odisea: failed to create SMAA compute pipeline {}", i);
      ShutdownSmaa();
      return false;
    }
  }

  // Las texturas de consulta se crean ya en COPY_DEST; sus bytes se suben en el
  // primer uso, cuando hay una lista de comandos abierta.
  D3D12_RESOURCE_DESC area_desc = SmaaTextureDesc(kSmaaAreaFormat, odisea::kSmaaAreaTexWidth,
                                                  odisea::kSmaaAreaTexHeight,
                                                  D3D12_RESOURCE_FLAG_NONE);
  D3D12_RESOURCE_DESC search_desc = SmaaTextureDesc(
      kSmaaSearchFormat, odisea::kSmaaSearchTexWidth, odisea::kSmaaSearchTexHeight,
      D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
          &area_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&smaa_area_texture_))) ||
      FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
          &search_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&smaa_search_texture_)))) {
    REXGPU_ERROR("odisea: failed to create the SMAA lookup textures");
    ShutdownSmaa();
    return false;
  }
  smaa_lookup_uploaded_ = false;
  return true;
}

void D3D12CommandProcessor::ShutdownSmaa() {
  smaa_weights_texture_.Reset();
  smaa_edges_texture_.Reset();
  smaa_textures_width_ = 0;
  smaa_textures_height_ = 0;
  smaa_textures_submission_ = 0;
  smaa_search_texture_.Reset();
  smaa_area_texture_.Reset();
  smaa_lookup_uploaded_ = false;
  for (auto& pipeline : smaa_pipelines_) {
    pipeline.Reset();
  }
  smaa_root_signature_.Reset();
}

bool D3D12CommandProcessor::EnsureSmaaTextures(uint32_t width, uint32_t height) {
  if (smaa_edges_texture_ && smaa_weights_texture_ && smaa_textures_width_ == width &&
      smaa_textures_height_ == height) {
    return true;
  }
  // Cambio de tamano: las viejas pueden seguir referenciadas por un envio que
  // aun no ha terminado, asi que se liberan igual que fxaa_source_texture_.
  // std::addressof: ComPtr sobrecarga el operador & (devuelve un ComPtrRef).
  for (Microsoft::WRL::ComPtr<ID3D12Resource>* texture :
       {std::addressof(smaa_edges_texture_), std::addressof(smaa_weights_texture_)}) {
    if (*texture) {
      if (submission_completed_ < smaa_textures_submission_) {
        (*texture)->AddRef();
        resources_for_deletion_.emplace_back(smaa_textures_submission_, texture->Get());
      }
      texture->Reset();
    }
  }
  smaa_textures_width_ = 0;
  smaa_textures_height_ = 0;
  smaa_textures_submission_ = 0;

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC desc = SmaaTextureDesc(kSmaaIntermediateFormat, width, height,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // Entre usos se quedan en NON_PIXEL_SHADER_RESOURCE, como la fuente del FXAA.
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&smaa_edges_texture_))) ||
      FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&smaa_weights_texture_)))) {
    REXGPU_ERROR("odisea: failed to create the {}x{} SMAA textures", width, height);
    smaa_edges_texture_.Reset();
    smaa_weights_texture_.Reset();
    return false;
  }
  smaa_textures_width_ = width;
  smaa_textures_height_ = height;
  return true;
}

bool D3D12CommandProcessor::UploadSmaaLookupTextures() {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  struct Lookup {
    ID3D12Resource* texture;
    const uint8_t* bytes;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
  };
  Lookup lookups[2] = {
      {smaa_area_texture_.Get(), odisea::SmaaAreaTexBytes(), odisea::kSmaaAreaTexWidth,
       odisea::kSmaaAreaTexHeight, 2, {}, {}},
      {smaa_search_texture_.Get(), odisea::SmaaSearchTexBytes(),
       odisea::kSmaaSearchTexWidth, odisea::kSmaaSearchTexHeight, 1, {}, {}},
  };

  // Primero se preparan los dos buffers de subida; solo si los dos estan listos
  // se graban las copias, para no dejar una textura a medias en otro estado.
  for (Lookup& lookup : lookups) {
    D3D12_RESOURCE_DESC texture_desc = lookup.texture->GetDesc();
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &lookup.layout, nullptr, nullptr,
                                  &upload_size);
    D3D12_RESOURCE_DESC upload_desc;
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Alignment = 0;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.SampleDesc.Quality = 0;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    upload_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesUpload, provider.GetHeapFlagCreateNotZeroed(),
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&lookup.upload)))) {
      REXGPU_ERROR("odisea: failed to create an SMAA lookup upload buffer");
      return false;
    }
    void* mapping = nullptr;
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(lookup.upload->Map(0, &no_read, &mapping)) || !mapping) {
      REXGPU_ERROR("odisea: failed to map an SMAA lookup upload buffer");
      return false;
    }
    const size_t row_bytes = size_t(lookup.width) * lookup.bytes_per_pixel;
    uint8_t* dst = static_cast<uint8_t*>(mapping) + lookup.layout.Offset;
    for (uint32_t y = 0; y < lookup.height; ++y) {
      std::memcpy(dst + size_t(y) * lookup.layout.Footprint.RowPitch,
                  lookup.bytes + size_t(y) * row_bytes, row_bytes);
    }
    lookup.upload->Unmap(0, nullptr);
  }

  for (Lookup& lookup : lookups) {
    D3D12_TEXTURE_COPY_LOCATION copy_src, copy_dst;
    copy_src.pResource = lookup.upload.Get();
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_src.PlacedFootprint = lookup.layout;
    copy_dst.pResource = lookup.texture;
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dst.SubresourceIndex = 0;
    deferred_command_list_.D3DCopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);
    PushTransitionBarrier(lookup.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // El buffer de subida vive hasta que termina el envio que copia de el.
    resources_for_deletion_.emplace_back(submission_current_, lookup.upload.Detach());
  }
  smaa_lookup_uploaded_ = true;
  return true;
}

bool D3D12CommandProcessor::ApplySmaa(ID3D12Resource* color_source, ID3D12Resource* dest,
                                      uint32_t width, uint32_t height) {
  if (!smaa_root_signature_ || !EnsureSmaaTextures(width, height)) {
    return false;
  }
  if (!smaa_lookup_uploaded_ && !UploadSmaaLookupTextures()) {
    return false;
  }
  smaa_textures_submission_ = submission_current_;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const SmaaConstants constants = {
      {1.0f / float(width), 1.0f / float(height), float(width), float(height)}};
  uint32_t group_count_x = (width + 15) / 16;
  uint32_t group_count_y = (height + 7) / 8;

  struct Pass {
    ID3D12PipelineState* pipeline;
    ID3D12Resource* inputs[3];
    DXGI_FORMAT input_formats[3];
    ID3D12Resource* output;
    DXGI_FORMAT output_format;
    // Las intermedias son nuestras y pasan por UAV; la salida ya llega en UAV.
    bool output_intermediate;
  };
  ID3D12Resource* edges = smaa_edges_texture_.Get();
  ID3D12Resource* weights = smaa_weights_texture_.Get();
  const Pass passes[3] = {
      {smaa_pipelines_[0].Get(),
       {color_source, color_source, color_source},
       {kFxaaSourceTextureFormat, kFxaaSourceTextureFormat, kFxaaSourceTextureFormat},
       edges,
       kSmaaIntermediateFormat,
       true},
      {smaa_pipelines_[1].Get(),
       {edges, smaa_area_texture_.Get(), smaa_search_texture_.Get()},
       {kSmaaIntermediateFormat, kSmaaAreaFormat, kSmaaSearchFormat},
       weights,
       kSmaaIntermediateFormat,
       true},
      {smaa_pipelines_[2].Get(),
       {color_source, weights, weights},
       {kFxaaSourceTextureFormat, kSmaaIntermediateFormat, kSmaaIntermediateFormat},
       dest,
       ui::d3d12::D3D12Presenter::kGuestOutputFormat,
       false},
  };

  for (const Pass& pass : passes) {
    ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[4];
    if (!RequestOneUseSingleViewDescriptors(4, descriptors)) {
      return false;
    }
    // A partir de aqui no se puede llamar a nada que cambie el heap de
    // descriptores hasta haberlos enlazado.
    for (uint32_t i = 0; i < 3; ++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = SmaaSrvDesc(pass.input_formats[i]);
      device->CreateShaderResourceView(pass.inputs[i], &srv_desc, descriptors[i].first);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    uav_desc.Format = pass.output_format;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;
    uav_desc.Texture2D.PlaneSlice = 0;
    device->CreateUnorderedAccessView(pass.output, nullptr, &uav_desc, descriptors[3].first);

    if (pass.output_intermediate) {
      PushTransitionBarrier(pass.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    deferred_command_list_.D3DSetComputeRootSignature(smaa_root_signature_.Get());
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(SmaaRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t), &constants, 0);
    for (uint32_t i = 0; i < 3; ++i) {
      deferred_command_list_.D3DSetComputeRootDescriptorTable(
          UINT(SmaaRootParameter::kInput0) + i, descriptors[i].second);
    }
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(SmaaRootParameter::kDestination), descriptors[3].second);
    SetExternalPipeline(pass.pipeline);
    SubmitBarriers();
    deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
    if (pass.output_intermediate) {
      PushTransitionBarrier(pass.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  static bool logged = false;
  if (!logged) {
    logged = true;
    REXGPU_INFO("odisea: SMAA 1x active on the D3D12 swap output ({}x{})", width, height);
  }
  return true;
}

}  // namespace rex::graphics::d3d12
