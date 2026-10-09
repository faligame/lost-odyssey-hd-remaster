// Fork (odisea): ver odisea_dlss_object_motion.hlsli.
#include "odisea_dlss_object_motion.hlsli"

XeObjectMotionVertex main(uint xe_vertex_id : SV_VertexID) {
  XeObjectMotionVertex output;
  float4 current = xe_om_current[xe_om_bases.x + xe_vertex_id];
  output.position = current;
  output.current = current;
  output.previous = xe_om_previous[xe_om_bases.y + xe_vertex_id];
  return output;
}
