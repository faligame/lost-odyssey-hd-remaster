// Fork (odisea): composicion de la salida con la interfaz a su propia resolucion.
//
// La escena (despues de la rampa de gamma y del SMAA/FXAA, a la escala 3D) se
// reescala a la resolucion de salida con un filtro bilineal, y encima va la
// interfaz, dibujada aparte a la resolucion de salida sobre transparente con el
// alfa acumulando la cobertura (premultiplicada):
//   salida = interfaz.rgb + escena.rgb * (1 - interfaz.a)
// Compilado por tools/build_hud_shaders.py.

#if XE_SPIRV
// Vulkan: push constants; enlaces 0 escena, 1 interfaz, 2 muestreador, 3 salida
// del presentador (A2B10G10R10).
struct XeHudComposeConstants {
  float4 output_size;
  uint4 flags;
};
[[vk::push_constant]] ConstantBuffer<XeHudComposeConstants> xe_hud_constants;
#define xe_hud_output_size (xe_hud_constants.output_size)
#define xe_hud_flags (xe_hud_constants.flags)
[[vk::binding(0, 0)]] Texture2D<float4> xe_hud_scene;
[[vk::binding(1, 0)]] Texture2D<float4> xe_hud_interface;
[[vk::binding(2, 0)]] SamplerState xe_hud_sampler_linear;
[[vk::binding(3, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> xe_hud_dest;
#else
cbuffer XeHudComposeConstants : register(b0) {
  // xy = tamano de la salida, zw = 1 / tamano de la salida.
  float4 xe_hud_output_size;
  // x = 1 si hay interfaz este fotograma.
  uint4 xe_hud_flags;
};

SamplerState xe_hud_sampler_linear : register(s0);
Texture2D<float4> xe_hud_scene : register(t0);
Texture2D<float4> xe_hud_interface : register(t1);
RWTexture2D<unorm float4> xe_hud_dest : register(u0);
#endif

[numthreads(16, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 pixel = xe_thread_id.xy;
  if (any(float2(pixel) >= xe_hud_output_size.xy)) {
    return;
  }
  float2 uv = (float2(pixel) + 0.5) * xe_hud_output_size.zw;
  float3 color = xe_hud_scene.SampleLevel(xe_hud_sampler_linear, uv, 0.0).rgb;
  // Enfocado de la escena (flags.y = nitidez 0..1 como bits de float), al estilo
  // de AMD CAS: cruz de 5 muestras a un pixel de la salida, peso negativo segun
  // el contraste local para no crear halos en los bordes fuertes.
  float sharpness = asfloat(xe_hud_flags.y);
  if (sharpness > 0.0) {
    float2 step = xe_hud_output_size.zw;
    float3 n = xe_hud_scene.SampleLevel(xe_hud_sampler_linear, uv - float2(0.0, step.y), 0.0).rgb;
    float3 s = xe_hud_scene.SampleLevel(xe_hud_sampler_linear, uv + float2(0.0, step.y), 0.0).rgb;
    float3 w = xe_hud_scene.SampleLevel(xe_hud_sampler_linear, uv - float2(step.x, 0.0), 0.0).rgb;
    float3 e = xe_hud_scene.SampleLevel(xe_hud_sampler_linear, uv + float2(step.x, 0.0), 0.0).rgb;
    float3 lo = min(color, min(min(n, s), min(w, e)));
    float3 hi = max(color, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(lo, 2.0 - hi) / max(hi, 1.0 / 1024.0)));
    float3 weight = -amp * lerp(1.0 / 8.0, 1.0 / 5.0, sharpness);
    color = (color + (n + s + w + e) * weight) / (1.0 + 4.0 * weight);
  }
  if (xe_hud_flags.x != 0u) {
    float4 interface_color = xe_hud_interface.Load(int3(pixel, 0));
    color = interface_color.rgb + color * (1.0 - interface_color.a);
  }
  xe_hud_dest[pixel] = float4(saturate(color), 1.0);
}
