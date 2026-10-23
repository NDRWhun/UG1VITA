// IDirect3DDevice9: the device state machine on the game thread. Draws and clears are recorded into the frame
// of platform/render.h; Present hands the frame to the device (render thread on the Vita).

#include <winapi/d3d9_internal.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>

namespace win32 { namespace d3d9
{

namespace
{

pf::gxm::Device& gpu()
{
    return pf::gxm::Device::get();
}

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193,

    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0 = 16, D3DTS_WORLD = 256,

    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7,

    D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2,

    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
};

const D3DMATRIX s_identity = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };

// Perf builds: game thread time spent in the Draw* methods, logged every 300 frames.
uint64_t s_drawUs = 0;
uint32_t s_drawCalls = 0, s_drawFrames = 0;

struct DrawTimer
{
    uint32_t start;
    DrawTimer() : start(pf::perf::enabled() ? pf::perf::now() : 0) {}
    ~DrawTimer()
    {
        if (!pf::perf::enabled())
            return;
        s_drawUs += pf::perf::now() - start;
        ++s_drawCalls;
    }
};

void drawFrameDone()
{
    if (!pf::perf::enabled() || ++s_drawFrames < 300)
        return;
    pf::logInfo("d3d9: draws on the game thread: %u us/frame, %u draws/frame, %.2f us/draw",
                uint32_t(s_drawUs / s_drawFrames), s_drawCalls / s_drawFrames,
                s_drawCalls ? double(s_drawUs) / double(s_drawCalls) : 0.0);
    s_drawUs = 0;
    s_drawCalls = s_drawFrames = 0;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

const render::TextureStorage* storageOf(const Object* object)
{
    if (!object || (object->kind() != KindTexture && object->kind() != KindCubeTexture))
        return nullptr;
    return static_cast<const Texture*>(object)->storage();
}

// Inputs of the vertex shader by primitive count.
uint32_t vertexCount(DWORD type, UINT primitives)
{
    switch (type)
    {
    case D3DPT_TRIANGLELIST:  return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return primitives + 2;
    default:                  return 0;
    }
}

uint8_t primitive(DWORD type)
{
    return type == D3DPT_TRIANGLESTRIP ? render::PrimitiveStrip
         : type == D3DPT_TRIANGLEFAN ? render::PrimitiveFan : render::PrimitiveTriangles;
}

}

// ---- Construction --------------------------------------------------------------------------------------------------

Device::Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags)
    :   Object(KindDevice)
    ,   m_app(app)
    ,   m_d3d(d3d)
    ,   m_params(params)
    ,   m_window(window)
    ,   m_flags(flags)
{
    d3d->addRef();
    memset(&m_state, 0, sizeof(m_state));
    // frame_rate=30 (default) shows each frame for two vblanks, 60 for one.
    m_vblanks = pf::Config::getInt("frame_rate", 30) >= 60 ? 1 : 2;
    resetBackBuffer(params);
    defaults();
}

Device::~Device()
{
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    hold(m_state.target, static_cast<Surface*>(nullptr));
    hold(m_state.depth, static_cast<Surface*>(nullptr));
    for (Object*& texture : m_state.textures)
        hold(texture, static_cast<Object*>(nullptr));
    for (VertexDeclaration* d : m_fvfDeclarations)
        delete d;
    delete m_backBuffer;
    delete m_autoDepth;
    m_d3d->release();
}

template< typename T >
void Device::hold(T*& slot, T* object)
{
    if (slot == object)
        return;
    if (object)
        object->addRef();
    if (slot)
        slot->release();
    slot = object;
}

void Device::resetBackBuffer(const D3DPRESENT_PARAMETERS& params)
{
    m_params = params;
    if (!m_params.BackBufferWidth || !m_params.BackBufferHeight)
    {
        m_params.BackBufferWidth = pf::gxm::DisplayWidth;
        m_params.BackBufferHeight = pf::gxm::DisplayHeight;
    }
    if (m_state.target == m_backBuffer)
        hold(m_state.target, static_cast<Surface*>(nullptr));
    if (m_state.depth == m_autoDepth)
        hold(m_state.depth, static_cast<Surface*>(nullptr));
    delete m_backBuffer;
    delete m_autoDepth;
    m_autoDepth = nullptr;
    const uint32_t w = m_params.BackBufferWidth, h = m_params.BackBufferHeight;
    m_backBuffer = new Surface(this, Surface::RoleBackBuffer, w, h, D3DFMT_A8R8G8B8);
    publish(m_app, m_backBuffer);
    if (m_params.EnableAutoDepthStencil)
    {
        m_autoDepth = new Surface(this, Surface::RoleDepth, w, h, m_params.AutoDepthStencilFormat);
        m_autoDepth->setDepth(gpu().createDepth(w, h));
        publish(m_app, m_autoDepth);
    }
    pf::logInfo("d3d9: back buffer %ux%u%s", w, h, m_autoDepth ? " with depth" : "");
}

void Device::defaults()
{
    DWORD* rs = m_state.renderStates;
    memset(rs, 0, sizeof(m_state.renderStates));
    rs[D3DRS_ZENABLE] = m_autoDepth ? 1 : 0;
    rs[8] = 3;                      // FILLMODE solid
    rs[9] = 2;                      // SHADEMODE gouraud
    rs[D3DRS_ZWRITEENABLE] = 1;
    rs[16] = 1;                     // LASTPIXEL
    rs[D3DRS_SRCBLEND] = 2;         // ONE
    rs[D3DRS_DESTBLEND] = 1;        // ZERO
    rs[D3DRS_CULLMODE] = 3;         // CCW
    rs[D3DRS_ZFUNC] = 4;            // LESSEQUAL
    rs[D3DRS_ALPHAFUNC] = 8;        // ALWAYS
    rs[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
    rs[D3DRS_LIGHTING] = 1;
    rs[D3DRS_COLORWRITEENABLE] = 0xf;
    rs[D3DRS_BLENDOP] = 1;
    rs[D3DRS_BLENDFACTOR] = 0xffffffffu;
    for (DWORD s = 0; s < MaxStages; ++s)
    {
        DWORD* tss = m_state.stageStates[s];
        memset(tss, 0, sizeof(m_state.stageStates[s]));
        tss[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[D3DTSS_TEXCOORDINDEX] = s;
    }
    for (DWORD s = 0; s < MaxSamplerStages; ++s)
    {
        DWORD* ss = m_state.samplerStates[s];
        memset(ss, 0, sizeof(m_state.samplerStates[s]));
        ss[D3DSAMP_ADDRESSU] = ss[D3DSAMP_ADDRESSV] = ss[3] = 1;   // WRAP
        ss[D3DSAMP_MAGFILTER] = ss[D3DSAMP_MINFILTER] = 1;         // POINT
        ss[10] = 1;                                                 // MAXANISOTROPY
    }
    for (D3DMATRIX& m : m_state.transforms)
        m = s_identity;
    hold(m_state.target, m_backBuffer);
    hold(m_state.depth, m_autoDepth);
    m_state.viewport = D3DVIEWPORT9{ 0, 0, m_params.BackBufferWidth, m_params.BackBufferHeight, 0.0f, 1.0f };
    m_sceneOpen = false;
    m_stateDirty = true;
}

// ---- Recording -----------------------------------------------------------------------------------------------------

render::Frame* Device::frame(WinApplication* app, x86::CPU& cpu)
{
    if (!m_frame)
    {
        app->unlockContext(cpu);
        m_frame = gpu().beginFrame(m_params.BackBufferWidth, m_params.BackBufferHeight);
        app->lockContext(cpu);
        m_sceneOpen = false;
        m_frameFull = false;
        m_stream = &frameStream(m_frame->index);
    }
    return m_frame;
}

// Hands the recorded frame to the device.
void Device::submit()
{
    m_frame->resolve = &resolveFrame;
    m_frame->commands = m_stream->data;
    m_frame->commandCount = m_stream->size;
    gpu().submitFrame(m_frame, m_vblanks);
    m_frame = nullptr;
    m_stream = nullptr;
    Texture::trimKept();
}

void Device::record(uint32_t header, DWORD value)
{
    if (!m_stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_stream->append(2);
    w[0] = header;
    w[1] = value;
}

void Device::recordWords(uint32_t header, const void* payload, uint32_t words)
{
    if (!m_stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_stream->append(1 + words);
    w[0] = header;
    memcpy(w + 1, payload, words * 4);
}

render::Scene* Device::scene()
{
    render::Frame& f = *m_frame;
    render::Target target = {};
    uint16_t width = uint16_t(m_params.BackBufferWidth), height = uint16_t(m_params.BackBufferHeight);
    Surface* t = m_state.target;
    if (t && t->role() == Surface::RoleLevel && t->container())
    {
        target.color = t->container()->storage();
        target.face = uint8_t(t->face());
        width = uint16_t(t->width());
        height = uint16_t(t->height());
    }
    if (m_state.depth && m_state.depth->depth())
        target.depth = m_state.depth->depth();
    if (m_sceneOpen && f.sceneCount)
    {
        render::Scene& last = f.scenes[f.sceneCount - 1];
        if (memcmp(&last.target, &target, sizeof(target)) == 0)
            return &last;
    }
    if (f.sceneCount == f.sceneCapacity)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: scene limit reached, draws dropped", f.index);
        m_frameFull = true;
        return nullptr;
    }
    render::Scene& s = f.scenes[f.sceneCount++];
    memset(&s, 0, sizeof(s));
    s.target = target;
    s.width = width;
    s.height = height;
    s.firstDraw = f.drawCount;
    if (target.color)
        const_cast<render::TextureStorage*>(target.color)->lastUse = f.index;
    if (target.depth)
        const_cast<render::DepthStorage*>(target.depth)->lastUse = f.index;
    m_sceneOpen = true;
    return &s;
}

uint16_t Device::viewportIndex(render::Frame& f, const render::Viewport& viewport)
{
    if (f.viewportCount && memcmp(&f.viewports[f.viewportCount - 1], &viewport, sizeof(viewport)) == 0)
        return uint16_t(f.viewportCount - 1);
    if (f.viewportCount == f.viewportCapacity)
        return uint16_t(f.viewportCount - 1);
    f.viewports[f.viewportCount] = viewport;
    return uint16_t(f.viewportCount++);
}

// The state resolution of a draw happens on the render thread (d3d9_record.cpp); the game thread records the
// geometry, the textures as they are now (marking them read by this frame) and the active effect pass.
HRESULT Device::draw(WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices,
                     const uint16_t* indices, uint32_t count, uint8_t layout, uint32_t texComponents, bool positionT,
                     render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage)
{
    const uint32_t inputs = vertexCount(type, primitives);
    if (!inputs)
    {
        if (type < D3DPT_TRIANGLELIST || type > D3DPT_TRIANGLEFAN)
            D3D_LOG_ONCE("d3d9: primitive type %u not drawn", type);
        return type >= 1 && type <= 6 ? D3D_OK : D3DERR_INVALIDCALL;
    }
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene();
    if (!s)
        return D3D_OK;
    if (f->drawCount == f->drawCapacity)
    {
        pf::logError("d3d9: frame %u: draw limit reached, draws dropped", f->index);
        m_frameFull = true;
        return D3D_OK;
    }
    if (m_stateDirty)
    {
        recordWords(CmdFixedState, static_cast<const FixedState*>(&m_state), sizeof(FixedState) / 4);
        m_stateDirty = false;
    }
    syncEffectValues(*m_stream);

    render::DrawCall& d = f->draws[f->drawCount];
    d.vertices = vertices;
    d.indices = indices;
    d.count = inputs;
    d.vertexCount = count;
    const D3DVIEWPORT9& vp = m_state.viewport;
    d.viewport = viewportIndex(*f, render::Viewport{ float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height),
                                                     vp.MinZ, vp.MaxZ });

    uint32_t* w = m_stream->append(1 + sizeof(DrawRecord) / 4);
    w[0] = CmdDraw;
    DrawRecord& r = *reinterpret_cast<DrawRecord*>(w + 1);
    r.draw = f->drawCount;
    r.primitive = primitive(type);
    r.layout = layout;
    r.texComponents = uint8_t(texComponents);
    r.positionT = positionT ? 1 : 0;
    if (m_effectPass)
    {
        m_effectPass->record(r.effect, r.textures);
    }
    else
    {
        r.effect = EffectDraw{ NoBlock, NoBlock, NoEffect, 0, { 0, 0 } };
        r.textures[0] = storageOf(m_state.textures[0]);
        r.textures[1] = r.textures[2] = r.textures[3] = nullptr;
    }
    for (const render::TextureStorage* t : r.textures)
    {
        if (!t)
            continue;
        const_cast<render::TextureStorage*>(t)->lastUse = f->index;
        if (t->uploads.load(std::memory_order_relaxed))
            f->uploads = true;
    }
    if (vertexStorage)
        vertexStorage->lastUse = f->index;
    if (indexStorage)
        indexStorage->lastUse = f->index;
    ++f->drawCount;
    s->drawCount = f->drawCount - s->firstDraw;
    return D3D_OK;
}

VertexDeclaration* Device::fvfDeclaration(DWORD fvf)
{
    for (VertexDeclaration* d : m_fvfDeclarations)
    {
        if (d->fvf() == fvf)
            return d;
    }
    VertexDeclaration* d = new VertexDeclaration(this, nullptr, 0, fvf);
    m_fvfDeclarations.push_back(d);
    return d;
}

// ---- Methods -------------------------------------------------------------------------------------------------------

HRESULT Device::TestCooperativeLevel(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

UINT Device::GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 96 * 1024 * 1024;
}

HRESULT Device::EvictManagedResources(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d)
{
    RC_USE(app);
    RC_USE(cpu);
    m_d3d->addRef();
    *d3d = m_d3d->guest();
    return D3D_OK;
}

HRESULT Device::GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!caps)
        return D3DERR_INVALIDCALL;
    Direct3D::fillCaps(caps);
    return D3D_OK;
}

HRESULT Device::GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (!mode)
        return D3DERR_INVALIDCALL;
    *mode = D3DDISPLAYMODE{ m_params.BackBufferWidth, m_params.BackBufferHeight, 60, D3DFMT_X8R8G8B8 };
    return D3D_OK;
}

HRESULT Device::GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!params)
        return D3DERR_INVALIDCALL;
    *params = D3DDEVICE_CREATION_PARAMETERS{ 0, 1, m_window, m_flags };
    return D3D_OK;
}

HRESULT Device::SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(surface);
    return D3D_OK;
}

void Device::SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(flags);
}

BOOL Device::ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(show);
    return 0;
}

UINT Device::GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

HRESULT Device::Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    if (m_frame)
        submit();
    RC_USE(app);
    RC_USE(cpu);
    resetBackBuffer(*params);
    for (Object*& texture : m_state.textures)
        hold(texture, static_cast<Object*>(nullptr));
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    defaults();
    return D3D_OK;
}

HRESULT Device::Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                        x86::reg32 dirtyRegion)
{
    RC_USE(window);
    RC_USE(dirtyRegion);
    if (source || dest)
        D3D_LOG_ONCE("d3d9: Present rectangles ignored");
    // A frame without draws still shows (black), so the pacing stays the same.
    frame(app, cpu);
    submit();
    m_sceneOpen = false;
    drawFrameDone();
    return D3D_OK;
}

HRESULT Device::GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                              x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    if (swapChain != 0 || index != 0 || !surface)
        return D3DERR_INVALIDCALL;
    m_backBuffer->addRef();
    *surface = m_backBuffer->guest();
    return D3D_OK;
}

HRESULT Device::GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (status)
    {
        status[0] = 0;
        status[1] = 0;
    }
    return D3D_OK;
}

void Device::SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(enable);
}

void Device::SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    RC_USE(flags);
    RC_USE(ramp);
    D3D_LOG_ONCE("d3d9: gamma ramps are ignored");
}

void Device::GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (!ramp)
        return;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            ramp[c * 256 + i] = WORD(i * 257);
}

namespace
{

// Direct3D format to texture format; FormatNone when not supported.
texture::Format textureFormat(DWORD format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8: return texture::FormatARGB8888;
    case D3DFMT_X8R8G8B8:
        D3D_LOG_ONCE("d3d9: X8R8G8B8 textures are kept as A8R8G8B8");
        return texture::FormatARGB8888;
    case D3DFMT_DXT1:     return texture::FormatBC1;
    case D3DFMT_DXT3:     return texture::FormatBC2;
    default:              return texture::FormatNone;
    }
}

uint32_t fullChain(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while ((width >> levels) || (height >> levels))
        ++levels;
    return levels;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

}

HRESULT Device::CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                              DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    const texture::Format f = textureFormat(format);
    const bool target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    if (!texture || !width || !height || f == texture::FormatNone)
    {
        pf::logError("d3d9: CreateTexture %ux%u format 0x%x usage 0x%x not supported", width, height, format, usage);
        return D3DERR_INVALIDCALL;
    }
    if (!target && (!powerOfTwo(width) || !powerOfTwo(height)))
    {
        pf::logError("d3d9: CreateTexture %ux%u: textures must be powers of two", width, height);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = fullChain(width, height);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(target ? 1 : (levels == 0 || levels > chain ? chain : levels));
    desc.cube = false;
    desc.target = target;
    desc.width = uint16_t(width);
    desc.height = uint16_t(height);
    Texture* t = new Texture(this, desc, format, usage, pool);
    if (!t->valid())
    {
        pf::logError("d3d9: out of texture memory for %ux%u format 0x%x", width, height, format);
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage,
                                  DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    const texture::Format f = textureFormat(format);
    const bool target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    if (!texture || !powerOfTwo(edge) || f == texture::FormatNone)
    {
        pf::logError("d3d9: CreateCubeTexture %u format 0x%x not supported", edge, format);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = fullChain(edge, edge);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(target ? 1 : (levels == 0 || levels > chain ? chain : levels));
    if (target && levels > 1)
        D3D_LOG_ONCE("d3d9: render target cube texture with %u levels: level 0 only", levels);
    desc.cube = true;
    desc.target = target;
    desc.width = desc.height = uint16_t(edge);
    Texture* t = new Texture(this, desc, format, usage, pool);
    if (!t->valid())
    {
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf,
                                   DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
        return D3DERR_INVALIDCALL;
    Buffer* b = new Buffer(this, KindVertexBuffer, length, usage, pool, fvf);
    if (!b->valid())
    {
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format,
                                  DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
        return D3DERR_INVALIDCALL;
    if (format != D3DFMT_INDEX16)
    {
        pf::logError("d3d9: index format %u not supported", format);
        return D3DERR_INVALIDCALL;
    }
    Buffer* b = new Buffer(this, KindIndexBuffer, length, usage, pool, format);
    if (!b->valid())
    {
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                   DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                                   x86::reg32 sharedHandle)
{
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(lockable);
    if (!surface)
        return D3DERR_INVALIDCALL;
    // A render target texture whose only reference is its surface.
    x86::reg32 texture = 0;
    const HRESULT result = CreateTexture(app, cpu, width, height, 1, D3DUSAGE_RENDERTARGET, format, 0, &texture,
                                         sharedHandle);
    if (result != D3D_OK)
        return result;
    Texture* t = static_cast<Texture*>(lookup(texture));
    *surface = t->surface(app, 0, 0)->guest();
    t->release();
    return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                          DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                          x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(discard);
    RC_USE(sharedHandle);
    if (!surface || !width || !height)
        return D3DERR_INVALIDCALL;
    render::DepthStorage* depth = gpu().createDepth(width, height);
    if (!depth)
        return D3DERR_OUTOFVIDEOMEMORY;
    Surface* s = new Surface(this, Surface::RoleDepth, width, height, format);
    s->setDepth(depth);
    *surface = publish(app, s);
    return D3D_OK;
}

HRESULT Device::SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index != 0)
    {
        D3D_LOG_ONCE("d3d9: render target %u ignored", index);
        return index < 4 && !surface ? D3D_OK : D3DERR_INVALIDCALL;
    }
    if (!surface || surface->role() == Surface::RoleDepth
        || (surface->role() == Surface::RoleLevel && (!surface->container() || !surface->container()->desc().target)))
        return D3DERR_INVALIDCALL;
    hold(m_state.target, surface);
    // A new render target resets the viewport to the whole target.
    m_state.viewport = D3DVIEWPORT9{ 0, 0, surface->width(), surface->height(), 0.0f, 1.0f };
    recordWords(CmdViewport, &m_state.viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (index != 0 || !m_state.target)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.target->addRef();
    *surface = m_state.target->guest();
    return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (surface && surface->role() != Surface::RoleDepth)
        return D3DERR_INVALIDCALL;
    hold(m_state.depth, surface);
    return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (!m_state.depth)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.depth->addRef();
    *surface = m_state.depth->guest();
    return D3D_OK;
}

HRESULT Device::BeginScene(WinApplication* app, x86::CPU& cpu)
{
    frame(app, cpu);
    return D3D_OK;
}

HRESULT Device::EndScene(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags,
                      DWORD argb, float z, DWORD stencil)
{
    RC_USE(stencil);
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene();
    if (!s)
        return D3D_OK;
    if (!s->target.depth)
        flags &= ~D3DCLEAR_ZBUFFER;
    const D3DVIEWPORT9& vp = m_state.viewport;
    const LONG vx0 = LONG(vp.X), vy0 = LONG(vp.Y);
    const LONG vx1 = std::min<LONG>(vx0 + LONG(vp.Width), s->width), vy1 = std::min<LONG>(vy0 + LONG(vp.Height), s->height);
    const uint32_t rectCount = rects && count ? count : 1;
    for (uint32_t i = 0; i < rectCount; ++i)
    {
        LONG x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
        if (rects && count)
        {
            x0 = std::max(x0, rects[i].x1);
            y0 = std::max(y0, rects[i].y1);
            x1 = std::min(x1, rects[i].x2);
            y1 = std::min(y1, rects[i].y2);
        }
        if (x0 >= x1 || y0 >= y1)
            continue;
        DWORD what = flags;
        // A whole depth clear before the scene's first draw is the scene's starting depth.
        if ((what & D3DCLEAR_ZBUFFER) && s->drawCount == 0 && x0 == 0 && y0 == 0 && x1 == s->width && y1 == s->height)
        {
            s->depthCleared = true;
            s->clearDepth = z;
            what &= ~D3DCLEAR_ZBUFFER;
        }
        if (!(what & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER)))
            continue;
        if (f->drawCount == f->drawCapacity)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        render::DrawCall& d = f->draws[f->drawCount];
        memset(&d, 0, sizeof(d));
        render::VertexLayout layout;
        memset(&layout, 0, sizeof(layout));
        layout.stride = 12;
        layout.count = 1;
        layout.elements[0] = render::VertexElement{ render::UsagePosition, render::TypeFloat3, 0 };
        d.state.layout = render::registerLayout(layout);
        d.state.vertexProgram = render::VpClear;
        d.state.fragmentProgram = render::FpClear;
        d.state.primitive = render::PrimitiveFan;
        d.state.colorMask = (what & D3DCLEAR_TARGET) ? 0xf : 0;
        d.state.depthEnable = d.state.depthWrite = (what & D3DCLEAR_ZBUFFER) ? 1 : 0;
        d.state.depthFunc = 8;
        d.state.cullMode = 1;
        float* v = static_cast<float*>(render::allocateVertices(*f, 48));
        float* c = render::allocateConstants(*f, sizeof(render::ClearFragmentConstants) / 4, &d.fragmentConstants);
        if (!v || !c)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        // Pixel edges (centres at integers) in clip space of the whole target.
        const float sx = 2.0f / s->width, sy = 2.0f / s->height;
        const float l = (float(x0) - 0.5f) * sx - 1.0f, r = (float(x1) - 0.5f) * sx - 1.0f;
        const float t = 1.0f - (float(y0) - 0.5f) * sy, b = 1.0f - (float(y1) - 0.5f) * sy;
        const float quad[12] = { l, t, z, r, t, z, r, b, z, l, b, z };
        memcpy(v, quad, sizeof(quad));
        memset(c, 0, sizeof(render::ClearFragmentConstants));
        color(argb, reinterpret_cast<render::ClearFragmentConstants*>(c)->color);
        d.vertices = v;
        d.count = 4;
        d.vertexCount = 4;
        d.vertexConstants = render::NoConstants;
        d.viewport = viewportIndex(*f, render::Viewport{ 0, 0, float(s->width), float(s->height), 0.0f, 1.0f });
        ++f->drawCount;
        s->drawCount = f->drawCount - s->firstDraw;
    }
    return D3D_OK;
}

HRESULT Device::SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    uint32_t t;
    switch (state)
    {
    case D3DTS_WORLD:       t = TransformWorld; break;
    case D3DTS_VIEW:        t = TransformView; break;
    case D3DTS_PROJECTION:  t = TransformProjection; break;
    case D3DTS_TEXTURE0:    t = TransformTexture0; break;
    default:                D3D_LOG_ONCE("d3d9: transform %u ignored", state); return D3D_OK;
    }
    if (memcmp(&m_state.transforms[t], matrix, sizeof(D3DMATRIX)) == 0)
        return D3D_OK;
    m_state.transforms[t] = *matrix;
    recordWords(CmdTransform | t << 8, matrix, 16);
    return D3D_OK;
}

HRESULT Device::GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    switch (state)
    {
    case D3DTS_WORLD:       *matrix = m_state.transforms[TransformWorld]; break;
    case D3DTS_VIEW:        *matrix = m_state.transforms[TransformView]; break;
    case D3DTS_PROJECTION:  *matrix = m_state.transforms[TransformProjection]; break;
    case D3DTS_TEXTURE0:    *matrix = m_state.transforms[TransformTexture0]; break;
    default:                *matrix = s_identity; break;
    }
    return D3D_OK;
}

HRESULT Device::MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    if (!matrix)
        return D3DERR_INVALIDCALL;
    D3DMATRIX current;
    GetTransform(app, cpu, state, &current);
    D3DMATRIX product;
    multiply(matrix->m, current.m, product.m);
    return SetTransform(app, cpu, state, &product);
}

HRESULT Device::SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    if (memcmp(&m_state.viewport, viewport, sizeof(D3DVIEWPORT9)) == 0)
        return D3D_OK;
    m_state.viewport = *viewport;
    recordWords(CmdViewport, viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = m_state.viewport;
    return D3D_OK;
}

HRESULT Device::SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(m_material, material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(material, m_material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(m_lights[index], light, sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(light, m_lights[index], sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8)
        return D3DERR_INVALIDCALL;
    m_lightEnabled[index] = enable;
    return D3D_OK;
}

HRESULT Device::GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8 || !enable)
        return D3DERR_INVALIDCALL;
    *enable = m_lightEnabled[index] ? 128 : 0;
    return D3D_OK;
}

HRESULT Device::SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(index);
    RC_USE(plane);
    D3D_LOG_ONCE("d3d9: clip planes are ignored");
    return D3D_OK;
}

HRESULT Device::SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (state >= MaxRenderStates)
        return D3DERR_INVALIDCALL;
    if (m_state.renderStates[state] == value)
        return D3D_OK;
    m_state.renderStates[state] = value;
    record(CmdRenderState | state << 8, value);
    return D3D_OK;
}

HRESULT Device::GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (state >= MaxRenderStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.renderStates[state];
    return D3D_OK;
}

HRESULT Device::GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxSamplerStages || !texture)
        return D3DERR_INVALIDCALL;
    Object* t = m_state.textures[stage];
    if (t)
        t->addRef();
    *texture = t ? t->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxSamplerStages)
    {
        D3D_LOG_ONCE("d3d9: texture stage %u ignored", stage);
        return D3D_OK;
    }
    if (texture && texture->kind() != KindTexture && texture->kind() != KindCubeTexture)
        return D3DERR_INVALIDCALL;
    hold(m_state.textures[stage], texture);
    return D3D_OK;
}

HRESULT Device::GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.stageStates[stage][type];
    return D3D_OK;
}

HRESULT Device::SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT)
        return D3DERR_INVALIDCALL;
    if (m_state.stageStates[stage][type] == value)
        return D3D_OK;
    m_state.stageStates[stage][type] = value;
    record(CmdStageState | stage << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.samplerStates[sampler][type];
    return D3D_OK;
}

HRESULT Device::SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates)
        return D3D_OK;
    if (m_state.samplerStates[sampler][type] == value)
        return D3D_OK;
    m_state.samplerStates[sampler][type] = value;
    record(CmdSamplerState | sampler << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes)
{
    RC_USE(app);
    RC_USE(cpu);
    if (passes)
        *passes = 1;
    return D3D_OK;
}

HRESULT Device::SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rect);
    D3D_LOG_ONCE("d3d9: scissor rectangles are ignored");
    return D3D_OK;
}

HRESULT Device::SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(software);
    return D3D_OK;
}

BOOL Device::GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

HRESULT Device::SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(segments);
    return D3D_OK;
}

HRESULT Device::DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    const uint32_t vertices = vertexCount(type, count);
    const uint8_t* data = static_cast<const uint8_t*>(vb->storage()->data) + m_state.streamOffset
                        + size_t(start) * m_state.streamStride;
    return draw(app, cpu, type, count, data, nullptr, vertices, decl->layout(m_state.streamStride),
                decl->texComponents(), decl->positionT(), vb->storage(), nullptr);
}

HRESULT Device::DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                     UINT vertices, UINT startIndex, UINT count)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    Buffer* ib = m_state.indices;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !ib || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    const uint8_t* data = static_cast<const uint8_t*>(vb->storage()->data) + m_state.streamOffset
                        + ptrdiff_t(baseVertex) * ptrdiff_t(m_state.streamStride);
    const uint16_t* indices = static_cast<const uint16_t*>(ib->storage()->data) + startIndex;
    return draw(app, cpu, type, count, data, indices, minIndex + vertices, decl->layout(m_state.streamStride),
                decl->texComponents(), decl->positionT(), vb->storage(), ib->storage());
}

HRESULT Device::DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                                UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    // A user-pointer draw unbinds stream 0.
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !stride)
        return D3DERR_INVALIDCALL;
    const uint32_t vertices = vertexCount(type, count);
    render::Frame* f = frame(app, cpu);
    void* copy = render::allocateVertices(*f, vertices * stride);
    if (!copy)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: vertex arena full, draws dropped", f->index);
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(copy, data, size_t(vertices) * stride);
    return draw(app, cpu, type, count, copy, nullptr, vertices, decl->layout(stride), decl->texComponents(),
                decl->positionT(), nullptr, nullptr);
}

HRESULT Device::DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                       UINT count, const void* indices, DWORD indexFormat, const void* data,
                                       UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !indices || !stride || indexFormat != D3DFMT_INDEX16)
        return D3DERR_INVALIDCALL;
    const uint32_t inputs = vertexCount(type, count);
    const uint32_t reach = minIndex + vertices;
    render::Frame* f = frame(app, cpu);
    void* vcopy = render::allocateVertices(*f, reach * stride);
    void* icopy = render::allocateVertices(*f, inputs * 2);
    if (!vcopy || !icopy)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(vcopy, data, size_t(reach) * stride);
    memcpy(icopy, indices, size_t(inputs) * 2);
    return draw(app, cpu, type, count, vcopy, static_cast<const uint16_t*>(icopy), reach, decl->layout(stride),
                decl->texComponents(), decl->positionT(), nullptr, nullptr);
}

HRESULT Device::CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                        x86::reg32* declaration)
{
    RC_USE(cpu);
    if (!elements || !declaration)
        return D3DERR_INVALIDCALL;
    uint32_t count = 0;
    while (count < 64 && elements[count].Stream != 0xFF)
        ++count;
    *declaration = publish(app, new VertexDeclaration(this, elements, count, 0));
    return D3D_OK;
}

HRESULT Device::SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration)
{
    RC_USE(app);
    RC_USE(cpu);
    hold(m_state.declaration, declaration);
    m_state.fvf = 0;
    return D3D_OK;
}

HRESULT Device::GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!declaration)
        return D3DERR_INVALIDCALL;
    if (m_state.declaration)
        m_state.declaration->addRef();
    *declaration = m_state.declaration ? m_state.declaration->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf)
{
    RC_USE(app);
    RC_USE(cpu);
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    m_state.fvf = fvf;
    return D3D_OK;
}

HRESULT Device::GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = m_state.fvf;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
    {
        pf::logError("d3d9: vertex shader objects are not supported (effects are native)");
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

HRESULT Device::GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
        *shader = 0;
    return D3D_OK;
}

HRESULT Device::SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                                UINT stride)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream != 0)
    {
        D3D_LOG_ONCE("d3d9: stream %u ignored", stream);
        return D3D_OK;
    }
    if (buffer && buffer->kind() != KindVertexBuffer)
        return D3DERR_INVALIDCALL;
    hold(m_state.stream, buffer);
    m_state.streamOffset = offset;
    m_state.streamStride = stride;
    return D3D_OK;
}

HRESULT Device::GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                                UINT* stride)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream != 0 || !buffer || !offset || !stride)
        return D3DERR_INVALIDCALL;
    if (m_state.stream)
        m_state.stream->addRef();
    *buffer = m_state.stream ? m_state.stream->guest() : 0;
    *offset = m_state.streamOffset;
    *stride = m_state.streamStride;
    return D3D_OK;
}

HRESULT Device::SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(stream);
    if (divider != 1)
        D3D_LOG_ONCE("d3d9: stream frequency %u ignored", divider);
    return D3D_OK;
}

HRESULT Device::SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (buffer && buffer->kind() != KindIndexBuffer)
        return D3DERR_INVALIDCALL;
    hold(m_state.indices, buffer);
    return D3D_OK;
}

HRESULT Device::GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!buffer)
        return D3DERR_INVALIDCALL;
    if (m_state.indices)
        m_state.indices->addRef();
    *buffer = m_state.indices ? m_state.indices->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
    {
        pf::logError("d3d9: pixel shader objects are not supported (effects are native)");
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

HRESULT Device::GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
        *shader = 0;
    return D3D_OK;
}

}}
