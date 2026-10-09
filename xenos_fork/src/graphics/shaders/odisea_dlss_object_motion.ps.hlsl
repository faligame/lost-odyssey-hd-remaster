// Fork (odisea): ver odisea_dlss_object_motion.hlsli.
#include "odisea_dlss_object_motion.hlsli"

float2 main(XeObjectMotionVertex input) : SV_Target {
  // Tapado por algo de la escena: se queda el movimiento que ya hay.
  float scene_depth = xe_om_depth.Load(int3(int2(input.position.xy), 0));
  bool occluded = xe_om_previous_jitter.z > 0.5 ? input.position.z < scene_depth - 0.002
                                                 : input.position.z > scene_depth + 0.002;
  if (occluded) {
    discard;
  }
  if (input.previous.w <= 1e-6 || input.current.w <= 1e-6) {
    discard;
  }
  float2 current = input.current.xy / input.current.w - xe_om_size_jitter.zw;
  float2 previous = input.previous.xy / input.previous.w - xe_om_previous_jitter.xy;
  float2 delta = previous - current;
  return float2(delta.x * 0.5 * xe_om_size_jitter.x, -delta.y * 0.5 * xe_om_size_jitter.y);
}
