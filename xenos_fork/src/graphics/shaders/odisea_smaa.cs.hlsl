// Fork (odisea): SMAA 1x como tres compute shaders sobre la salida del
// swap, en el mismo punto que el FXAA. SMAA es de Jorge Jimenez et al., licencia
// MIT (thirdparty/smaa, sin modificar). Una sola fuente para los dos backends:
// tools/build_smaa_shaders.py la compila con fxc (D3D12, DXBC) y con dxc
// (Vulkan, SPIR-V) y deja las cabeceras en shaders/bytecode y shaders/vulkan_spirv.
//
// XE_SMAA_PASS:
//   0  deteccion de bordes por color   input0 = color
//   1  pesos de mezcla                 input0 = bordes, input1 = area, input2 = search
//   2  mezcla de vecindad              input0 = color, input1 = pesos
//
// Recursos (mismo orden en los dos backends; las entradas que una pasada no usa
// se enlazan igualmente, repitiendo una valida):
//   D3D12   b0 constantes, t0..t2 entradas, s0 sampler lineal, u0 destino
//   Vulkan  push constants, bindings 0..2 entradas, 3 sampler, 4 destino
// xe_smaa_metrics = (1/ancho, 1/alto, ancho, alto)

#if XE_SMAA_SPIRV
struct XeSmaaConstants {
  float4 metrics;
};
[[vk::push_constant]] ConstantBuffer<XeSmaaConstants> xe_smaa_constants;
#define XE_SMAA_METRICS (xe_smaa_constants.metrics)
[[vk::binding(0, 0)]] Texture2D<float4> xe_smaa_input0;
[[vk::binding(1, 0)]] Texture2D<float4> xe_smaa_input1;
[[vk::binding(2, 0)]] Texture2D<float4> xe_smaa_input2;
[[vk::binding(3, 0)]] SamplerState xe_smaa_sampler_linear;
// El destino de la ultima pasada es la imagen de salida del presentador
// (A2B10G10R10); las intermedias son RGBA8, que cualquier dispositivo admite
// como storage image (RG8 no esta garantizado).
#if XE_SMAA_PASS == 2
[[vk::binding(4, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> xe_smaa_dest;
#else
[[vk::binding(4, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> xe_smaa_dest;
#endif
#else
cbuffer XeSmaaConstants : register(b0) {
  float4 xe_smaa_metrics;
};
#define XE_SMAA_METRICS (xe_smaa_metrics)
Texture2D<float4> xe_smaa_input0 : register(t0);
Texture2D<float4> xe_smaa_input1 : register(t1);
Texture2D<float4> xe_smaa_input2 : register(t2);
SamplerState xe_smaa_sampler_linear : register(s0);
RWTexture2D<float4> xe_smaa_dest : register(u0);
#endif

#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS XE_SMAA_METRICS
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1

#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(xe_smaa_sampler_linear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(xe_smaa_sampler_linear, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
  tex.SampleLevel(xe_smaa_sampler_linear, coord, 0, offset)
// En compute no hay derivadas: las muestras de LOD implicito del original van
// al nivel 0, que es lo que ya hacen porque todas las texturas tienen un nivel.
#define SMAASample(tex, coord) tex.SampleLevel(xe_smaa_sampler_linear, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(xe_smaa_sampler_linear, coord, 0)
#define SMAASampleOffset(tex, coord, offset) \
  tex.SampleLevel(xe_smaa_sampler_linear, coord, 0, offset)
#define SMAAGather(tex, coord) tex.Gather(xe_smaa_sampler_linear, coord, 0)
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]

// Las funciones de deteccion de bordes hacen "discard" donde no hay borde, y el
// render target del original esta limpio a cero. En compute no hay discard: el
// equivalente es devolver cero (todas esas funciones devuelven float2).
#define discard return float2(0.0, 0.0)
#include "../../../thirdparty/smaa/SMAA.hlsl"
#undef discard

[numthreads(16, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 pixel = xe_thread_id.xy;
  if (any(float2(pixel) >= XE_SMAA_METRICS.zw)) {
    return;
  }
  float2 texcoord = (float2(pixel) + 0.5) * XE_SMAA_METRICS.xy;
#if XE_SMAA_PASS == 0
  float4 offset[3];
  SMAAEdgeDetectionVS(texcoord, offset);
  float2 edges = SMAAColorEdgeDetectionPS(texcoord, offset, xe_smaa_input0);
  xe_smaa_dest[pixel] = float4(edges, 0.0, 0.0);
#elif XE_SMAA_PASS == 1
  float2 pixcoord;
  float4 offset[3];
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
  xe_smaa_dest[pixel] = SMAABlendingWeightCalculationPS(
      texcoord, pixcoord, offset, xe_smaa_input0, xe_smaa_input1, xe_smaa_input2,
      float4(0.0, 0.0, 0.0, 0.0));
#else
  float4 offset;
  SMAANeighborhoodBlendingVS(texcoord, offset);
  xe_smaa_dest[pixel] =
      SMAANeighborhoodBlendingPS(texcoord, offset, xe_smaa_input0, xe_smaa_input1);
#endif
}
