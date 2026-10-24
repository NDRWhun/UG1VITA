#ifndef WINAPI_D3D9_INTERNAL_H_
#define WINAPI_D3D9_INTERNAL_H_

// Direct3D 9 layer: COM objects in guest memory, Direct3D types, the device state machine that records frames
// (platform/render.h) and the binding the effect layer (d3dx9.cpp) gives the device.
//
// A guest COM object is 8 bytes of guest memory: its vtable and a handle. The handle indexes the host object
// table; vtable slots hold their own guest address, under which the method is registered.

#include <lib/winapp.h>
#include <lib/memmap.h>
#include <winapi/types.h>
#include <winapi/wrapper.h>
#include <platform/log.h>
#include <platform/render.h>
#include <cstring>
#include <vector>

namespace win32 { namespace d3d9
{

namespace render = pf::render;
namespace texture = pf::texture;

// Bitwise equality of count dwords at a and b.
inline bool sameWords(const void* a, const void* b, uint32_t count)
{
    const uint8_t* p = static_cast<const uint8_t*>(a);
    const uint8_t* q = static_cast<const uint8_t*>(b);
    for (uint32_t i = 0; i < count; ++i, p += 4, q += 4)
    {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, q, 4);
        if (x != y)
            return false;
    }
    return true;
}

// ---- Constants and types -------------------------------------------------------------------------------------------

enum : HRESULT
{
    D3D_OK                  = 0,
    D3DERR_NOTFOUND         = 0x88760866,
    D3DERR_NOTAVAILABLE     = 0x8876086A,
    D3DERR_INVALIDCALL      = 0x8876086C,
    D3DERR_OUTOFVIDEOMEMORY = 0x8876017C,
    E_NOINTERFACE           = 0x80004002,
    E_FAIL                  = 0x80004005
};

enum : DWORD
{
    D3DFMT_UNKNOWN  = 0,
    D3DFMT_A8R8G8B8 = 21,
    D3DFMT_X8R8G8B8 = 22,
    D3DFMT_D24S8    = 75,
    D3DFMT_D24X8    = 77,
    D3DFMT_D16      = 80,
    D3DFMT_INDEX16  = 101,
    D3DFMT_INDEX32  = 102,
    D3DFMT_DXT1     = 0x31545844,
    D3DFMT_DXT3     = 0x33545844,
    D3DFMT_DXT5     = 0x35545844
};

enum : DWORD
{
    D3DUSAGE_RENDERTARGET   = 0x1,
    D3DUSAGE_DEPTHSTENCIL   = 0x2,
    D3DUSAGE_DYNAMIC        = 0x200,

    D3DLOCK_READONLY        = 0x10,
    D3DLOCK_DISCARD         = 0x2000,
    D3DLOCK_NOOVERWRITE     = 0x1000,

    D3DRTYPE_SURFACE        = 1,
    D3DRTYPE_TEXTURE        = 3,
    D3DRTYPE_CUBETEXTURE    = 5,
    D3DRTYPE_VERTEXBUFFER   = 6,
    D3DRTYPE_INDEXBUFFER    = 7
};

struct D3DMATRIX
{
    float m[16];        // rows
};

struct D3DVIEWPORT9
{
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

struct D3DPRESENT_PARAMETERS
{
    UINT    BackBufferWidth;
    UINT    BackBufferHeight;
    DWORD   BackBufferFormat;
    UINT    BackBufferCount;
    DWORD   MultiSampleType;
    DWORD   MultiSampleQuality;
    DWORD   SwapEffect;
    HWND    hDeviceWindow;
    BOOL    Windowed;
    BOOL    EnableAutoDepthStencil;
    DWORD   AutoDepthStencilFormat;
    DWORD   Flags;
    UINT    FullScreen_RefreshRateInHz;
    UINT    PresentationInterval;
};

struct D3DDISPLAYMODE
{
    UINT    Width, Height, RefreshRate;
    DWORD   Format;
};

struct D3DLOCKED_RECT
{
    LONG    Pitch;
    DWORD   pBits;      // guest address
};

struct D3DSURFACE_DESC
{
    DWORD   Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
    UINT    Width, Height;
};

struct D3DVERTEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
    DWORD   FVF;
};

struct D3DINDEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
};

struct D3DVERTEXELEMENT9
{
    WORD    Stream;
    WORD    Offset;
    BYTE    Type;
    BYTE    Method;
    BYTE    Usage;
    BYTE    UsageIndex;
};

struct D3DRECT
{
    LONG x1, y1, x2, y2;
};

struct D3DADAPTER_IDENTIFIER9
{
    char    Driver[512];
    char    Description[512];
    char    DeviceName[32];
    DWORD   DriverVersionLowPart;
    DWORD   DriverVersionHighPart;
    DWORD   VendorId;
    DWORD   DeviceId;
    DWORD   SubSysId;
    DWORD   Revision;
    GUID    DeviceIdentifier;
    DWORD   WHQLLevel;
};

struct D3DDEVICE_CREATION_PARAMETERS
{
    UINT    AdapterOrdinal;
    DWORD   DeviceType;
    HWND    hFocusWindow;
    DWORD   BehaviorFlags;
};

// D3DCAPS9 as 76 DWORDs; the indices of the fields this layer sets.
enum CapsField
{
    CapsDeviceType = 0, CapsAdapterOrdinal, CapsCaps, CapsCaps2, CapsCaps3, CapsPresentationIntervals,
    CapsCursorCaps, CapsDevCaps, CapsPrimitiveMiscCaps, CapsRasterCaps, CapsZCmpCaps, CapsSrcBlendCaps,
    CapsDestBlendCaps, CapsAlphaCmpCaps, CapsShadeCaps, CapsTextureCaps, CapsTextureFilterCaps,
    CapsCubeTextureFilterCaps, CapsVolumeTextureFilterCaps, CapsTextureAddressCaps, CapsVolumeTextureAddressCaps,
    CapsLineCaps, CapsMaxTextureWidth, CapsMaxTextureHeight, CapsMaxVolumeExtent, CapsMaxTextureRepeat,
    CapsMaxTextureAspectRatio, CapsMaxAnisotropy, CapsMaxVertexW, CapsGuardBandLeft, CapsGuardBandTop,
    CapsGuardBandRight, CapsGuardBandBottom, CapsExtentsAdjust, CapsStencilCaps, CapsFVFCaps, CapsTextureOpCaps,
    CapsMaxTextureBlendStages, CapsMaxSimultaneousTextures, CapsVertexProcessingCaps, CapsMaxActiveLights,
    CapsMaxUserClipPlanes, CapsMaxVertexBlendMatrices, CapsMaxVertexBlendMatrixIndex, CapsMaxPointSize,
    CapsMaxPrimitiveCount, CapsMaxVertexIndex, CapsMaxStreams, CapsMaxStreamStride, CapsVertexShaderVersion,
    CapsMaxVertexShaderConst, CapsPixelShaderVersion, CapsPixelShader1xMaxValue, CapsDevCaps2,
    CapsMaxNpatchTessellationLevel, CapsReserved5, CapsMasterAdapterOrdinal, CapsAdapterOrdinalInGroup,
    CapsNumberOfAdaptersInGroup, CapsDeclTypes, CapsNumSimultaneousRTs, CapsStretchRectFilterCaps,
    CapsCount = 76
};

struct D3DCAPS9
{
    DWORD field[CapsCount];
};

// ---- Objects -------------------------------------------------------------------------------------------------------

enum Kind
{
    KindDirect3D,
    KindDevice,
    KindTexture,
    KindCubeTexture,
    KindSurface,
    KindVertexBuffer,
    KindIndexBuffer,
    KindVertexDeclaration,
    KindEffect,
    KindEffectPool,
    KindCount
};

class Object
{
public:
    explicit Object(Kind kind) : m_kind(kind), m_refs(1), m_guest(0) {}
    virtual ~Object();

    Kind kind() const               { return m_kind; }
    x86::reg32 guest() const        { return m_guest; }
    // Deletes the object (and frees its guest block) on the last reference. Texture surfaces count their
    // container's references, as in Direct3D.
    virtual ULONG addRef()          { return ++m_refs; }
    virtual ULONG release();

    HRESULT QueryInterface(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* object);
    ULONG AddRef(WinApplication* app, x86::CPU& cpu);
    ULONG Release(WinApplication* app, x86::CPU& cpu);

private:
    friend x86::reg32 publish(WinApplication* app, Object* object);

    Kind        m_kind;
    uint32_t    m_refs;
    x86::reg32  m_guest;
};

// Gives a host object its guest COM object (the vtable of its kind) and returns the guest address.
x86::reg32 publish(WinApplication* app, Object* object);
// The host object of a guest COM pointer; null for 0 or an unknown pointer (logged).
Object* lookup(x86::reg32 guest);
// Builds the vtables and registers the methods, once.
void initInterfaces(WinApplication* app);

// A vtable slot: the method, or null for one that logs its name and guest address and traps.
struct MethodEntry
{
    const char* name;
    MethodPtr   method;
};
// Builds the vtable of a kind; slots in interface order.
void buildInterface(WinApplication* app, Kind kind, const char* interfaceName, const MethodEntry* methods,
                    uint32_t count);
// The effect interfaces (d3dx9.cpp).
void initEffectInterfaces(WinApplication* app);

// Guest entries of the methods native code calls directly (d3d9_native.cpp, d3d9_native.h).
namespace guest
{

void SetTransform(WinApplication* app, x86::CPU& cpu);
void SetMaterial(WinApplication* app, x86::CPU& cpu);
void SetLight(WinApplication* app, x86::CPU& cpu);
void LightEnable(WinApplication* app, x86::CPU& cpu);
void SetRenderState(WinApplication* app, x86::CPU& cpu);
void SetTexture(WinApplication* app, x86::CPU& cpu);
void SetVertexDeclaration(WinApplication* app, x86::CPU& cpu);
void EffectSetInt(WinApplication* app, x86::CPU& cpu);
void EffectSetFloat(WinApplication* app, x86::CPU& cpu);
void EffectSetVector(WinApplication* app, x86::CPU& cpu);
void EffectSetVectorArray(WinApplication* app, x86::CPU& cpu);
void EffectSetMatrix(WinApplication* app, x86::CPU& cpu);
void EffectSetMatrixArray(WinApplication* app, x86::CPU& cpu);
void EffectSetTexture(WinApplication* app, x86::CPU& cpu);
void EffectBegin(WinApplication* app, x86::CPU& cpu);
void EffectPass(WinApplication* app, x86::CPU& cpu);
void EffectEnd(WinApplication* app, x86::CPU& cpu);

}

#define D3D_GUEST(fn) { #fn, &win32::d3d9::guest::fn }
#define D3D_GUEST_NAMED(name, fn) { name, &win32::d3d9::guest::fn }

#define D3D_METHOD(cls, fn) { #fn, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_NAMED(name, cls, fn) { name, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_TRAP(fn) { #fn, nullptr }

// Logs a message once per call site.
#define D3D_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; pf::logInfo(__VA_ARGS__); } } while (0)

class Device;
class Texture;
struct KeptLevel;
class Surface;
class Buffer;
class VertexDeclaration;

class Direct3D : public Object
{
public:
    Direct3D() : Object(KindDirect3D) {}

    HRESULT RegisterSoftwareDevice(WinApplication* app, x86::CPU& cpu, x86::reg32 init);
    UINT GetAdapterCount(WinApplication* app, x86::CPU& cpu);
    HRESULT GetAdapterIdentifier(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD flags,
                                 D3DADAPTER_IDENTIFIER9* identifier);
    UINT GetAdapterModeCount(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format);
    HRESULT EnumAdapterModes(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format, UINT mode,
                             D3DDISPLAYMODE* out);
    HRESULT GetAdapterDisplayMode(WinApplication* app, x86::CPU& cpu, UINT adapter, D3DDISPLAYMODE* out);
    HRESULT CheckDeviceType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD displayFormat,
                            DWORD backFormat, BOOL windowed);
    HRESULT CheckDeviceFormat(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                              DWORD usage, DWORD resourceType, DWORD format);
    HRESULT CheckDeviceMultiSampleType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD format,
                                       BOOL windowed, DWORD multiSample, DWORD* qualityLevels);
    HRESULT CheckDepthStencilMatch(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                                   DWORD targetFormat, DWORD depthFormat);
    HRESULT CheckDeviceFormatConversion(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                        DWORD sourceFormat, DWORD targetFormat);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, D3DCAPS9* caps);
    HANDLE GetAdapterMonitor(WinApplication* app, x86::CPU& cpu, UINT adapter);
    HRESULT CreateDevice(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, HWND window, DWORD flags,
                         D3DPRESENT_PARAMETERS* params, x86::reg32* device);

    static void fillCaps(D3DCAPS9* caps);
};

// Textures, cube textures and their surfaces.
class Texture : public Object
{
public:
    Texture(Device* device, const render::TextureDesc& desc, DWORD format, DWORD usage, DWORD pool);
    ~Texture() override;

    render::TextureStorage* storage() const    { return m_storage; }
    const render::TextureDesc& desc() const    { return m_desc; }
    DWORD format() const                        { return m_format; }
    Device* device() const                      { return m_device; }
    bool valid() const                          { return m_storage != nullptr; }
    void fillDesc(UINT level, D3DSURFACE_DESC* desc) const;
    // The surface of a level (and face), kept while the texture lives; one reference added for the caller.
    Surface* surface(WinApplication* app, uint32_t face, uint32_t level);
    // Game thread, once per frame: drops kept linear copies over the byte cap whose uploads are finished.
    static void trimKept();

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    DWORD SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod);
    DWORD GetLOD(WinApplication* app, x86::CPU& cpu);
    DWORD GetLevelCount(WinApplication* app, x86::CPU& cpu);
    HRESULT GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DSURFACE_DESC* desc);
    HRESULT GetSurfaceLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* surface);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                     DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu, UINT level);
    HRESULT AddDirtyRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT GetCubeMapSurface(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, x86::reg32* surface);
    HRESULT LockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, D3DLOCKED_RECT* locked,
                         const RECT* rect, DWORD flags);
    HRESULT UnlockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level);
    HRESULT AddDirtyRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, const RECT* rect);

private:
    HRESULT lock(uint32_t face, uint32_t level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT unlock(uint32_t face, uint32_t level);
    // The level's kept linear copy, taken out of the cache; null when there is none.
    MemMap* takeKept(uint32_t index, uint32_t* ticket);
    void keep(uint32_t index, MemMap* memory, uint32_t ticket);

    friend struct KeptLevels;

    Device*                 m_device;
    render::TextureDesc     m_desc;
    DWORD                   m_format, m_usage, m_pool;
    render::TextureStorage* m_storage;
    std::vector<MemMap*>    m_locks;        // per face * levels + level: the guest buffer of a lock
    std::vector<uint32_t>   m_tickets;      // the upload reading a locked buffer (0: none)
    std::vector<DWORD>      m_lockFlags;
    std::vector<bool>       m_written;      // the level has been written since creation
    std::vector<uint8_t>    m_lockCounts;   // locks so far, saturating at 2
    std::vector<KeptLevel*> m_kept;         // the linear copy kept after the last unlock (KeptLevels lock)
    std::vector<Surface*>   m_surfaces;
};

class Surface : public Object
{
public:
    enum Role
    {
        RoleLevel,          // a level (face) of a texture
        RoleBackBuffer,
        RoleDepth
    };

    Surface(Device* device, Role role, uint32_t width, uint32_t height, DWORD format);
    ~Surface() override;

    Role role() const                       { return m_role; }
    uint32_t width() const                  { return m_width; }
    uint32_t height() const                 { return m_height; }
    Texture* container() const              { return m_container; }
    uint32_t face() const                   { return m_face; }
    uint32_t level() const                  { return m_level; }
    render::DepthStorage* depth() const     { return m_depth; }
    void setContainer(Texture* texture, uint32_t face, uint32_t level);
    void setDepth(render::DepthStorage* depth) { m_depth = depth; }
    ULONG addRef() override;
    ULONG release() override;

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container);
    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, D3DSURFACE_DESC* desc);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu);

private:
    Device*                 m_device;
    Role                    m_role;
    uint32_t                m_width, m_height;
    DWORD                   m_format;
    Texture*                m_container = nullptr;
    uint32_t                m_face = 0, m_level = 0;
    render::DepthStorage*   m_depth = nullptr;      // owned for RoleDepth
};

// Vertex and index buffers: the guest writes a shadow copy in guest memory; unlocks copy into the GPU buffer,
// a new one when the current is still read by frames in flight (unless NOOVERWRITE).
class Buffer : public Object
{
public:
    Buffer(Device* device, Kind kind, UINT size, DWORD usage, DWORD pool, DWORD format);
    ~Buffer() override;

    render::BufferStorage* storage() const  { return m_storage; }
    bool valid() const                      { return m_storage != nullptr; }
    UINT size() const                       { return m_size; }
    DWORD format() const                    { return m_format; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT Lock(WinApplication* app, x86::CPU& cpu, UINT offset, UINT size, x86::reg32* data, DWORD flags);
    HRESULT Unlock(WinApplication* app, x86::CPU& cpu);
    HRESULT GetVertexDesc(WinApplication* app, x86::CPU& cpu, D3DVERTEXBUFFER_DESC* desc);
    HRESULT GetIndexDesc(WinApplication* app, x86::CPU& cpu, D3DINDEXBUFFER_DESC* desc);

private:
    Device*                 m_device;
    UINT                    m_size;
    DWORD                   m_usage, m_pool, m_format;
    MemMap*                 m_shadow;
    render::BufferStorage*  m_storage;
    UINT                    m_lockOffset = 0, m_lockSize = 0;
    DWORD                   m_lockFlags = 0;
    int                     m_locks = 0;
};

class VertexDeclaration : public Object
{
public:
    VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t count, DWORD fvf);

    DWORD fvf() const                           { return m_fvf; }
    // The layout of these elements at a stream stride (registered once per stride).
    uint8_t layout(uint32_t stride);
    // Components of the TEXCOORD0 input (0: none).
    uint32_t texComponents() const              { return m_texComponents; }
    bool positionT() const                      { return m_layout.positionT; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT GetDeclaration(WinApplication* app, x86::CPU& cpu, D3DVERTEXELEMENT9* elements, UINT* count);

    // Layout of an FVF code; stride from the FVF.
    static render::VertexLayout fvfLayout(DWORD fvf, uint32_t* texComponents);

private:
    Device*                         m_device;
    std::vector<D3DVERTEXELEMENT9>  m_elements;
    DWORD                           m_fvf;
    render::VertexLayout            m_layout;   // stride 0
    uint32_t                        m_texComponents = 0;
    uint32_t                        m_stride = 0;
    uint8_t                         m_layoutId = 0;
};

// ---- Recording: the game thread records state changes and draws; the render thread resolves them -----------------

// Commands of a frame's stream: a header word (Command in bits 0-7, arguments above) and payload words.
enum Command : uint8_t
{
    CmdRenderState,         // header: state << 8; value
    CmdStageState,          // header: stage << 8 | type << 16; value
    CmdSamplerState,        // header: sampler << 8 | type << 16; value
    CmdTransform,           // header: transform << 8 (Transform); 16 floats
    CmdViewport,            // D3DVIEWPORT9
    CmdFixedState,          // FixedState, whole
    CmdValues,              // header: block << 8; offset | count << 16; count floats
    CmdDraw                 // DrawRecord
};

enum Transform : uint8_t
{
    TransformWorld, TransformView, TransformProjection, TransformTexture0, TransformCount
};

// Growing word buffer, one per frame in flight.
struct Stream
{
    uint32_t*   data = nullptr;
    uint32_t    size = 0, capacity = 0;

    uint32_t* append(uint32_t words)
    {
        if (size + words > capacity)
            grow(words);
        uint32_t* p = data + size;
        size += words;
        return p;
    }
    void grow(uint32_t words);
};

// The stream of a frame index, emptied; game thread.
Stream& frameStream(uint32_t frameIndex);
// Render thread: completes the draws of a frame from its stream (Frame::resolve).
void resolveFrame(render::Frame& frame);

// Device state the draws read, mirrored on the render thread.
struct FixedState
{
    DWORD           renderStates[256];
    DWORD           stageStates[8][33];
    DWORD           samplerStates[16][14];
    D3DMATRIX       transforms[TransformCount];
    D3DVIEWPORT9    viewport;
};

enum : uint16_t
{
    NoBlock = 0xffff
};
enum : uint8_t
{
    NoEffect = 0xff
};

// The active effect pass of a draw: its kind and value blocks (parameters, shared parameters of its pool).
struct EffectDraw
{
    uint16_t    own, shared;
    uint8_t     kind, pass;     // kind NoEffect: fixed function
    uint8_t     pad[2];
};

struct DrawRecord
{
    uint32_t                        draw;       // index in Frame::draws; the game thread filled its geometry
    uint8_t                         primitive, layout, texComponents, positionT;
    EffectDraw                      effect;
    const render::TextureStorage*   textures[render::MaxSamplers];  // stage 0, or the pass's samplers
};

// ---- Effects (d3dx9.cpp) -------------------------------------------------------------------------------------------

struct EffectBinding
{
    uint8_t         vertexProgram;          // render::VertexProgram
    uint8_t         fragmentProgram;
    uint8_t         samplerCount;
    render::Sampler samplers[render::MaxSamplers];
    bool            cullNone;
    bool            depthOff;
    bool            alphaTest;
    DWORD           alphaRef;
    bool            blend;
    DWORD           srcBlend, dstBlend;
};

class EffectPass
{
public:
    virtual ~EffectPass() {}
    // Game thread, per draw: the pass and the storages of its samplers (the rest null).
    virtual void record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers]) const = 0;
};

// Game thread: value blocks changed while no frame recorded, sent whole.
void syncEffectValues(Stream& stream);
// Game thread: every value block sent whole with the next frame.
void markEffectValuesDirty();
// Render thread: floats of a value block, and what a pass draws with from mirrored blocks (shared may be null).
// projected: stage 0 TEXTURETRANSFORMFLAGS has PROJECTED and the draw has four texture coordinates (ps_1_1 divides
// them by the last one).
uint32_t effectBlockFloats();
void effectBinding(const EffectDraw& effect, const float* own, const float* shared, bool projected,
                   EffectBinding& binding);
void effectVertexConstants(const EffectDraw& effect, const float* own, const float* shared, float* out);
void effectFragmentConstants(const EffectDraw& effect, const float* own, const float* shared, float* out);

// ---- Fixed-function resolution (d3d9_state.cpp, pure) ----------------------------------------------------------------

// Texture stage state indices and values used here.
enum : DWORD
{
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_COUNT = 33,

    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5,
    D3DTOP_MODULATE4X = 6, D3DTOP_ADD = 7,

    D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2, D3DTA_TFACTOR = 3, D3DTA_SELECTMASK = 0xf,

    D3DTTFF_DISABLE = 0, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3, D3DTTFF_COUNT4 = 4, D3DTTFF_PROJECTED = 0x100
};

struct FixedStage
{
    DWORD colorOp, colorArg1, colorArg2;
    DWORD alphaOp, alphaArg1, alphaArg2;
    DWORD transformFlags;
    bool  textured;         // a texture is bound to stage 0
};

struct FixedResult
{
    uint8_t fragmentProgram;    // FpFixedColor, FpFixedTexture or FpFixedProjected
    float   textureMask[4];
    float   scale[4];
    float   addMask[4];
    float   diffuseMask[4];
    float   factorMask[4];
};

// Stage 0 colour and alpha operations as program and masks; unsupported operations are logged and approximated.
FixedResult resolveFixed(const FixedStage& stage);
// Texture coordinate padding for an input of n components: missing components become (.., 1, 0).
void texturePad(uint32_t components, float pad[4]);
// Alpha test constants (sign, threshold) for D3DCMP func and ALPHAREF; false: the test always passes.
bool alphaTest(DWORD func, DWORD ref, float out[2]);

struct BlendFactorDraw
{
    DWORD   src, dst;           // D3DBLEND; BLENDFACTOR / INVBLENDFACTOR replaced
    uint8_t output;             // render::FactorOutput
    float   factor[4];          // uBlendFactor (rgba)
};
// BLENDFACTOR (14) and INVBLENDFACTOR (15) blends as fixed factors and a program factor output. variants: the
// program has factor variants and no alpha test; alphaWrite: the colour mask writes alpha. What is not exact is
// logged and approximated (14 as ONE, 15 as ZERO).
BlendFactorDraw resolveBlendFactor(DWORD src, DWORD dst, DWORD op, DWORD factor, bool variants, bool alphaWrite);
// Row-vector product out = a * b.
void multiply(const float* a, const float* b, float* out);

// ---- Device --------------------------------------------------------------------------------------------------------

enum : DWORD
{
    MaxRenderStates = 256,
    MaxStages = 8,
    MaxSamplerStates = 14,
    MaxSamplerStages = 16
};

class Device : public Object
{
public:
    Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags);
    ~Device() override;

    void setEffectPass(const EffectPass* pass)  { m_effectPass = pass; }
    const EffectPass* effectPass() const        { return m_effectPass; }
    // The stream of the frame being recorded; null between frames.
    Stream* stream() const                      { return m_stream; }

    HRESULT TestCooperativeLevel(WinApplication* app, x86::CPU& cpu);
    UINT GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu);
    HRESULT EvictManagedResources(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps);
    HRESULT GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode);
    HRESULT GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params);
    HRESULT SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface);
    void SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags);
    BOOL ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show);
    UINT GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu);
    HRESULT Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params);
    HRESULT Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                    x86::reg32 dirtyRegion);
    HRESULT GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                          x86::reg32* surface);
    HRESULT GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status);
    void SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable);
    void SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp);
    void GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp);
    HRESULT CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                          DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage, DWORD format,
                              DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf, DWORD pool,
                               x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format, DWORD pool,
                              x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                               DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                               x86::reg32 sharedHandle);
    HRESULT CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                      DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                      x86::reg32 sharedHandle);
    HRESULT SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface);
    HRESULT GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface);
    HRESULT SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface);
    HRESULT GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface);
    HRESULT BeginScene(WinApplication* app, x86::CPU& cpu);
    HRESULT EndScene(WinApplication* app, x86::CPU& cpu);
    HRESULT Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags, DWORD color,
                  float z, DWORD stencil);
    HRESULT SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix);
    HRESULT MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport);
    HRESULT GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport);
    HRESULT SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material);
    HRESULT GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material);
    HRESULT SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light);
    HRESULT GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light);
    HRESULT LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable);
    HRESULT GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable);
    HRESULT SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane);
    HRESULT SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value);
    HRESULT GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture);
    HRESULT GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value);
    HRESULT SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value);
    HRESULT GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value);
    HRESULT SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value);
    HRESULT ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes);
    HRESULT SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software);
    BOOL GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu);
    HRESULT SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments);
    HRESULT DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count);
    HRESULT DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                 UINT vertices, UINT startIndex, UINT count);
    HRESULT DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                            UINT stride);
    HRESULT DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                   UINT count, const void* indices, DWORD indexFormat, const void* data,
                                   UINT stride);
    HRESULT CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                    x86::reg32* declaration);
    HRESULT SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration);
    HRESULT GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration);
    HRESULT SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf);
    HRESULT GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf);
    HRESULT SetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader);
    HRESULT GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);
    HRESULT SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                            UINT stride);
    HRESULT GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                            UINT* stride);
    HRESULT SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider);
    HRESULT SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer);
    HRESULT GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer);
    HRESULT SetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader);
    HRESULT GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);

private:
    struct State : FixedState
    {
        Object*             textures[MaxSamplerStages];
        VertexDeclaration*  declaration;
        DWORD               fvf;
        Buffer*             stream;
        UINT                streamOffset, streamStride;
        Buffer*             indices;
        Surface*            target;
        Surface*            depth;
    };

    void defaults();
    void resetBackBuffer(const D3DPRESENT_PARAMETERS& params);
    template< typename T > void hold(T*& slot, T* object);
    render::Frame* frame(WinApplication* app, x86::CPU& cpu);
    render::Scene* scene();
    uint16_t viewportIndex(render::Frame& frame, const render::Viewport& viewport);
    // Records a state change for the render thread (header and one value word).
    void record(uint32_t header, DWORD value);
    void recordWords(uint32_t header, const void* payload, uint32_t words);
    void submit();
    HRESULT draw(WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices,
                 const uint16_t* indices, uint32_t vertexCount, uint8_t layout, uint32_t texComponents,
                 bool positionT, render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage);
    VertexDeclaration* fvfDeclaration(DWORD fvf);

    WinApplication*         m_app;
    Direct3D*               m_d3d;
    D3DPRESENT_PARAMETERS   m_params;
    HWND                    m_window;
    DWORD                   m_flags;
    State                   m_state;
    Surface*                m_backBuffer = nullptr;
    Surface*                m_autoDepth = nullptr;
    const EffectPass*       m_effectPass = nullptr;
    std::vector<VertexDeclaration*> m_fvfDeclarations;
    DWORD                   m_lights[8][26] = {};
    BOOL                    m_lightEnabled[8] = {};
    DWORD                   m_material[17] = {};

    // Recording
    render::Frame*          m_frame = nullptr;
    bool                    m_sceneOpen = false;    // m_frame's last scene is the current target
    uint32_t                m_vblanks;
    bool                    m_frameFull = false;
    Stream*                 m_stream = nullptr;
    bool                    m_stateDirty = true;    // changed while no frame recorded: sent whole before a draw
};

}}

namespace win32
{

// Guest COM pointers as host objects in method arguments (null for 0).
#define D3D9_UNPACKER(type)                                                     \
template<> struct Unpacker<type*>                                               \
{                                                                               \
    static inline type* unpack(WinApplication* app, x86::reg32 address)        \
    {                                                                           \
        return static_cast<type*>(d3d9::lookup(app->getMemory<x86::reg32>(address))); \
    }                                                                           \
};

D3D9_UNPACKER(d3d9::Object)
D3D9_UNPACKER(d3d9::Direct3D)
D3D9_UNPACKER(d3d9::Device)
D3D9_UNPACKER(d3d9::Texture)
D3D9_UNPACKER(d3d9::Surface)
D3D9_UNPACKER(d3d9::Buffer)
D3D9_UNPACKER(d3d9::VertexDeclaration)

}

#endif
