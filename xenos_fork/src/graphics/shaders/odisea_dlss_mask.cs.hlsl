// Fork (odisea): mascara de efectos para DLSS (pInBiasCurrentColorMask).
//
// Los efectos de la escena (dibujos con mezcla que no escriben profundidad:
// particulas, humo, chispas) se dibujan otra vez en una textura RGBA8 a la
// escala 3D escribiendo solo el alfa (el maximo); aqui ese alfa pasa al canal
// unico que lee DLSS. Donde vale 1, DLSS se fia del color actual y no arrastra
// el historial (los efectos no tienen vectores de movimiento propios).
// Compilado por tools/build_hud_shaders.py.

Texture2D<float4> xe_dlss_mask_source : register(t0);
RWTexture2D<float> xe_dlss_mask_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 size;
  xe_dlss_mask_source.GetDimensions(size.x, size.y);
  uint2 pixel = xe_thread_id.xy;
  if (any(pixel >= size)) {
    return;
  }
  xe_dlss_mask_dest[pixel] = saturate(xe_dlss_mask_source.Load(int3(pixel, 0)).a * 2.0);
}
