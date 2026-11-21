// Colour types of the fragment programs: half with HALF (setting shader_precision=half), else float. Texture
// coordinates, projections and the alpha test constants stay float.
#ifdef HALF
#define colour half
#define colour3 half3
#define colour4 half4
#else
#define colour float
#define colour3 float3
#define colour4 float4
#endif
