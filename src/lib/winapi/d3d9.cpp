// d3d9.dll: guest COM objects of the Direct3D layer, the vtables, and IDirect3D9.

#include <winapi/d3d9.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include <lib/fpucontrol.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <cstdio>

namespace win32 { namespace d3d9
{

namespace
{

enum : x86::reg32
{
    ObjectBytes     = 8,                // vtable, handle
    ObjectArena     = 1024 * 1024,
    VtableArena     = 64 * 1024
};

MemMap*                 s_objectMemory = nullptr;
MemMap*                 s_vtableMemory = nullptr;
x86::reg32              s_vtableNext = 0;
x86::reg32              s_vtables[KindCount] = {};
x86::reg32              s_objectStart = 0;          // guest range of s_objectMemory
x86::reg32              s_objectEnd = 0;
std::vector<Object*>    s_objects(1, nullptr);      // handle 0 is never used
std::vector<uint32_t>   s_freeHandles;
std::vector<x86::reg32> s_freeBlocks;
x86::reg32              s_blockNext = 0;
WinApplication*         s_app = nullptr;

// A method without an implementation: cpu.ip is the slot address the guest called through.
void unimplemented(WinApplication* app, x86::CPU& cpu)
{
    const char* name = app->getMethodName(cpu.ip);
    pf::logError("d3d9: %s is not implemented (guest method 0x%08x)", name ? name : "?", cpu.ip);
    x86::guestTrap(cpu.ip, "unimplemented Direct3D method");
}

x86::reg32 allocateBlock()
{
    if (!s_freeBlocks.empty())
    {
        const x86::reg32 block = s_freeBlocks.back();
        s_freeBlocks.pop_back();
        return block;
    }
    if (s_blockNext + ObjectBytes > s_objectMemory->getBlockSize())
        pf::fatal("d3d9: guest object memory exhausted");
    const x86::reg32 block = s_objectMemory->getBlockStart() + s_blockNext;
    s_blockNext += ObjectBytes;
    return block;
}

}

Object::~Object()
{
    if (!m_guest || !s_app)
        return;
    const uint32_t handle = s_app->getMemory<x86::reg32>(m_guest + 4);
    if (handle < s_objects.size() && s_objects[handle] == this)
    {
        s_objects[handle] = nullptr;
        s_freeHandles.push_back(handle);
    }
    s_app->getMemory<x86::reg32>(m_guest) = 0;
    s_app->getMemory<x86::reg32>(m_guest + 4) = 0;
    s_freeBlocks.push_back(m_guest);
}

ULONG Object::release()
{
    const ULONG refs = --m_refs;
    if (!refs)
        delete this;
    return refs;
}

HRESULT Object::QueryInterface(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* object)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(iid);
    // Every interface the game asks for is the object's own (or IUnknown).
    D3D_LOG_ONCE("d3d9: QueryInterface answers with the object itself");
    addRef();
    *object = m_guest;
    return D3D_OK;
}

ULONG Object::AddRef(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return addRef();
}

ULONG Object::Release(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return release();
}

x86::reg32 publish(WinApplication* app, Object* object)
{
    initInterfaces(app);
    uint32_t handle;
    if (!s_freeHandles.empty())
    {
        handle = s_freeHandles.back();
        s_freeHandles.pop_back();
        s_objects[handle] = object;
    }
    else
    {
        handle = uint32_t(s_objects.size());
        s_objects.push_back(object);
    }
    const x86::reg32 block = allocateBlock();
    app->getMemory<x86::reg32>(block) = s_vtables[object->kind()];
    app->getMemory<x86::reg32>(block + 4) = handle;
    object->m_guest = block;
    return block;
}

Object* lookup(x86::reg32 guest)
{
    if (!guest)
        return nullptr;
    if (guest - s_objectStart >= s_objectEnd - s_objectStart)
    {
        pf::logError("d3d9: 0x%08x is not a Direct3D object", guest);
        return nullptr;
    }
    const uint32_t handle = s_app->getMemory<x86::reg32>(guest + 4);
    if (handle >= s_objects.size() || !s_objects[handle])
    {
        pf::logError("d3d9: object 0x%08x was released", guest);
        return nullptr;
    }
    return s_objects[handle];
}

void buildInterface(WinApplication* app, Kind kind, const char* interfaceName, const MethodEntry* methods,
                    uint32_t count)
{
    if (s_vtableNext + count * 4 > s_vtableMemory->getBlockSize())
        pf::fatal("d3d9: vtable memory exhausted");
    const x86::reg32 vtable = s_vtableMemory->getBlockStart() + s_vtableNext;
    s_vtableNext += count * 4;
    for (uint32_t i = 0; i < count; ++i)
    {
        const x86::reg32 slot = vtable + 4 * i;
        app->getMemory<x86::reg32>(slot) = slot;
        char name[96];
        snprintf(name, sizeof(name), "%s::%s", interfaceName, methods[i].name);
        app->registerMethod(slot, { name, methods[i].method ? methods[i].method : &unimplemented });
    }
    s_vtables[kind] = vtable;
}

// ---- Vtables -------------------------------------------------------------------------------------------------------

namespace
{

#define D3D_UNKNOWN(cls) D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef), \
                         D3D_NAMED("Release", Object, Release)

const MethodEntry s_direct3D[] = {
    D3D_UNKNOWN(Direct3D),
    D3D_METHOD(Direct3D, RegisterSoftwareDevice),
    D3D_METHOD(Direct3D, GetAdapterCount),
    D3D_METHOD(Direct3D, GetAdapterIdentifier),
    D3D_METHOD(Direct3D, GetAdapterModeCount),
    D3D_METHOD(Direct3D, EnumAdapterModes),
    D3D_METHOD(Direct3D, GetAdapterDisplayMode),
    D3D_METHOD(Direct3D, CheckDeviceType),
    D3D_METHOD(Direct3D, CheckDeviceFormat),
    D3D_METHOD(Direct3D, CheckDeviceMultiSampleType),
    D3D_METHOD(Direct3D, CheckDepthStencilMatch),
    D3D_METHOD(Direct3D, CheckDeviceFormatConversion),
    D3D_METHOD(Direct3D, GetDeviceCaps),
    D3D_METHOD(Direct3D, GetAdapterMonitor),
    D3D_METHOD(Direct3D, CreateDevice),
};

const MethodEntry s_device[] = {
    D3D_UNKNOWN(Device),
    D3D_METHOD(Device, TestCooperativeLevel),
    D3D_METHOD(Device, GetAvailableTextureMem),
    D3D_METHOD(Device, EvictManagedResources),
    D3D_METHOD(Device, GetDirect3D),
    D3D_METHOD(Device, GetDeviceCaps),
    D3D_METHOD(Device, GetDisplayMode),
    D3D_METHOD(Device, GetCreationParameters),
    D3D_METHOD(Device, SetCursorProperties),
    D3D_METHOD(Device, SetCursorPosition),
    D3D_METHOD(Device, ShowCursor),
    D3D_TRAP(CreateAdditionalSwapChain),
    D3D_TRAP(GetSwapChain),
    D3D_METHOD(Device, GetNumberOfSwapChains),
    D3D_METHOD(Device, Reset),
    D3D_METHOD(Device, Present),
    D3D_METHOD(Device, GetBackBuffer),
    D3D_METHOD(Device, GetRasterStatus),
    D3D_METHOD(Device, SetDialogBoxMode),
    D3D_METHOD(Device, SetGammaRamp),
    D3D_METHOD(Device, GetGammaRamp),
    D3D_METHOD(Device, CreateTexture),
    D3D_TRAP(CreateVolumeTexture),
    D3D_METHOD(Device, CreateCubeTexture),
    D3D_METHOD(Device, CreateVertexBuffer),
    D3D_METHOD(Device, CreateIndexBuffer),
    D3D_METHOD(Device, CreateRenderTarget),
    D3D_METHOD(Device, CreateDepthStencilSurface),
    D3D_TRAP(UpdateSurface),
    D3D_TRAP(UpdateTexture),
    D3D_TRAP(GetRenderTargetData),
    D3D_TRAP(GetFrontBufferData),
    D3D_TRAP(StretchRect),
    D3D_TRAP(ColorFill),
    D3D_TRAP(CreateOffscreenPlainSurface),
    D3D_METHOD(Device, SetRenderTarget),
    D3D_METHOD(Device, GetRenderTarget),
    D3D_METHOD(Device, SetDepthStencilSurface),
    D3D_METHOD(Device, GetDepthStencilSurface),
    D3D_METHOD(Device, BeginScene),
    D3D_METHOD(Device, EndScene),
    D3D_METHOD(Device, Clear),
    D3D_GUEST(SetTransform),
    D3D_METHOD(Device, GetTransform),
    D3D_METHOD(Device, MultiplyTransform),
    D3D_METHOD(Device, SetViewport),
    D3D_METHOD(Device, GetViewport),
    D3D_GUEST(SetMaterial),
    D3D_METHOD(Device, GetMaterial),
    D3D_GUEST(SetLight),
    D3D_METHOD(Device, GetLight),
    D3D_GUEST(LightEnable),
    D3D_METHOD(Device, GetLightEnable),
    D3D_METHOD(Device, SetClipPlane),
    D3D_TRAP(GetClipPlane),
    D3D_GUEST(SetRenderState),
    D3D_METHOD(Device, GetRenderState),
    D3D_TRAP(CreateStateBlock),
    D3D_TRAP(BeginStateBlock),
    D3D_TRAP(EndStateBlock),
    D3D_TRAP(SetClipStatus),
    D3D_TRAP(GetClipStatus),
    D3D_METHOD(Device, GetTexture),
    D3D_GUEST(SetTexture),
    D3D_METHOD(Device, GetTextureStageState),
    D3D_METHOD(Device, SetTextureStageState),
    D3D_METHOD(Device, GetSamplerState),
    D3D_METHOD(Device, SetSamplerState),
    D3D_METHOD(Device, ValidateDevice),
    D3D_TRAP(SetPaletteEntries),
    D3D_TRAP(GetPaletteEntries),
    D3D_TRAP(SetCurrentTexturePalette),
    D3D_TRAP(GetCurrentTexturePalette),
    D3D_METHOD(Device, SetScissorRect),
    D3D_TRAP(GetScissorRect),
    D3D_METHOD(Device, SetSoftwareVertexProcessing),
    D3D_METHOD(Device, GetSoftwareVertexProcessing),
    D3D_METHOD(Device, SetNPatchMode),
    D3D_TRAP(GetNPatchMode),
    D3D_METHOD(Device, DrawPrimitive),
    D3D_GUEST(DrawIndexedPrimitive),
    D3D_GUEST(DrawPrimitiveUP),
    D3D_METHOD(Device, DrawIndexedPrimitiveUP),
    D3D_TRAP(ProcessVertices),
    D3D_METHOD(Device, CreateVertexDeclaration),
    D3D_GUEST(SetVertexDeclaration),
    D3D_METHOD(Device, GetVertexDeclaration),
    D3D_METHOD(Device, SetFVF),
    D3D_METHOD(Device, GetFVF),
    D3D_TRAP(CreateVertexShader),
    D3D_METHOD(Device, SetVertexShader),
    D3D_METHOD(Device, GetVertexShader),
    D3D_TRAP(SetVertexShaderConstantF),
    D3D_TRAP(GetVertexShaderConstantF),
    D3D_TRAP(SetVertexShaderConstantI),
    D3D_TRAP(GetVertexShaderConstantI),
    D3D_TRAP(SetVertexShaderConstantB),
    D3D_TRAP(GetVertexShaderConstantB),
    D3D_GUEST(SetStreamSource),
    D3D_METHOD(Device, GetStreamSource),
    D3D_METHOD(Device, SetStreamSourceFreq),
    D3D_TRAP(GetStreamSourceFreq),
    D3D_GUEST(SetIndices),
    D3D_METHOD(Device, GetIndices),
    D3D_TRAP(CreatePixelShader),
    D3D_METHOD(Device, SetPixelShader),
    D3D_METHOD(Device, GetPixelShader),
    D3D_TRAP(SetPixelShaderConstantF),
    D3D_TRAP(GetPixelShaderConstantF),
    D3D_TRAP(SetPixelShaderConstantI),
    D3D_TRAP(GetPixelShaderConstantI),
    D3D_TRAP(SetPixelShaderConstantB),
    D3D_TRAP(GetPixelShaderConstantB),
    D3D_TRAP(DrawRectPatch),
    D3D_TRAP(DrawTriPatch),
    D3D_TRAP(DeletePatch),
    D3D_TRAP(CreateQuery),
};
static_assert(sizeof(s_device) / sizeof(s_device[0]) == 119, "IDirect3DDevice9 has 119 methods");

#define D3D_RESOURCE(cls) D3D_UNKNOWN(cls), D3D_METHOD(cls, GetDevice), D3D_TRAP(SetPrivateData),            \
                          D3D_TRAP(GetPrivateData), D3D_TRAP(FreePrivateData), D3D_METHOD(cls, SetPriority),     \
                          D3D_METHOD(cls, GetPriority), D3D_METHOD(cls, PreLoad), D3D_METHOD(cls, GetType)

const MethodEntry s_texture[] = {
    D3D_RESOURCE(Texture),
    D3D_METHOD(Texture, SetLOD),
    D3D_METHOD(Texture, GetLOD),
    D3D_METHOD(Texture, GetLevelCount),
    D3D_TRAP(SetAutoGenFilterType),
    D3D_TRAP(GetAutoGenFilterType),
    D3D_TRAP(GenerateMipSubLevels),
    D3D_METHOD(Texture, GetLevelDesc),
    D3D_METHOD(Texture, GetSurfaceLevel),
    D3D_METHOD(Texture, LockRect),
    D3D_METHOD(Texture, UnlockRect),
    D3D_METHOD(Texture, AddDirtyRect),
};

const MethodEntry s_cubeTexture[] = {
    D3D_RESOURCE(Texture),
    D3D_METHOD(Texture, SetLOD),
    D3D_METHOD(Texture, GetLOD),
    D3D_METHOD(Texture, GetLevelCount),
    D3D_TRAP(SetAutoGenFilterType),
    D3D_TRAP(GetAutoGenFilterType),
    D3D_TRAP(GenerateMipSubLevels),
    D3D_METHOD(Texture, GetLevelDesc),
    D3D_METHOD(Texture, GetCubeMapSurface),
    D3D_NAMED("LockRect", Texture, LockRectCube),
    D3D_NAMED("UnlockRect", Texture, UnlockRectCube),
    D3D_NAMED("AddDirtyRect", Texture, AddDirtyRectCube),
};

const MethodEntry s_surface[] = {
    D3D_RESOURCE(Surface),
    D3D_METHOD(Surface, GetContainer),
    D3D_METHOD(Surface, GetDesc),
    D3D_METHOD(Surface, LockRect),
    D3D_METHOD(Surface, UnlockRect),
    D3D_TRAP(GetDC),
    D3D_TRAP(ReleaseDC),
};

const MethodEntry s_vertexBuffer[] = {
    D3D_RESOURCE(Buffer),
    D3D_METHOD(Buffer, Lock),
    D3D_METHOD(Buffer, Unlock),
    D3D_NAMED("GetDesc", Buffer, GetVertexDesc),
};

const MethodEntry s_indexBuffer[] = {
    D3D_RESOURCE(Buffer),
    D3D_METHOD(Buffer, Lock),
    D3D_METHOD(Buffer, Unlock),
    D3D_NAMED("GetDesc", Buffer, GetIndexDesc),
};

const MethodEntry s_vertexDeclaration[] = {
    D3D_UNKNOWN(VertexDeclaration),
    D3D_METHOD(VertexDeclaration, GetDevice),
    D3D_METHOD(VertexDeclaration, GetDeclaration),
};

#define D3D_BUILD(kind, name, table) buildInterface(app, kind, name, table, sizeof(table) / sizeof(table[0]))

}

void initInterfaces(WinApplication* app)
{
    if (s_app)
        return;
    s_app = app;
    s_objectMemory = new MemMap(ObjectArena);
    s_objectStart = s_objectMemory->getBlockStart();
    s_objectEnd = s_objectStart + s_objectMemory->getBlockSize();
    s_vtableMemory = new MemMap(VtableArena);
    D3D_BUILD(KindDirect3D, "IDirect3D9", s_direct3D);
    D3D_BUILD(KindDevice, "IDirect3DDevice9", s_device);
    D3D_BUILD(KindTexture, "IDirect3DTexture9", s_texture);
    D3D_BUILD(KindCubeTexture, "IDirect3DCubeTexture9", s_cubeTexture);
    D3D_BUILD(KindSurface, "IDirect3DSurface9", s_surface);
    D3D_BUILD(KindVertexBuffer, "IDirect3DVertexBuffer9", s_vertexBuffer);
    D3D_BUILD(KindIndexBuffer, "IDirect3DIndexBuffer9", s_indexBuffer);
    D3D_BUILD(KindVertexDeclaration, "IDirect3DVertexDeclaration9", s_vertexDeclaration);
    initEffectInterfaces(app);
    bridge::init(app);
}

// ---- IDirect3D9 ----------------------------------------------------------------------------------------------------

namespace
{

// Modes offered: the game's own 4:3 list and the Vita display. Back buffers of another size are scaled.
const D3DDISPLAYMODE s_modes[] = {
    { 640, 480, 60, D3DFMT_X8R8G8B8 },
    { 800, 600, 60, D3DFMT_X8R8G8B8 },
    { pf::gxm::DisplayWidth, pf::gxm::DisplayHeight, 60, D3DFMT_X8R8G8B8 },
    { 1024, 768, 60, D3DFMT_X8R8G8B8 },
    { 1280, 960, 60, D3DFMT_X8R8G8B8 },
};
const UINT s_modeCount = sizeof(s_modes) / sizeof(s_modes[0]);

// A Radeon 9700: outside every vendor gate of the effect loader, so technique 0 (the shaders) is chosen.
const DWORD VendorAti = 0x1002;
const DWORD DeviceRadeon9700 = 0x4E44;

}

HRESULT Direct3D::RegisterSoftwareDevice(WinApplication* app, x86::CPU& cpu, x86::reg32 init)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(init);
    return D3DERR_INVALIDCALL;
}

UINT Direct3D::GetAdapterCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

HRESULT Direct3D::GetAdapterIdentifier(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD flags,
                                       D3DADAPTER_IDENTIFIER9* identifier)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(flags);
    if (adapter != 0 || !identifier)
        return D3DERR_INVALIDCALL;
    memset(identifier, 0, sizeof(*identifier));
    strcpy(identifier->Driver, "d3d9.dll");
    strcpy(identifier->Description, "PS Vita SGX543MP4+ (GXM)");
    strcpy(identifier->DeviceName, "\\\\.\\DISPLAY1");
    identifier->DriverVersionHighPart = 0x00060000;
    identifier->DriverVersionLowPart = 0x0001;
    identifier->VendorId = VendorAti;
    identifier->DeviceId = DeviceRadeon9700;
    identifier->WHQLLevel = 1;
    return D3D_OK;
}

UINT Direct3D::GetAdapterModeCount(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format)
{
    RC_USE(app);
    RC_USE(cpu);
    return adapter == 0 && (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) ? s_modeCount : 0;
}

HRESULT Direct3D::EnumAdapterModes(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format, UINT mode,
                                   D3DDISPLAYMODE* out)
{
    if (mode >= GetAdapterModeCount(app, cpu, adapter, format) || !out)
        return D3DERR_INVALIDCALL;
    *out = s_modes[mode];
    out->Format = format;
    return D3D_OK;
}

HRESULT Direct3D::GetAdapterDisplayMode(WinApplication* app, x86::CPU& cpu, UINT adapter, D3DDISPLAYMODE* out)
{
    RC_USE(app);
    RC_USE(cpu);
    if (adapter != 0 || !out)
        return D3DERR_INVALIDCALL;
    *out = D3DDISPLAYMODE{ pf::gxm::DisplayWidth, pf::gxm::DisplayHeight, 60, D3DFMT_X8R8G8B8 };
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD displayFormat,
                                  DWORD backFormat, BOOL windowed)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    RC_USE(displayFormat);
    RC_USE(backFormat);
    RC_USE(windowed);
    return adapter == 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Direct3D::CheckDeviceFormat(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                    DWORD adapterFormat, DWORD usage, DWORD resourceType, DWORD format)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    RC_USE(adapterFormat);
    RC_USE(usage);
    RC_USE(resourceType);
    if (adapter != 0)
        return D3DERR_INVALIDCALL;
    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_DXT1:
    case D3DFMT_DXT3:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D16:
    case D3DFMT_INDEX16:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT Direct3D::CheckDeviceMultiSampleType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                             DWORD format, BOOL windowed, DWORD multiSample, DWORD* qualityLevels)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(adapter);
    RC_USE(type);
    RC_USE(format);
    RC_USE(windowed);
    if (qualityLevels)
        *qualityLevels = 1;
    return multiSample == 0 ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT Direct3D::CheckDepthStencilMatch(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                         DWORD adapterFormat, DWORD targetFormat, DWORD depthFormat)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(adapter);
    RC_USE(type);
    RC_USE(adapterFormat);
    RC_USE(targetFormat);
    RC_USE(depthFormat);
    return D3D_OK;
}

HRESULT Direct3D::CheckDeviceFormatConversion(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                              DWORD sourceFormat, DWORD targetFormat)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(adapter);
    RC_USE(type);
    RC_USE(sourceFormat);
    RC_USE(targetFormat);
    return D3DERR_NOTAVAILABLE;
}

void Direct3D::fillCaps(D3DCAPS9* caps)
{
    memset(caps, 0, sizeof(*caps));
    DWORD* c = caps->field;
    auto f = [](float v) { DWORD d; memcpy(&d, &v, 4); return d; };
    c[CapsDeviceType] = 1;                          // HAL
    c[CapsCaps2] = 0x00020000;                      // CANRENDERWINDOWED... DYNAMICTEXTURES
    c[CapsPresentationIntervals] = 0x80000003;      // IMMEDIATE, ONE, TWO
    c[CapsDevCaps] = 0x001BBEF0;
    c[CapsPrimitiveMiscCaps] = 0x000002F2;
    c[CapsRasterCaps] = 0x07732191;
    c[CapsZCmpCaps] = 0xFF;
    c[CapsSrcBlendCaps] = 0x1FFF;
    c[CapsDestBlendCaps] = 0x1FFF;
    c[CapsAlphaCmpCaps] = 0xFF;
    c[CapsShadeCaps] = 0x00084208;
    c[CapsTextureCaps] = 0x0001EC45;
    c[CapsTextureFilterCaps] = 0x03030300;
    c[CapsCubeTextureFilterCaps] = 0x03030300;
    c[CapsTextureAddressCaps] = 0x3F;
    c[CapsLineCaps] = 0x1F;
    c[CapsMaxTextureWidth] = 4096;
    c[CapsMaxTextureHeight] = 4096;
    c[CapsMaxTextureRepeat] = 8192;
    c[CapsMaxTextureAspectRatio] = 4096;
    c[CapsMaxAnisotropy] = 1;
    c[CapsMaxVertexW] = f(1e10f);
    c[CapsGuardBandLeft] = f(-8192.0f);
    c[CapsGuardBandTop] = f(-8192.0f);
    c[CapsGuardBandRight] = f(8192.0f);
    c[CapsGuardBandBottom] = f(8192.0f);
    c[CapsStencilCaps] = 0x1FF;
    c[CapsFVFCaps] = 0x00080008;
    c[CapsTextureOpCaps] = 0x03FFFFFF;
    c[CapsMaxTextureBlendStages] = 8;
    c[CapsMaxSimultaneousTextures] = 8;
    c[CapsVertexProcessingCaps] = 0x13B;
    c[CapsMaxActiveLights] = 8;
    c[CapsMaxUserClipPlanes] = 6;
    c[CapsMaxVertexBlendMatrices] = 4;
    c[CapsMaxVertexBlendMatrixIndex] = 0;
    c[CapsMaxPointSize] = f(256.0f);
    c[CapsMaxPrimitiveCount] = 0x000FFFFF;
    c[CapsMaxVertexIndex] = 0x00FFFFFF;
    c[CapsMaxStreams] = 16;
    c[CapsMaxStreamStride] = 255;
    c[CapsVertexShaderVersion] = 0xFFFE0101;
    c[CapsMaxVertexShaderConst] = 256;
    c[CapsPixelShaderVersion] = 0xFFFF0104;
    c[CapsPixelShader1xMaxValue] = f(8.0f);
    c[CapsNumberOfAdaptersInGroup] = 1;
    c[CapsDeclTypes] = 0x3;
    c[CapsNumSimultaneousRTs] = 1;
    c[CapsStretchRectFilterCaps] = 0x03000300;
}

HRESULT Direct3D::GetDeviceCaps(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, D3DCAPS9* caps)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    if (adapter != 0 || !caps)
        return D3DERR_INVALIDCALL;
    fillCaps(caps);
    return D3D_OK;
}

HANDLE Direct3D::GetAdapterMonitor(WinApplication* app, x86::CPU& cpu, UINT adapter)
{
    RC_USE(app);
    RC_USE(cpu);
    return adapter == 0 ? 1 : 0;
}

HRESULT Direct3D::CreateDevice(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, HWND window,
                               DWORD flags, D3DPRESENT_PARAMETERS* params, x86::reg32* device)
{
    if (adapter != 0 || !params || !device)
        return D3DERR_INVALIDCALL;
    // Without D3DCREATE_FPU_PRESERVE the x87 control word becomes 0x007F, as on Windows.
    fpucontrol::deviceCreated(cpu, flags);
    pf::logInfo("d3d9: CreateDevice type %u flags 0x%x, back buffer %ux%u format %u, depth %u (%u), windowed %u",
                type, flags, params->BackBufferWidth, params->BackBufferHeight, params->BackBufferFormat,
                params->EnableAutoDepthStencil, params->AutoDepthStencilFormat, params->Windowed);
    Device* d = new Device(app, this, *params, window, flags);
    *device = publish(app, d);
    return D3D_OK;
}

x86::reg32 Direct3DCreate9(WinApplication* app, x86::CPU& cpu, UINT SDKVersion)
{
    RC_USE(cpu);
    if (SDKVersion != 31 && SDKVersion != 32)
    {
        pf::logError("d3d9: Direct3DCreate9(%u): unsupported SDK version", SDKVersion);
        return 0;
    }
    return publish(app, new Direct3D());
}

}}
