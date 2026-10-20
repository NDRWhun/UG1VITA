// Direct3D resources: textures (2D, cube, render targets), surfaces, vertex and index buffers, vertex
// declarations. Guest locks go to guest memory; unlocks write the GPU copy (a new one when frames in flight
// still read the current one).

#include <winapi/d3d9_internal.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <algorithm>

namespace win32 { namespace d3d9
{

namespace
{

pf::gxm::Device& gpu()
{
    return pf::gxm::Device::get();
}

uint8_t* host(x86::reg32 address)
{
    return g_guestBase + address;
}

x86::reg32 guestOf(Object* object)
{
    return object ? object->guest() : 0;
}

}

// ---- Texture -------------------------------------------------------------------------------------------------------

Texture::Texture(Device* device, const render::TextureDesc& desc, DWORD format, DWORD usage, DWORD pool)
    :   Object(desc.cube ? KindCubeTexture : KindTexture)
    ,   m_device(device)
    ,   m_desc(desc)
    ,   m_format(format)
    ,   m_usage(usage)
    ,   m_pool(pool)
    ,   m_storage(gpu().createTexture(desc))
{
    device->addRef();
    const uint32_t count = (desc.cube ? 6 : 1) * desc.levels;
    m_locks.assign(count, nullptr);
    m_lockFlags.assign(count, 0);
    m_written.assign(count, false);
    m_surfaces.assign(count, nullptr);
}

Texture::~Texture()
{
    for (MemMap* lock : m_locks)
        delete lock;
    for (Surface* surface : m_surfaces)
        delete surface;
    if (m_storage)
        gpu().release(m_storage);
    m_device->release();
}

void Texture::fillDesc(UINT level, D3DSURFACE_DESC* desc) const
{
    desc->Format = m_format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_usage;
    desc->Pool = m_pool;
    desc->MultiSampleType = 0;
    desc->MultiSampleQuality = 0;
    desc->Width = texture::levelDim(m_desc.width, level);
    desc->Height = texture::levelDim(m_desc.height, level);
}

Surface* Texture::surface(WinApplication* app, uint32_t face, uint32_t level)
{
    const uint32_t index = face * m_desc.levels + level;
    Surface*& s = m_surfaces[index];
    if (!s)
    {
        s = new Surface(m_device, Surface::RoleLevel, texture::levelDim(m_desc.width, level),
                        texture::levelDim(m_desc.height, level), m_format);
        s->setContainer(this, face, level);
        publish(app, s);
    }
    s->addRef();
    return s;
}

HRESULT Texture::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Texture::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(priority);
    return 0;
}

DWORD Texture::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

void Texture::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Texture::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.cube ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE;
}

DWORD Texture::SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lod);
    return 0;
}

DWORD Texture::GetLOD(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

DWORD Texture::GetLevelCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.levels;
}

HRESULT Texture::GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DSURFACE_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (level >= m_desc.levels || !desc)
        return D3DERR_INVALIDCALL;
    fillDesc(level, desc);
    return D3D_OK;
}

HRESULT Texture::GetSurfaceLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* surface)
{
    RC_USE(cpu);
    if (level >= m_desc.levels || m_desc.cube || !surface)
        return D3DERR_INVALIDCALL;
    *surface = guestOf(this->surface(app, 0, level));
    return D3D_OK;
}

HRESULT Texture::GetCubeMapSurface(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, x86::reg32* surface)
{
    RC_USE(cpu);
    if (level >= m_desc.levels || !m_desc.cube || face >= 6 || !surface)
        return D3DERR_INVALIDCALL;
    *surface = guestOf(this->surface(app, face, level));
    return D3D_OK;
}

HRESULT Texture::lock(uint32_t face, uint32_t level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (!m_storage || m_desc.target || level >= m_desc.levels || !locked)
        return D3DERR_INVALIDCALL;
    const uint32_t index = face * m_desc.levels + level;
    if (m_locks[index])
        return D3DERR_INVALIDCALL;
    const texture::Format format = m_desc.format;
    const uint32_t bytes = texture::levelBytes(format, m_desc.width, m_desc.height, level);
    const uint32_t pitch = texture::levelPitch(format, m_desc.width, level);
    MemMap* memory = new MemMap(bytes);
    void* data = host(memory->getBlockStart());
    if (m_written[index] && !(flags & D3DLOCK_DISCARD))
        gpu().readTexture(m_storage, face, level, data, pitch);
    m_locks[index] = memory;
    m_lockFlags[index] = flags;
    x86::reg32 bits = memory->getBlockStart();
    if (rect)
    {
        const uint32_t unit = texture::isBlock(format) ? 4 : 1;
        bits += uint32_t(rect->top) / unit * pitch + uint32_t(rect->left) / unit * texture::unitBytes(format);
    }
    locked->Pitch = LONG(pitch);
    locked->pBits = bits;
    return D3D_OK;
}

HRESULT Texture::unlock(uint32_t face, uint32_t level)
{
    if (level >= m_desc.levels)
        return D3DERR_INVALIDCALL;
    const uint32_t index = face * m_desc.levels + level;
    MemMap* memory = m_locks[index];
    if (!memory)
        return D3DERR_INVALIDCALL;
    if (!(m_lockFlags[index] & D3DLOCK_READONLY))
    {
        if (gpu().busy(*m_storage))
        {
            // Frames in flight still read this copy: continue in a new one.
            render::TextureStorage* fresh = gpu().createTexture(m_desc);
            if (fresh)
            {
                gpu().copyTexture(fresh, m_storage);
                gpu().release(m_storage);
                m_storage = fresh;
            }
        }
        gpu().writeTexture(m_storage, face, level, host(memory->getBlockStart()),
                           texture::levelPitch(m_desc.format, m_desc.width, level));
        m_written[index] = true;
    }
    delete memory;
    m_locks[index] = nullptr;
    return D3D_OK;
}

HRESULT Texture::LockRect(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                          DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.cube ? D3DERR_INVALIDCALL : lock(0, level, locked, rect, flags);
}

HRESULT Texture::UnlockRect(WinApplication* app, x86::CPU& cpu, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.cube ? D3DERR_INVALIDCALL : unlock(0, level);
}

HRESULT Texture::LockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, D3DLOCKED_RECT* locked,
                              const RECT* rect, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    return face < 6 ? lock(face, level, locked, rect, flags) : D3DERR_INVALIDCALL;
}

HRESULT Texture::UnlockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    return face < 6 ? unlock(face, level) : D3DERR_INVALIDCALL;
}

HRESULT Texture::AddDirtyRect(WinApplication* app, x86::CPU& cpu, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rect);
    return D3D_OK;
}

HRESULT Texture::AddDirtyRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(face);
    RC_USE(rect);
    return D3D_OK;
}

// ---- Surface -------------------------------------------------------------------------------------------------------

Surface::Surface(Device* device, Role role, uint32_t width, uint32_t height, DWORD format)
    :   Object(KindSurface)
    ,   m_device(device)
    ,   m_role(role)
    ,   m_width(width)
    ,   m_height(height)
    ,   m_format(format)
{
}

Surface::~Surface()
{
    if (m_role == RoleDepth && m_depth)
        gpu().release(m_depth);
}

void Surface::setContainer(Texture* texture, uint32_t face, uint32_t level)
{
    m_container = texture;
    m_face = face;
    m_level = level;
}

ULONG Surface::addRef()
{
    return m_container ? m_container->addRef() : Object::addRef();
}

ULONG Surface::release()
{
    return m_container ? m_container->release() : Object::release();
}

HRESULT Surface::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Surface::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(priority);
    return 0;
}

DWORD Surface::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

void Surface::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Surface::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3DRTYPE_SURFACE;
}

HRESULT Surface::GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(iid);
    if (!container)
        return D3DERR_INVALIDCALL;
    Object* owner = m_container ? static_cast<Object*>(m_container) : static_cast<Object*>(m_device);
    owner->addRef();
    *container = owner->guest();
    return D3D_OK;
}

HRESULT Surface::GetDesc(WinApplication* app, x86::CPU& cpu, D3DSURFACE_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    if (m_container)
    {
        m_container->fillDesc(m_level, desc);
        return D3D_OK;
    }
    desc->Format = m_format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_role == RoleDepth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    desc->Pool = 0;
    desc->MultiSampleType = 0;
    desc->MultiSampleQuality = 0;
    desc->Width = m_width;
    desc->Height = m_height;
    return D3D_OK;
}

HRESULT Surface::LockRect(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (m_container && !m_container->desc().target)
    {
        return m_container->desc().cube
             ? m_container->LockRectCube(app, cpu, m_face, m_level, locked, rect, flags)
             : m_container->LockRect(app, cpu, m_level, locked, rect, flags);
    }
    pf::logError("d3d9: render target and depth surfaces cannot be locked");
    return D3DERR_INVALIDCALL;
}

HRESULT Surface::UnlockRect(WinApplication* app, x86::CPU& cpu)
{
    if (m_container && !m_container->desc().target)
    {
        return m_container->desc().cube ? m_container->UnlockRectCube(app, cpu, m_face, m_level)
                                         : m_container->UnlockRect(app, cpu, m_level);
    }
    return D3DERR_INVALIDCALL;
}

// ---- Buffer --------------------------------------------------------------------------------------------------------

Buffer::Buffer(Device* device, Kind kind, UINT size, DWORD usage, DWORD pool, DWORD format)
    :   Object(kind)
    ,   m_device(device)
    ,   m_size(size)
    ,   m_usage(usage)
    ,   m_pool(pool)
    ,   m_format(format)
    ,   m_shadow(new MemMap(size ? size : 1))
    ,   m_storage(gpu().createBuffer(size))
{
    device->addRef();
}

Buffer::~Buffer()
{
    delete m_shadow;
    if (m_storage)
        gpu().release(m_storage);
    m_device->release();
}

HRESULT Buffer::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Buffer::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(priority);
    return 0;
}

DWORD Buffer::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

void Buffer::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Buffer::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return kind() == KindVertexBuffer ? D3DRTYPE_VERTEXBUFFER : D3DRTYPE_INDEXBUFFER;
}

HRESULT Buffer::Lock(WinApplication* app, x86::CPU& cpu, UINT offset, UINT size, x86::reg32* data, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!m_storage || !data || offset > m_size)
        return D3DERR_INVALIDCALL;
    if (!size || offset + size > m_size)
        size = m_size - offset;
    if (m_locks++ == 0)
    {
        m_lockOffset = offset;
        m_lockSize = size;
        m_lockFlags = flags;
    }
    else
    {
        // Nested locks: one range over all of them.
        const UINT end = std::max(m_lockOffset + m_lockSize, offset + size);
        m_lockOffset = std::min(m_lockOffset, offset);
        m_lockSize = end - m_lockOffset;
        m_lockFlags &= flags;
    }
    *data = m_shadow->getBlockStart() + offset;
    return D3D_OK;
}

HRESULT Buffer::Unlock(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    if (m_locks == 0)
        return D3DERR_INVALIDCALL;
    if (--m_locks)
        return D3D_OK;
    if (m_lockFlags & D3DLOCK_READONLY)
        return D3D_OK;
    const uint8_t* shadow = host(m_shadow->getBlockStart());
    if (!(m_lockFlags & D3DLOCK_NOOVERWRITE) && gpu().busy(*m_storage))
    {
        // Frames in flight read the current copy: a new one with every byte the guest has written.
        render::BufferStorage* fresh = gpu().createBuffer(m_size);
        if (fresh)
        {
            memcpy(fresh->data, shadow, m_size);
            gpu().release(m_storage);
            m_storage = fresh;
            return D3D_OK;
        }
    }
    memcpy(static_cast<uint8_t*>(m_storage->data) + m_lockOffset, shadow + m_lockOffset, m_lockSize);
    return D3D_OK;
}

HRESULT Buffer::GetVertexDesc(WinApplication* app, x86::CPU& cpu, D3DVERTEXBUFFER_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    *desc = D3DVERTEXBUFFER_DESC{ 100 /* D3DFMT_VERTEXDATA */, D3DRTYPE_VERTEXBUFFER, m_usage, m_pool, m_size,
                                  m_format };
    return D3D_OK;
}

HRESULT Buffer::GetIndexDesc(WinApplication* app, x86::CPU& cpu, D3DINDEXBUFFER_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    *desc = D3DINDEXBUFFER_DESC{ m_format, D3DRTYPE_INDEXBUFFER, m_usage, m_pool, m_size };
    return D3D_OK;
}

// ---- Vertex declaration --------------------------------------------------------------------------------------------

namespace
{

enum : BYTE
{
    D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2, D3DDECLUSAGE_NORMAL = 3,
    D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_POSITIONT = 9, D3DDECLUSAGE_COLOR = 10,
    D3DDECLTYPE_UNUSED = 17
};

const uint8_t s_typeSize[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4 };

void add(render::VertexLayout& layout, uint8_t usage, uint8_t type, uint16_t offset)
{
    if (layout.count < render::MaxVertexElements)
        layout.elements[layout.count++] = render::VertexElement{ usage, type, offset };
}

}

VertexDeclaration::VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t count, DWORD fvf)
    :   Object(KindVertexDeclaration)
    ,   m_device(device)
    ,   m_elements(elements, elements + count)
    ,   m_fvf(fvf)
{
    memset(&m_layout, 0, sizeof(m_layout));
    if (fvf)
    {
        m_layout = fvfLayout(fvf, &m_texComponents);
        return;
    }
    for (const D3DVERTEXELEMENT9& e : m_elements)
    {
        if (e.Stream != 0)
        {
            D3D_LOG_ONCE("d3d9: vertex elements of stream %u ignored", e.Stream);
            continue;
        }
        if (e.Type > render::TypeUByte4N)
        {
            pf::logError("d3d9: vertex element type %u not supported", e.Type);
            continue;
        }
        uint8_t usage;
        switch (e.Usage)
        {
        case D3DDECLUSAGE_POSITIONT:
            m_layout.positionT = true;
            usage = render::UsagePosition;
            break;
        case D3DDECLUSAGE_POSITION:     usage = render::UsagePosition; break;
        case D3DDECLUSAGE_NORMAL:       usage = render::UsageNormal; break;
        case D3DDECLUSAGE_BLENDWEIGHT:  usage = render::UsageWeight; break;
        case D3DDECLUSAGE_BLENDINDICES: usage = render::UsageIndices; break;
        case D3DDECLUSAGE_COLOR:
            if (e.UsageIndex != 0)
                continue;
            usage = render::UsageColor;
            break;
        case D3DDECLUSAGE_TEXCOORD:
            if (e.UsageIndex > 3)
                continue;
            usage = uint8_t(render::UsageTex0 + e.UsageIndex);
            if (e.UsageIndex == 0)
                m_texComponents = e.Type <= render::TypeFloat4 ? e.Type + 1u : 4u;
            break;
        default:
            D3D_LOG_ONCE("d3d9: vertex element usage %u ignored", e.Usage);
            continue;
        }
        add(m_layout, usage, e.Type, e.Offset);
    }
}

render::VertexLayout VertexDeclaration::fvfLayout(DWORD fvf, uint32_t* texComponents)
{
    render::VertexLayout layout;
    memset(&layout, 0, sizeof(layout));
    uint16_t offset = 0;
    switch (fvf & 0xE)
    {
    case 0x2:   // XYZ
        add(layout, render::UsagePosition, render::TypeFloat3, offset);
        offset += 12;
        break;
    case 0x4:   // XYZRHW
        add(layout, render::UsagePosition, render::TypeFloat4, offset);
        layout.positionT = true;
        offset += 16;
        break;
    default:
        if (fvf & 0xE)
            pf::logError("d3d9: FVF position 0x%x not supported", fvf & 0xE);
        break;
    }
    if (fvf & 0x10)     // NORMAL
    {
        add(layout, render::UsageNormal, render::TypeFloat3, offset);
        offset += 12;
    }
    if (fvf & 0x20)     // PSIZE
        offset += 4;
    if (fvf & 0x40)     // DIFFUSE
    {
        add(layout, render::UsageColor, render::TypeColor, offset);
        offset += 4;
    }
    if (fvf & 0x80)     // SPECULAR
        offset += 4;
    const uint32_t texCount = (fvf >> 8) & 0xF;
    *texComponents = 0;
    for (uint32_t i = 0; i < texCount; ++i)
    {
        static const uint8_t components[4] = { 2, 3, 4, 1 };
        const uint32_t n = components[(fvf >> (16 + 2 * i)) & 3];
        if (i < 4)
            add(layout, uint8_t(render::UsageTex0 + i), uint8_t(render::TypeFloat1 + n - 1), offset);
        if (i == 0)
            *texComponents = n;
        offset += uint16_t(4 * n);
    }
    layout.stride = offset;
    return layout;
}

uint8_t VertexDeclaration::layout(uint32_t stride)
{
    if (stride != m_stride || !m_stride)
    {
        render::VertexLayout layout = m_layout;
        layout.stride = uint16_t(stride);
        m_layoutId = render::registerLayout(layout);
        m_stride = stride;
    }
    return m_layoutId;
}

HRESULT VertexDeclaration::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT VertexDeclaration::GetDeclaration(WinApplication* app, x86::CPU& cpu, D3DVERTEXELEMENT9* elements,
                                          UINT* count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!count)
        return D3DERR_INVALIDCALL;
    const UINT n = UINT(m_elements.size()) + 1;
    if (elements)
    {
        memcpy(elements, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
        elements[m_elements.size()] = D3DVERTEXELEMENT9{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 };
    }
    *count = n;
    return D3D_OK;
}

}}
