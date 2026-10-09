// Fork (odisea): vectores de movimiento de los objetos animados para DLSS.
//
// Los dibujos de personajes (con huesos) se dibujan otra vez con stream output:
// sus vertices ya transformados (posicion de recorte, con el jitter del
// fotograma) quedan en un bufer, en el orden de los triangulos. Si el mismo
// dibujo existio en el fotograma anterior, se dibuja aqui leyendo de los dos
// buferes por SV_VertexID: cada pixel del personaje recibe su movimiento real
// (sin el jitter), y sustituye al de la camara en la textura de movimiento. La
// profundidad de la escena descarta lo que este tapado.
// Compilado por tools/build_hud_shaders.py.

cbuffer XeObjectMotionConstants : register(b0) {
  // xy = tamano de la escena en pixeles, zw = jitter de este fotograma en NDC.
  float4 xe_om_size_jitter;
  // xy = jitter del fotograma anterior en NDC; z = 1 si la profundidad esta
  // invertida (lo cercano vale mas).
  float4 xe_om_previous_jitter;
  // x = primer vertice en el bufer actual, y = en el anterior.
  uint4 xe_om_bases;
};

Buffer<float4> xe_om_current : register(t0);
Buffer<float4> xe_om_previous : register(t1);
Texture2D<float> xe_om_depth : register(t2);

struct XeObjectMotionVertex {
  float4 position : SV_Position;
  float4 current : TEXCOORD0;
  float4 previous : TEXCOORD1;
};
