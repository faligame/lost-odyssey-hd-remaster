// Fork (odisea): mascara reactiva para DLSS (pInBiasCurrentColorMask).
//
// Los vectores de movimiento solo son de camara: lo que se mueve por su cuenta
// (el personaje que la camara sigue, su sombra, efectos, texturas animadas)
// lleva el vector del fondo y DLSS arrastraria su color del sitio equivocado.
// Aqui se compara cada pixel con el fotograma anterior en el punto que dice su
// vector: si el color actual cae fuera del rango de los 3x3 vecinos de alli (con
// margen, para no confundir el jitter con movimiento), el pixel se marca y DLSS
// usa la imagen actual en el. Se combina (maximo) con la mascara de efectos.
// Compilado por tools/build_hud_shaders.py.

cbuffer XeDlssReactiveConstants : register(b0) {
  // x = margen (en color 0..1), y = ganancia, z = 1 si hay fotograma anterior,
  // w = 1 si hay mascara de efectos.
  float4 xe_dlss_reactive_params;
};

Texture2D<float4> xe_dlss_current : register(t0);
Texture2D<float4> xe_dlss_previous : register(t1);
Texture2D<float2> xe_dlss_motion : register(t2);
Texture2D<float4> xe_dlss_effects : register(t3);
RWTexture2D<float> xe_dlss_reactive : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 size;
  xe_dlss_current.GetDimensions(size.x, size.y);
  uint2 pixel = xe_thread_id.xy;
  if (any(pixel >= size)) {
    return;
  }
  float mask = 0.0;
  if (xe_dlss_reactive_params.w > 0.5) {
    mask = saturate(xe_dlss_effects.Load(int3(pixel, 0)).a * 2.0);
  }
  if (xe_dlss_reactive_params.z > 0.5) {
    float3 current = xe_dlss_current.Load(int3(pixel, 0)).rgb;
    int2 previous_pixel = int2(floor(float2(pixel) + 0.5 + xe_dlss_motion.Load(int3(pixel, 0))));
    if (all(previous_pixel >= 0) && all(previous_pixel < int2(size))) {
      float3 low = float3(1.0, 1.0, 1.0), high = float3(0.0, 0.0, 0.0);
      [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
          int2 p = clamp(previous_pixel + int2(x, y), int2(0, 0), int2(size) - 1);
          float3 c = xe_dlss_previous.Load(int3(p, 0)).rgb;
          low = min(low, c);
          high = max(high, c);
        }
      }
      float margin = xe_dlss_reactive_params.x;
      float3 outside = max(low - margin - current, current - high - margin);
      float distance = max(max(outside.r, outside.g), max(outside.b, 0.0));
      mask = max(mask, saturate(distance * xe_dlss_reactive_params.y));
    }
  }
  xe_dlss_reactive[pixel] = mask;
}
