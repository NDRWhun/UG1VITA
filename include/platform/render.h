#ifndef PLATFORM_RENDER_H_
#define PLATFORM_RENDER_H_

// The frame data model between the Direct3D 9 layer (game thread, records) and a device that draws it (the GXM
// render thread on the Vita). Platform neutral.
//
// A Frame is a list of scenes (one render target and depth surface each) and of draw calls. A draw call has a
// DrawState (compared with memcmp: equal states need no state changes), the vertex and index data it reads, its
// shader constants (offsets into the frame's constant arena) and a viewport. Clears are draw calls of the clear
// programs. Programs are the port's own: one pair per effect pass of the game and the fixed-function variants.
//
// Coordinates follow Direct3D: clip space z in [0, 1], window y down, pixel centres at integer coordinates (the
// devices add the half pixel). Matrices are Direct3D row-vector matrices, stored as 4 rows of float4.

#include <platform/texture.h>
#include <cstddef>
#include <cstdint>

namespace pf { namespace render {

using texture::Format;

// ---- Resources (created by the device; the layer keeps pointers) ---------------------------------------------------

// lastUse: index of the last frame that read the resource; a frame with a higher index than the device's completed
// frame may still read it, so the layer writes a busy resource only through a new copy.
struct Resource
{
    uint32_t lastUse = 0;
};

struct TextureDesc
{
    Format      format;
    uint8_t     levels;
    bool        cube;
    bool        target;     // render target: one level, rendered to by scenes
    uint16_t    width, height;
    bool        linear = false; // streamed: one uncompressed level in linear rows, rewritten by the CPU (movie frames)
};

struct TextureStorage : Resource
{
    TextureDesc desc;
};

struct DepthStorage : Resource
{
    uint16_t    width, height;
};

struct BufferStorage : Resource
{
    uint32_t    size;
    void*       data;       // vertex or index data the GPU reads; written by the layer
};

// ---- Programs ------------------------------------------------------------------------------------------------------

enum VertexProgram : uint8_t
{
    VpFixed,                    // fixed function, untransformed positions
    VpFixedScreen,              // fixed function, POSITIONT
    VpWorld,
    VpWorldHeadlight,
    VpWorldReflect,
    VpWorldHeadlightReflect,
    VpWorldBone,
    VpCar,
    VpFilter,
    VpClear,
    VpCount
};

enum FragmentProgram : uint8_t
{
    FpFixedColor,               // fixed function, no texture
    FpFixedTexture,
    FpFixedProjected,           // texture coordinates divided by z (COUNT3 | PROJECTED)
    FpWorld,
    FpWorldHeadlight,
    FpWorldReflect,
    FpWorldHeadlightReflect,
    FpWorldBone,
    FpCar,
    FpFilter1,
    FpFilter2,
    FpFilter3,
    FpFilter4,
    FpClear,
    FpCount
};

// Constant blocks: arrays of floats; the field tables below give the uniform names.
struct FixedVertexConstants
{
    float wvp[16];              // VpFixed: world * view * projection
    float screen[4];            // VpFixedScreen: x scale, y scale, x offset, y offset (window -> clip)
    float texMatrix[16];        // stage 0 texture transform (identity without one)
    float texPad[4];            // added to the texture coordinate input: missing components become (.., 1, 0)
    float diffuseMask[4];       // 1: the vertex colour is a factor of the stage result, per component
    float factorMask[4];        // 1: the texture factor is a factor
    float factor[4];            // TEXTUREFACTOR as rgba
};

struct WorldVertexConstants
{
    float wvp[16];
    float textureOffset[4];
};

struct WorldHeadlightVertexConstants
{
    float wvp[16];
    float headlightWvp[16];
    float textureOffset[4];
};

struct WorldReflectVertexConstants
{
    float wvp[16];
};

struct WorldHeadlightReflectVertexConstants
{
    float wvp[16];
    float headlightWvp[16];
};

struct WorldBoneVertexConstants
{
    float wvp[16];
    float localEyePos[4];
    float harmonic[40];
    float diffuseMin[4];
    float diffuseRange[4];
    float bones[256];           // 16 matrices
};

struct CarVertexConstants
{
    float wvp[16];
    float worldView[16];
    float localEyePos[4];
    float harmonic[40];
    float diffuseMin[4];
    float diffuseRange[4];
    float specularMin[4];
    float specularRange[4];
    float powers[4];            // specular power, envmap power
};

// Every fragment block starts with the alpha test: discard when alphaTest.x * alpha < alphaTest.y (programs
// built with ATEST).
struct FixedFragmentConstants
{
    float alphaTest[4];
    float textureMask[4];       // 1: the texel is a factor, per component
    float scale[4];             // 1, 2 or 4 (MODULATE, 2X, 4X)
    float addMask[4];           // 1: the texel is added (ADD)
};

struct WorldFragmentConstants
{
    float alphaTest[4];
};

struct WorldHeadlightFragmentConstants
{
    float alphaTest[4];
    float headlightColor[4];
};

struct CarFragmentConstants
{
    float alphaTest[4];
    float envmapMin[4];
    float envmapRange[4];
};

struct FilterFragmentConstants
{
    float alphaTest[4];
    float weights[16];
};

struct ClearFragmentConstants
{
    float alphaTest[4];
    float color[4];
};

struct UniformField
{
    const char* name;
    uint16_t    offset;         // floats
    uint16_t    count;
};

// Vertex inputs, by Direct3D usage.
enum Usage : uint8_t
{
    UsagePosition,
    UsageNormal,
    UsageColor,
    UsageTex0,
    UsageTex1,
    UsageTex2,
    UsageTex3,
    UsageWeight,
    UsageIndices,
    UsageCount
};

struct VertexProgramInfo
{
    const char*         name;           // shader binary shader_<name>
    uint16_t            constantFloats;
    uint8_t             fieldCount;
    const UniformField* fields;
};

struct FragmentProgramInfo
{
    const char*         name;           // shader_<name>; the alpha-tested variant is shader_<name>_atest
    uint16_t            constantFloats;
    uint8_t             fieldCount;
    const UniformField* fields;
    uint8_t             samplers;       // uTex0..uTex<n-1>
};

const VertexProgramInfo& vertexProgram(VertexProgram program);
const FragmentProgramInfo& fragmentProgram(FragmentProgram program);
// Name of a vertex input in the programs: aPosition, aNormal, aColor, aTex0..3, aWeight, aIndex.
const char* inputName(Usage usage);

// ---- Vertex layouts ------------------------------------------------------------------------------------------------

enum ElementType : uint8_t
{
    TypeFloat1,
    TypeFloat2,
    TypeFloat3,
    TypeFloat4,
    TypeColor,                  // D3DCOLOR: bytes B, G, R, A (normalised)
    TypeUByte4,
    TypeShort2,
    TypeShort4,
    TypeUByte4N,
    TypeCount
};

struct VertexElement
{
    uint8_t     usage;          // Usage
    uint8_t     type;           // ElementType
    uint16_t    offset;
};

enum
{
    MaxVertexElements = 12
};

struct VertexLayout
{
    uint16_t        stride;
    uint8_t         count;
    bool            positionT;  // POSITIONT: window coordinates and 1/w
    VertexElement   elements[MaxVertexElements];
};

// Registers a layout (equal layouts share an id) and returns its id; game thread. Ids are small and stay valid.
uint8_t registerLayout(const VertexLayout& layout);
const VertexLayout& layout(uint8_t id);

// ---- Draw state ----------------------------------------------------------------------------------------------------

enum
{
    MaxSamplers = 4
};

enum Address : uint8_t
{
    AddressWrap,
    AddressClamp,
    AddressMirror
};

enum Mip : uint8_t
{
    MipNone,
    MipPoint,
    MipLinear
};

struct Sampler
{
    uint8_t addressU;           // Address
    uint8_t addressV;
    uint8_t filter;             // bit 0: min linear, bit 1: mag linear
    uint8_t mip;                // Mip
};

enum Primitive : uint8_t
{
    PrimitiveTriangles,
    PrimitiveStrip,
    PrimitiveFan
};

// Direct3D values: blend factors D3DBLEND (1..15), compare D3DCMP (1..8), cull D3DCULL (1 none, 2 cw, 3 ccw),
// colour mask D3DCOLORWRITEENABLE bits (1 r, 2 g, 4 b, 8 a).
struct DrawState
{
    const TextureStorage*   textures[MaxSamplers];
    Sampler                 samplers[MaxSamplers];
    uint8_t                 vertexProgram;
    uint8_t                 fragmentProgram;
    uint8_t                 layout;
    uint8_t                 primitive;
    uint8_t                 blendEnable;
    uint8_t                 srcBlend;
    uint8_t                 dstBlend;
    uint8_t                 colorMask;
    uint8_t                 alphaTest;      // the program variant with the alpha test
    uint8_t                 depthEnable;
    uint8_t                 depthWrite;
    uint8_t                 depthFunc;
    uint8_t                 cullMode;
    uint8_t                 pad[3];
};

struct Viewport
{
    float x, y, width, height;
    float minZ, maxZ;
};

enum : uint32_t
{
    NoConstants = 0xffffffffu
};

struct DrawCall
{
    DrawState       state;
    uint32_t        vertexConstants;    // float offset into Frame::constants, or NoConstants
    uint32_t        fragmentConstants;
    const void*     vertices;           // vertex 0 of the draw
    const uint16_t* indices;            // null: vertices in order
    uint32_t        count;              // indices or vertices
    uint32_t        vertexCount;        // vertices the indices reach (min index 0)
    uint16_t        viewport;           // index into Frame::viewports
};

// A render target: a target texture (a cube face) or the back buffer (null), and a depth surface or none.
struct Target
{
    const TextureStorage*   color;
    uint8_t                 face;
    const DepthStorage*     depth;      // null: no depth surface
};

struct Scene
{
    Target      target;
    uint16_t    width, height;
    uint32_t    firstDraw, drawCount;
    bool        depthCleared;           // the scene starts with a clear of the whole depth surface
    float       clearDepth;
};

// Arenas and arrays of a frame slot; the device sets the pointers and capacities, the layer the counts.
struct Frame
{
    uint32_t    index;                  // 1, 2, ... in submission order
    uint16_t    width, height;          // back buffer

    Scene*      scenes;
    uint32_t    sceneCount, sceneCapacity;
    DrawCall*   draws;
    uint32_t    drawCount, drawCapacity;
    Viewport*   viewports;
    uint32_t    viewportCount, viewportCapacity;
    float*      constants;
    uint32_t    constantCount, constantCapacity;   // floats
    uint8_t*    vertices;                           // GPU-visible arena for user-pointer draws
    uint32_t    vertexBytes, vertexCapacity;
};

// Arena helpers: null when the arena is full.
float* allocateConstants(Frame& frame, uint32_t count, uint32_t* offset);
void* allocateVertices(Frame& frame, uint32_t bytes);

inline bool operator==(const DrawState& a, const DrawState& b)
{
    return __builtin_memcmp(&a, &b, sizeof(DrawState)) == 0;
}

}}

#endif
