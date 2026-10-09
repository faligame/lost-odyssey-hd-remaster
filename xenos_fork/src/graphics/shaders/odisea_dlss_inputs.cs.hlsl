// Fork (odisea): entradas de DLSS sacadas de la EDRAM (camino ROV).
//
// Se lanza justo cuando el juego resuelve la profundidad de la escena (copia de
// profundidad de la superficie principal), antes de que otras pasadas pisen esa
// zona de la EDRAM. Por cada pixel del host de la escena:
//   - profundidad: los 24 bits altos de la EDRAM (unorm24 o float 20e4) -> R32F;
//   - vector de movimiento de la camara: el punto del mundo (profundidad y
//     ViewProjection inversa de este fotograma, sin jitter) proyectado con la
//     ViewProjection del anterior; en pixeles de la escena, del actual al anterior.
// La direccion en la EDRAM es la de XeEdramOffsetInts (edram.xesli) para 1x MSAA
// con escala en cuartos: tiles de (20q)x(4q) muestras de 32 bits, la mitad de
// cada tile de profundidad girada.
// Compilado por tools/build_hud_shaders.py.

#if XE_SPIRV
// Vulkan: push constants (176 bytes); enlaces 0 EDRAM (bufer de almacenamiento),
// 1 profundidad R32F, 2 movimiento RG16F.
struct XeDlssInputsConstants {
  // Los mismos campos que el cbuffer de abajo.
  row_major float4x4 inverse_view_projection;
  row_major float4x4 previous_view_projection;
  float4 size_jitter;
  uint4 edram;
  uint4 edram_extra;
};
[[vk::push_constant]] ConstantBuffer<XeDlssInputsConstants> xe_dlss_constants;
#define xe_dlss_inverse_view_projection (xe_dlss_constants.inverse_view_projection)
#define xe_dlss_previous_view_projection (xe_dlss_constants.previous_view_projection)
#define xe_dlss_size_jitter (xe_dlss_constants.size_jitter)
#define xe_dlss_edram (xe_dlss_constants.edram)
#define xe_dlss_edram_extra (xe_dlss_constants.edram_extra)
[[vk::binding(0, 0)]] StructuredBuffer<uint> xe_dlss_edram_buffer;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> xe_dlss_depth;
[[vk::binding(2, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> xe_dlss_motion;
#else
cbuffer XeDlssInputsConstants : register(b0) {
  // Matrices fila (vector fila: clip = [x y z 1] * M).
  row_major float4x4 xe_dlss_inverse_view_projection;
  row_major float4x4 xe_dlss_previous_view_projection;
  // xy = tamano de la escena en pixeles del host, zw = jitter en pixeles.
  float4 xe_dlss_size_jitter;
  // x = base en tiles, y = pitch en tiles, z = escala en cuartos, w = flags
  // (bit 0: profundidad float 20e4).
  uint4 xe_dlss_edram;
  // x = numero de tiles de la EDRAM; y, z = escala y desplazamiento z del
  // viewport del juego (profundidad = z_ndc * escala + desplazamiento; Lost
  // Odyssey usa profundidad invertida: -1 y 1), como bits de float.
  uint4 xe_dlss_edram_extra;
};

Buffer<uint> xe_dlss_edram_buffer : register(t0);
RWTexture2D<float> xe_dlss_depth : register(u0);
RWTexture2D<float2> xe_dlss_motion : register(u1);

#endif

float XeFloat20e4To32(uint f24) {
  uint mantissa = f24 & 0xFFFFFu;
  uint exponent = f24 >> 20u;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      return 0.0;
    }
    uint shift = 20u - firstbithigh(mantissa);
    mantissa = (mantissa << shift) & 0xFFFFFu;
    exponent = 1u - shift;
  }
  return asfloat(((exponent + 112u) << 23u) | (mantissa << 3u));
}

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 pixel = xe_thread_id.xy;
  float2 size = xe_dlss_size_jitter.xy;
  if (any(float2(pixel) >= size)) {
    return;
  }

  // Direccion en la EDRAM.
  uint scale_q = xe_dlss_edram.z;
  uint2 tile_size = uint2(20u, 4u) * scale_q;
  uint2 tile = pixel / tile_size;
  uint2 local = pixel - tile * tile_size;
  uint half_width = tile_size.x >> 1u;
  local.x = local.x >= half_width ? local.x - half_width : local.x + half_width;
  uint tile_index = xe_dlss_edram.x + tile.y * xe_dlss_edram.y + tile.x;
  uint address = tile_index * (tile_size.x * tile_size.y) + local.y * tile_size.x + local.x;
  address %= tile_size.x * tile_size.y * xe_dlss_edram_extra.x;
  uint value = xe_dlss_edram_buffer[address] >> 8u;
  float depth = (xe_dlss_edram.w & 1u) ? XeFloat20e4To32(value) : float(value) * (1.0 / 16777215.0);
  depth = saturate(depth);
  xe_dlss_depth[pixel] = depth;

  // Movimiento de la camara (sin jitter).
  float2 center = float2(pixel) + 0.5 - xe_dlss_size_jitter.zw;
  float2 ndc = float2(center.x / size.x * 2.0 - 1.0, 1.0 - center.y / size.y * 2.0);
  float z_scale = asfloat(xe_dlss_edram_extra.y), z_offset = asfloat(xe_dlss_edram_extra.z);
  float z_ndc = abs(z_scale) > 1e-6 ? (depth - z_offset) / z_scale : depth;
  float4 world = mul(float4(ndc, min(z_ndc, 0.99999), 1.0), xe_dlss_inverse_view_projection);
  float2 motion = float2(0.0, 0.0);
  if (abs(world.w) > 1e-8) {
    world /= world.w;
    float4 previous_clip = mul(float4(world.xyz, 1.0), xe_dlss_previous_view_projection);
    if (previous_clip.w > 1e-6) {
      float2 previous_ndc = previous_clip.xy / previous_clip.w;
      float2 previous_pixel =
          float2((previous_ndc.x + 1.0) * 0.5 * size.x, (1.0 - previous_ndc.y) * 0.5 * size.y);
      motion = previous_pixel - center;
    }
  }
  xe_dlss_motion[pixel] = motion;
}
