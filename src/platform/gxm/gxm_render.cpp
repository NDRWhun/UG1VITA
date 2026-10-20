// Direct3D frames on SceGxm: GPU heaps, resources, frame slots and scene drawing (gxm_render.h).

#include "gxm_render.h"
#include <platform/log.h>
#include <platform/texture.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define NFSU_SHADERS(X) \
    X(ff_v) X(ff_screen_v) X(world_v) X(worldheadlight_v) X(worldreflect_v) X(worldheadlightreflect_v) \
    X(worldbone_v) X(car_v) X(filter_v) X(clearz_v) \
    X(ff_color_f) X(ff_color_f_atest) X(ff_tex_f) X(ff_tex_f_atest) X(ff_proj_f) X(ff_proj_f_atest) \
    X(world_f) X(world_f_atest) X(worldheadlight_f) X(worldheadlight_f_atest) X(worldreflect_f) \
    X(worldreflect_f_atest) X(worldheadlightreflect_f) X(worldheadlightreflect_f_atest) X(car_f) X(car_f_atest) \
    X(filter1_f) X(filter1_f_atest) X(filter2_f) X(filter2_f_atest) X(filter3_f) X(filter3_f_atest) \
    X(filter4_f) X(filter4_f_atest) X(clear_f) \
    X(filter1_f_bfcolor) X(filter1_f_bfalpha) X(filter1_f_bfscale) X(filter2_f_bfcolor) X(filter2_f_bfalpha) \
    X(filter2_f_bfscale) X(filter3_f_bfcolor) X(filter3_f_bfalpha) X(filter3_f_bfscale) X(filter4_f_bfcolor) \
    X(filter4_f_bfalpha) X(filter4_f_bfscale)

extern "C" {
#define NFSU_DECLARE_SHADER(name) extern const unsigned char shader_##name[];
NFSU_SHADERS(NFSU_DECLARE_SHADER)
}

namespace pf { namespace gxm {

namespace
{

struct ShaderBinary
{
    const char*             name;
    const unsigned char*    data;
};

#define NFSU_SHADER_ENTRY(name) { #name, shader_##name },
const ShaderBinary s_binaries[] = { NFSU_SHADERS(NFSU_SHADER_ENTRY) };

const unsigned char* binary(const char* name)
{
    for (const ShaderBinary& b : s_binaries)
    {
        if (strcmp(b.name, name) == 0)
            return b.data;
    }
    return nullptr;
}

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

const uint32_t SlotVertexBytes = 4 * 1024 * 1024;
const uint32_t SlotDraws = 16384;
const uint32_t SlotConstants = 512 * 1024;      // floats
const uint32_t SlotScenes = 128;
const uint32_t SlotViewports = 512;
const uint32_t NotificationBase = 8;            // notification words 0..7 belong to the present path

SceGxmTextureFormat textureFormat(texture::Format format)
{
    switch (format)
    {
    case texture::FormatBC1: return SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR;
    case texture::FormatBC2: return SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR;
    default:                 return SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB;
    }
}

SceGxmBlendFactor blendFactor(uint8_t factor)
{
    switch (factor)
    {
    case 1:  return SCE_GXM_BLEND_FACTOR_ZERO;
    case 3:  return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case 4:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:  return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case 6:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:  return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case 8:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:  return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case 10: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return SCE_GXM_BLEND_FACTOR_ONE;       // ONE (the layer resolves BLENDFACTOR, INVBLENDFACTOR)
    }
}

// The alpha channel reads the alpha of a colour factor, as in Direct3D.
SceGxmBlendFactor alphaFactor(SceGxmBlendFactor factor)
{
    switch (factor)
    {
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:           return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:           return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:  return SCE_GXM_BLEND_FACTOR_ONE;
    default:                                       return factor;
    }
}

// Program variant: 0 plain, 1 alpha test, 2.. blend factor outputs (FrameRenderer::Variants).
uint32_t variant(const render::DrawState& state)
{
    return state.factorOutput ? 1u + state.factorOutput : state.alphaTest ? 1u : 0u;
}

SceGxmAttributeFormat attributeFormat(uint8_t type, uint8_t* components)
{
    switch (type)
    {
    case render::TypeFloat1:  *components = 1; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat3:  *components = 3; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeColor:
    case render::TypeUByte4N: *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    case render::TypeUByte4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8;
    case render::TypeShort2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    case render::TypeShort4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    default:                  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    }
}

SceGxmPrimitiveType primitiveType(uint8_t primitive)
{
    return primitive == render::PrimitiveStrip ? SCE_GXM_PRIMITIVE_TRIANGLE_STRIP
         : primitive == render::PrimitiveFan ? SCE_GXM_PRIMITIVE_TRIANGLE_FAN : SCE_GXM_PRIMITIVE_TRIANGLES;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

}

// ---- GpuHeap -------------------------------------------------------------------------------------------------------

GpuHeap::GpuHeap(const char* name, SceKernelMemBlockType type, uint32_t chunkSize, uint32_t attributes)
    :   m_name(name), m_type(type), m_chunkSize(chunkSize), m_attributes(attributes)
{
}

GpuHeap::~GpuHeap()
{
    for (Chunk* c : m_chunks)
    {
        sceGxmUnmapMemory(c->base);
        sceKernelFreeMemBlock(c->uid);
        delete c;
    }
}

bool GpuHeap::addChunk(uint32_t minimum)
{
    const uint32_t granularity = m_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
    const uint32_t size = alignUp(std::max(minimum, m_chunkSize), granularity);
    const SceUID uid = sceKernelAllocMemBlock(m_name, m_type, size, nullptr);
    if (uid < 0)
        return false;
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SceGxmMemoryAttribFlags(m_attributes)) < 0)
    {
        sceKernelFreeMemBlock(uid);
        return false;
    }
    Chunk* c = new Chunk{ uid, static_cast<uint8_t*>(base), size, {} };
    c->holes[0] = size;
    m_chunks.push_back(c);
    logInfo("gxm: %s heap +%u KB", m_name, size / 1024);
    return true;
}

void* GpuHeap::allocate(uint32_t size, uint32_t alignment)
{
    size = alignUp(size ? size : 1, 16);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (Chunk* c : m_chunks)
        {
            for (auto it = c->holes.begin(); it != c->holes.end(); ++it)
            {
                const uint32_t start = alignUp(it->first, alignment);
                if (start + size > it->first + it->second)
                    continue;
                const uint32_t holeStart = it->first, holeEnd = it->first + it->second;
                c->holes.erase(it);
                if (start > holeStart)
                    c->holes[holeStart] = start - holeStart;
                if (start + size < holeEnd)
                    c->holes[start + size] = holeEnd - (start + size);
                void* p = c->base + start;
                m_used[p] = std::make_pair(c, size);
                return p;
            }
        }
        if (attempt == 0 && !addChunk(size + alignment))
            return nullptr;
    }
    return nullptr;
}

void GpuHeap::free(void* pointer)
{
    auto it = m_used.find(pointer);
    if (it == m_used.end())
        return;
    Chunk* c = it->second.first;
    uint32_t start = uint32_t(static_cast<uint8_t*>(pointer) - c->base);
    uint32_t size = it->second.second;
    m_used.erase(it);
    auto next = c->holes.lower_bound(start);
    if (next != c->holes.end() && next->first == start + size)
    {
        size += next->second;
        next = c->holes.erase(next);
    }
    if (next != c->holes.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start)
        {
            start = previous->first;
            size += previous->second;
            c->holes.erase(previous);
        }
    }
    c->holes[start] = size;
}

// ---- Initialisation ------------------------------------------------------------------------------------------------

bool FrameRenderer::init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications)
{
    m_context = context;
    m_patcher = patcher;
    m_textureHeap = new GpuHeap("gxm_textures", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 16 * 1024 * 1024,
                                SCE_GXM_MEMORY_ATTRIB_RW);
    m_bufferHeap = new GpuHeap("gxm_buffers", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 8 * 1024 * 1024,
                               SCE_GXM_MEMORY_ATTRIB_READ);

    // Programs.
    auto registerProgram = [this](const char* name, SceGxmShaderPatcherId* id) -> const SceGxmProgram* {
        const unsigned char* data = binary(name);
        if (!data)
            fatal("shader %s is not linked", name);
        const SceGxmProgram* program = reinterpret_cast<const SceGxmProgram*>(data);
        if (sceGxmProgramCheck(program) < 0 || sceGxmShaderPatcherRegisterProgram(m_patcher, program, id) < 0)
            fatal("shader %s cannot be registered", name);
        return program;
    };
    for (int v = 0; v < render::VpCount; ++v)
    {
        const render::VertexProgramInfo& info = render::vertexProgram(render::VertexProgram(v));
        VertexProgram& p = m_vertexPrograms[v];
        p.program = registerProgram(info.name, &p.id);
        for (uint8_t i = 0; i < info.fieldCount; ++i)
            p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
    }
    for (int f = 0; f < render::FpCount; ++f)
    {
        const render::FragmentProgramInfo& info = render::fragmentProgram(render::FragmentProgram(f));
        for (int v = 0; v < Variants; ++v)
        {
            // Without the variant: the plain program (the layer never selects it).
            if ((v == 1 && f == render::FpClear) || (v >= 2 && !info.blendFactor))
            {
                m_fragmentPrograms[f][v] = m_fragmentPrograms[f][0];
                m_fragmentPrograms[f][v].owned = false;
                continue;
            }
            // world_f serves world and worldbone: register it once.
            if (f == render::FpWorldBone)
            {
                m_fragmentPrograms[f][v] = m_fragmentPrograms[render::FpWorld][v];
                m_fragmentPrograms[f][v].owned = false;
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "%s%s", info.name,
                     v == 1 ? "_atest" : render::factorSuffix(render::FactorOutput(v >= 2 ? v - 1 : 0)));
            FragmentProgram& p = m_fragmentPrograms[f][v];
            p.owned = true;
            p.program = registerProgram(name, &p.id);
            for (uint8_t i = 0; i < info.fieldCount; ++i)
                p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
            for (uint8_t s = 0; s < info.samplers; ++s)
            {
                char sampler[8] = "uTex0";
                sampler[4] = char('0' + s);
                const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program, sampler);
                p.units[s] = parameter ? sceGxmProgramParameterGetResourceIndex(parameter) : 0;
            }
        }
    }

    // 0..65535 for draws without indices.
    m_sequenceUid = sceKernelAllocMemBlock("gxm_sequence", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 128 * 1024, nullptr);
    if (m_sequenceUid < 0)
        return false;
    void* sequence = nullptr;
    sceKernelGetMemBlockBase(m_sequenceUid, &sequence);
    sceGxmMapMemory(sequence, 128 * 1024, SCE_GXM_MEMORY_ATTRIB_READ);
    m_sequence = static_cast<uint16_t*>(sequence);
    for (uint32_t i = 0; i < 65536; ++i)
        m_sequence[i] = uint16_t(i);

    // Frame slots: host arrays, a GPU-read vertex arena each.
    m_arenaUid = sceKernelAllocMemBlock("gxm_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                        Slots * SlotVertexBytes, nullptr);
    if (m_arenaUid < 0)
        return false;
    void* arena = nullptr;
    sceKernelGetMemBlockBase(m_arenaUid, &arena);
    sceGxmMapMemory(arena, Slots * SlotVertexBytes, SCE_GXM_MEMORY_ATTRIB_READ);
    for (uint32_t i = 0; i < Slots; ++i)
    {
        Slot& s = m_slots[i];
        s.scenes.resize(SlotScenes);
        s.draws.resize(SlotDraws);
        s.viewports.resize(SlotViewports);
        s.constants.resize(SlotConstants);
        s.vertices = static_cast<uint8_t*>(arena) + i * SlotVertexBytes;
        s.done.address = notifications + NotificationBase + i;
        *s.done.address = 0;
        s.done.value = 0;
    }

    render::TextureDesc black = { texture::FormatARGB8888, 1, false, false, 8, 8 };
    m_black = static_cast<Texture*>(createTexture(black));
    if (!m_black)
        return false;
    memset(m_black->data, 0, m_black->bytes);
    for (uint32_t i = 0; i < 64; ++i)
        static_cast<uint8_t*>(m_black->data)[i * 4 + 3] = 0xff;
    return true;
}

void FrameRenderer::shutdown()
{
    for (auto& v : m_vertexCache)
        sceGxmShaderPatcherReleaseVertexProgram(m_patcher, v.second);
    for (auto& f : m_fragmentCache)
        sceGxmShaderPatcherReleaseFragmentProgram(m_patcher, f.second);
    for (auto& t : m_targets)
        sceGxmDestroyRenderTarget(t.second);
    for (VertexProgram& p : m_vertexPrograms)
        if (p.id)
            sceGxmShaderPatcherUnregisterProgram(m_patcher, p.id);
    for (int f = 0; f < render::FpCount; ++f)
        for (int v = 0; v < Variants; ++v)
            if (m_fragmentPrograms[f][v].owned && m_fragmentPrograms[f][v].id)
                sceGxmShaderPatcherUnregisterProgram(m_patcher, m_fragmentPrograms[f][v].id);
    delete m_textureHeap;
    delete m_bufferHeap;
    if (m_arenaUid >= 0)
        sceKernelFreeMemBlock(m_arenaUid);
    if (m_sequenceUid >= 0)
        sceKernelFreeMemBlock(m_sequenceUid);
}

// ---- Frames (game thread) ------------------------------------------------------------------------------------------

uint32_t FrameRenderer::completedFrame()
{
    for (Slot& s : m_slots)
    {
        if (s.inFlight && *s.done.address == s.done.value)
        {
            s.inFlight = false;
            m_completed = std::max(m_completed, s.frame.index);
        }
    }
    return m_completed;
}

render::Frame* FrameRenderer::beginFrame(uint32_t width, uint32_t height)
{
    const uint32_t index = m_lastIndex + 1;
    Slot& s = m_slots[index % Slots];
    if (s.inFlight)
    {
        sceGxmNotificationWait(&s.done);
        s.inFlight = false;
        m_completed = std::max(m_completed, s.frame.index);
    }
    completedFrame();
    freePending();

    // Back buffers of another size than the display are drawn offscreen and scaled when shown.
    const bool direct = width == 960 && height == 544;
    if (!direct && (!s.offscreen || s.offscreen->desc.width != width || s.offscreen->desc.height != height))
    {
        if (s.offscreen)
            release(s.offscreen);
        render::TextureDesc desc = { texture::FormatARGB8888, 1, false, true, uint16_t(width), uint16_t(height) };
        s.offscreen = static_cast<Texture*>(createTexture(desc));
    }

    m_lastIndex = index;
    render::Frame& f = s.frame;
    f.index = index;
    f.width = uint16_t(width);
    f.height = uint16_t(height);
    f.scenes = s.scenes.data();
    f.sceneCount = 0;
    f.sceneCapacity = uint32_t(s.scenes.size());
    f.draws = s.draws.data();
    f.drawCount = 0;
    f.drawCapacity = uint32_t(s.draws.size());
    f.viewports = s.viewports.data();
    f.viewportCount = 0;
    f.viewportCapacity = uint32_t(s.viewports.size());
    f.constants = s.constants.data();
    f.constantCount = 0;
    f.constantCapacity = uint32_t(s.constants.size());
    f.vertices = static_cast<uint8_t*>(s.vertices);
    f.vertexBytes = 0;
    f.vertexCapacity = SlotVertexBytes;
    return &f;
}

void FrameRenderer::submitted(render::Frame* frame, uint32_t vblanks)
{
    Slot& s = m_slots[frame->index % Slots];
    s.vblanks = vblanks;
    s.done.value = frame->index;
    s.inFlight = true;
    if (s.offscreen)
        s.offscreen->lastUse = frame->index;
}

void FrameRenderer::freePending()
{
    size_t kept = 0;
    for (size_t i = 0; i < m_pending.size(); ++i)
    {
        if (m_pending[i].after <= m_completed)
            destroy(m_pending[i].kind, m_pending[i].object);
        else
            m_pending[kept++] = m_pending[i];
    }
    m_pending.resize(kept);
}

void FrameRenderer::destroy(int kind, void* object)
{
    if (kind == 0)
    {
        Texture* t = static_cast<Texture*>(object);
        m_textureHeap->free(t->data);
        delete t;
    }
    else if (kind == 1)
    {
        Depth* d = static_cast<Depth*>(object);
        m_textureHeap->free(d->data);
        m_textureHeap->free(d->stencil);
        delete d;
    }
    else
    {
        Buffer* b = static_cast<Buffer*>(object);
        m_bufferHeap->free(b->data);
        delete b;
    }
}

void FrameRenderer::release(render::TextureStorage* texture)
{
    if (texture)
        m_pending.push_back(Pending{ 0, texture, m_lastIndex + 1 });
}

void FrameRenderer::release(render::DepthStorage* depth)
{
    if (depth)
        m_pending.push_back(Pending{ 1, depth, m_lastIndex + 1 });
}

void FrameRenderer::release(render::BufferStorage* buffer)
{
    if (buffer)
        m_pending.push_back(Pending{ 2, buffer, m_lastIndex + 1 });
}

// ---- Resources (game thread) ---------------------------------------------------------------------------------------

render::TextureStorage* FrameRenderer::createTexture(const render::TextureDesc& desc)
{
    Texture* t = new Texture();
    t->desc = desc;
    const SceGxmTextureFormat format = textureFormat(desc.format);
    if (desc.target && !desc.cube)
    {
        // Linear: rendered to as a colour surface, rows of 8-pixel multiples.
        const uint32_t strideInPixels = alignUp(desc.width, 8);
        t->stride = strideInPixels * 4;
        t->bytes = t->stride * desc.height;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        sceGxmColorSurfaceInit(&t->surfaces[0], SCE_GXM_COLOR_FORMAT_A8R8G8B8, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, desc.width,
                               desc.height, strideInPixels, t->data);
        return t;
    }
    if (desc.linear && !desc.cube && desc.levels == 1 && !texture::isBlock(desc.format))
    {
        // Streamed: linear rows (8-pixel multiples) the CPU rewrites with a row copy instead of a swizzle.
        t->stride = alignUp(desc.width, 8) * texture::unitBytes(desc.format);
        t->bytes = t->stride * desc.height;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        return t;
    }
    t->stride = 0;
    t->faceStride = texture::faceStride(desc.format, desc.width, desc.height, desc.levels);
    t->bytes = texture::totalBytes(desc.format, desc.width, desc.height, desc.levels, desc.cube);
    t->data = m_textureHeap->allocate(t->bytes, 256);
    if (!t->data)
    {
        delete t;
        return nullptr;
    }
    if (desc.cube)
        sceGxmTextureInitCube(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    else
        sceGxmTextureInitSwizzled(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    if (desc.target)
    {
        // Cube target: each face a swizzled colour surface.
        for (uint32_t face = 0; face < 6; ++face)
            sceGxmColorSurfaceInit(&t->surfaces[face], SCE_GXM_COLOR_FORMAT_A8R8G8B8, SCE_GXM_COLOR_SURFACE_SWIZZLED,
                                   SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, desc.width,
                                   desc.height, desc.width, static_cast<uint8_t*>(t->data) + face * t->faceStride);
    }
    return t;
}

void FrameRenderer::writeTexture(render::TextureStorage* storage, uint32_t face, uint32_t level, const void* data,
                                 uint32_t pitch)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    uint8_t* base = static_cast<uint8_t*>(t->data) + face * t->faceStride;
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        if (pitch == t->stride)
            memcpy(base, data, size_t(rowBytes) + size_t(t->stride) * (d.height - 1));
        else
            for (uint32_t y = 0; y < d.height; ++y)
                memcpy(base + y * t->stride, static_cast<const uint8_t*>(data) + y * pitch, rowBytes);
        return;
    }
    texture::swizzleLevel(d.format, d.width, d.height, level, data, pitch,
                          base + texture::levelOffset(d.format, d.width, d.height, level));
}

void FrameRenderer::readTexture(const render::TextureStorage* storage, uint32_t face, uint32_t level, void* data,
                                uint32_t pitch)
{
    const Texture* t = static_cast<const Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    const uint8_t* base = static_cast<const uint8_t*>(t->data) + face * t->faceStride;
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        for (uint32_t y = 0; y < d.height; ++y)
            memcpy(static_cast<uint8_t*>(data) + y * pitch, base + y * t->stride, rowBytes);
        return;
    }
    texture::unswizzleLevel(d.format, d.width, d.height, level,
                            base + texture::levelOffset(d.format, d.width, d.height, level), data, pitch);
}

void FrameRenderer::copyTexture(render::TextureStorage* to, const render::TextureStorage* from)
{
    Texture* dst = static_cast<Texture*>(to);
    const Texture* src = static_cast<const Texture*>(from);
    if (dst->bytes == src->bytes)
        memcpy(dst->data, src->data, dst->bytes);
}

render::DepthStorage* FrameRenderer::createDepth(uint32_t width, uint32_t height)
{
    const uint32_t w = alignUp(width, SCE_GXM_TILE_SIZEX), h = alignUp(height, SCE_GXM_TILE_SIZEY);
    Depth* d = new Depth();
    d->width = uint16_t(width);
    d->height = uint16_t(height);
    d->data = m_textureHeap->allocate(w * h * 4, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    d->stencil = m_textureHeap->allocate(w * h, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    if (!d->data || !d->stencil)
    {
        m_textureHeap->free(d->data);
        m_textureHeap->free(d->stencil);
        delete d;
        return nullptr;
    }
    sceGxmDepthStencilSurfaceInit(&d->surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                  w, d->data, d->stencil);
    return d;
}

render::BufferStorage* FrameRenderer::createBuffer(uint32_t size)
{
    Buffer* b = new Buffer();
    b->size = size;
    b->data = m_bufferHeap->allocate(size, 16);
    if (!b->data)
    {
        delete b;
        return nullptr;
    }
    return b;
}

// ---- Drawing (render thread) ---------------------------------------------------------------------------------------

uint32_t FrameRenderer::vblanks(const render::Frame& frame) const
{
    return m_slots[frame.index % Slots].vblanks;
}

SceGxmNotification* FrameRenderer::doneNotification(const render::Frame& frame)
{
    return &m_slots[frame.index % Slots].done;
}

const SceGxmTexture* FrameRenderer::offscreen(const render::Frame& frame) const
{
    const Texture* t = m_slots[frame.index % Slots].offscreen;
    return (frame.width == 960 && frame.height == 544) || !t ? nullptr : &t->texture;
}

bool FrameRenderer::presentScene(const render::Frame& frame) const
{
    if (offscreen(frame))
        return true;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        if (!frame.scenes[i].target.color)
            return false;
    }
    return true;
}

SceGxmRenderTarget* FrameRenderer::renderTarget(uint32_t width, uint32_t height)
{
    const uint32_t key = width << 16 | height;
    auto it = m_targets.find(key);
    if (it != m_targets.end())
        return it->second;
    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = uint16_t(width);
    params.height = uint16_t(height);
    params.scenesPerFrame = 8;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    const int result = sceGxmCreateRenderTarget(&params, &target);
    if (result < 0)
        fatal("sceGxmCreateRenderTarget(%ux%u) failed: 0x%08x", width, height, result);
    m_targets[key] = target;
    return target;
}

SceGxmVertexProgram* FrameRenderer::vertexProgram(uint8_t program, uint8_t layoutId)
{
    const uint32_t key = uint32_t(program) << 8 | layoutId;
    auto it = m_vertexCache.find(key);
    if (it != m_vertexCache.end())
        return it->second;
    const VertexProgram& p = m_vertexPrograms[program < render::VpCount ? program : 0];
    const render::VertexLayout& layout = render::layout(layoutId);
    SceGxmVertexAttribute attributes[render::UsageCount];
    unsigned int count = 0;
    for (uint32_t u = 0; u < render::UsageCount; ++u)
    {
        const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program,
                                                                                    render::inputName(render::Usage(u)));
        if (!parameter)
            continue;
        const render::VertexElement* element = nullptr;
        for (uint32_t i = 0; i < layout.count; ++i)
        {
            if (layout.elements[i].usage == u)
                element = &layout.elements[i];
        }
        SceGxmVertexAttribute& a = attributes[count++];
        a.streamIndex = 0;
        a.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(parameter));
        if (element)
        {
            a.offset = element->offset;
            a.format = uint8_t(attributeFormat(element->type, &a.componentCount));
        }
        else
        {
            // The layout lacks the input: read the first element (unused by the program's result).
            logError("gxm: vertex program %s: layout %u has no %s", render::vertexProgram(render::VertexProgram(program)).name,
                     layoutId, render::inputName(render::Usage(u)));
            a.offset = layout.count ? layout.elements[0].offset : 0;
            a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a.componentCount = 1;
        }
    }
    SceGxmVertexStream stream = { layout.stride, SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    SceGxmVertexProgram* result = nullptr;
    const int error = sceGxmShaderPatcherCreateVertexProgram(m_patcher, p.id, attributes, count, &stream, 1, &result);
    if (error < 0)
        fatal("gxm: vertex program %s for layout %u: 0x%08x", render::vertexProgram(render::VertexProgram(program)).name,
              layoutId, error);
    m_vertexCache[key] = result;
    return result;
}

SceGxmFragmentProgram* FrameRenderer::fragmentProgram(const render::DrawState& state)
{
    const bool blend = state.blendEnable != 0;
    const uint8_t mask = state.colorMask;
    const uint32_t v = variant(state);
    const uint64_t key = uint64_t(state.vertexProgram) | uint64_t(state.fragmentProgram) << 8
                       | uint64_t(v) << 16 | uint64_t(blend) << 19 | uint64_t(mask) << 20
                       | (blend ? uint64_t(state.srcBlend) << 24 | uint64_t(state.dstBlend) << 32 : 0);
    auto it = m_fragmentCache.find(key);
    if (it != m_fragmentCache.end())
        return it->second;
    const FragmentProgram& p = m_fragmentPrograms[state.fragmentProgram < render::FpCount ? state.fragmentProgram : 0][v];
    SceGxmBlendInfo info;
    memset(&info, 0, sizeof(info));
    info.colorMask = uint8_t(((mask & 1) ? SCE_GXM_COLOR_MASK_R : 0) | ((mask & 2) ? SCE_GXM_COLOR_MASK_G : 0)
                           | ((mask & 4) ? SCE_GXM_COLOR_MASK_B : 0) | ((mask & 8) ? SCE_GXM_COLOR_MASK_A : 0));
    info.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    info.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    uint8_t src = blend ? state.srcBlend : 2, dst = blend ? state.dstBlend : 1;
    if (src == 12 || src == 13)
    {
        // BOTHSRCALPHA, BOTHINVSRCALPHA.
        dst = src == 12 ? 6 : 5;
        src = src == 12 ? 5 : 6;
    }
    const SceGxmBlendFactor colorSrc = blendFactor(src), colorDst = blendFactor(dst);
    info.colorSrc = colorSrc;
    info.colorDst = colorDst;
    info.alphaSrc = alphaFactor(colorSrc);
    info.alphaDst = alphaFactor(colorDst);
    const bool plain = !blend && mask == 0xf;
    const SceGxmProgram* vertex = m_vertexPrograms[state.vertexProgram < render::VpCount ? state.vertexProgram : 0].program;
    SceGxmFragmentProgram* result = nullptr;
    const int error = sceGxmShaderPatcherCreateFragmentProgram(m_patcher, p.id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                               SCE_GXM_MULTISAMPLE_NONE, plain ? nullptr : &info,
                                                               vertex, &result);
    if (error < 0)
        fatal("gxm: fragment program %u (vertex %u): 0x%08x", state.fragmentProgram, state.vertexProgram, error);
    m_fragmentCache[key] = result;
    return result;
}

void FrameRenderer::upload(void* buffer, const render::UniformField* fields, uint8_t count,
                           const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants)
{
    for (uint8_t i = 0; i < count && i < parameters.size(); ++i)
    {
        if (parameters[i])
            sceGxmSetUniformDataF(buffer, parameters[i], 0, fields[i].count, constants + fields[i].offset);
    }
}

void FrameRenderer::draw(const render::Frame& frame, const render::DrawCall& d)
{
    const render::DrawState& s = d.state;
    const bool first = !m_lastValid;
    const bool programs = first || s.vertexProgram != m_last.vertexProgram || s.fragmentProgram != m_last.fragmentProgram
                        || s.layout != m_last.layout || s.alphaTest != m_last.alphaTest
                        || s.blendEnable != m_last.blendEnable || s.srcBlend != m_last.srcBlend
                        || s.dstBlend != m_last.dstBlend || s.colorMask != m_last.colorMask
                        || s.factorOutput != m_last.factorOutput;
    if (programs)
    {
        sceGxmSetVertexProgram(m_context, vertexProgram(s.vertexProgram, s.layout));
        sceGxmSetFragmentProgram(m_context, fragmentProgram(s));
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    if ((programs || d.vertexConstants != m_lastVertexConstants) && vinfo.fieldCount && d.vertexConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveVertexDefaultUniformBuffer(m_context, &buffer);
        upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
               frame.constants + d.vertexConstants);
    }
    if ((programs || d.fragmentConstants != m_lastFragmentConstants) && finfo.fieldCount
        && d.fragmentConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(m_context, &buffer);
        upload(buffer, finfo.fields, finfo.fieldCount, m_fragmentPrograms[s.fragmentProgram][variant(s)].fields,
               frame.constants + d.fragmentConstants);
    }
    m_lastVertexConstants = d.vertexConstants;
    m_lastFragmentConstants = d.fragmentConstants;

    const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
    for (uint8_t i = 0; i < finfo.samplers; ++i)
    {
        if (!first && !programs && s.textures[i] == m_last.textures[i]
            && memcmp(&s.samplers[i], &m_last.samplers[i], sizeof(render::Sampler)) == 0)
            continue;
        const Texture* t = s.textures[i] ? static_cast<const Texture*>(s.textures[i]) : m_black;
        SceGxmTexture texture = t->texture;
        const render::Sampler& sm = s.samplers[i];
        const bool pow2 = powerOfTwo(t->desc.width) && powerOfTwo(t->desc.height);
        auto mode = [pow2](uint8_t a) {
            return !pow2 || a == render::AddressClamp ? SCE_GXM_TEXTURE_ADDR_CLAMP
                 : a == render::AddressMirror ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
        };
        if (!t->desc.cube)
        {
            sceGxmTextureSetUAddrMode(&texture, mode(sm.addressU));
            sceGxmTextureSetVAddrMode(&texture, mode(sm.addressV));
        }
        sceGxmTextureSetMinFilter(&texture, (sm.filter & 1) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&texture, (sm.filter & 2) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
        if (t->desc.levels > 1)
        {
            if (sm.mip == render::MipNone)
                sceGxmTextureSetMipmapCount(&texture, 1);
            sceGxmTextureSetMipFilter(&texture, sm.mip == render::MipLinear ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                                             : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
        }
        sceGxmSetFragmentTexture(m_context, fp.units[i], &texture);
    }

    if (first || d.viewport != m_lastViewport)
    {
        // Direct3D window coordinates have pixel centres at integers: half a pixel to GXM's.
        const render::Viewport& v = frame.viewports[d.viewport];
        sceGxmSetViewport(m_context, v.x + 0.5f * v.width + 0.5f, 0.5f * v.width, v.y + 0.5f * v.height + 0.5f,
                          -0.5f * v.height, v.minZ, v.maxZ - v.minZ);
        m_lastViewport = d.viewport;
    }
    if (first || s.depthEnable != m_last.depthEnable || s.depthWrite != m_last.depthWrite || s.depthFunc != m_last.depthFunc)
    {
        const uint32_t func = s.depthEnable && s.depthFunc >= 1 && s.depthFunc <= 8 ? s.depthFunc - 1u : 7u;
        sceGxmSetFrontDepthFunc(m_context, SceGxmDepthFunc(func << 22));
        sceGxmSetFrontDepthWriteEnable(m_context, s.depthEnable && s.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED
                                                                                 : SCE_GXM_DEPTH_WRITE_DISABLED);
    }
    if (first || s.cullMode != m_last.cullMode)
        sceGxmSetCullMode(m_context, s.cullMode == 2 ? SCE_GXM_CULL_CW : s.cullMode == 3 ? SCE_GXM_CULL_CCW
                                                                                          : SCE_GXM_CULL_NONE);
    m_last = s;
    m_lastValid = true;

    sceGxmSetVertexStream(m_context, 0, d.vertices);
    const uint32_t count = d.count;
    if (!d.indices && count > 65536)
    {
        logError("gxm: draw of %u vertices without indices skipped", count);
        return;
    }
    sceGxmDraw(m_context, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16, d.indices ? d.indices : m_sequence, count);
}

void FrameRenderer::planFrame(const render::Frame& frame)
{
    m_plan.resize(frame.sceneCount);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool used = scene.target.depth && scene.depthCleared;
        for (uint32_t n = 0; n < scene.drawCount && scene.target.depth && !used; ++n)
            used = frame.draws[scene.firstDraw + n].state.depthEnable != 0;
        m_plan[i].depthUsed = used;
    }
}

void FrameRenderer::drawFrame(const render::Frame& frame, const SceGxmColorSurface* display, SceGxmSyncObject* displaySync)
{
    Slot& slot = m_slots[frame.index % Slots];
    planFrame(frame);
    const bool lastWritesDone = !presentScene(frame);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        const SceGxmColorSurface* color = display;
        SceGxmSyncObject* sync = nullptr;
        uint32_t width = scene.width, height = scene.height;
        if (scene.target.color)
        {
            const Texture* t = static_cast<const Texture*>(scene.target.color);
            color = &t->surfaces[scene.target.face < 6 ? scene.target.face : 0];
        }
        else if (slot.offscreen && !(frame.width == 960 && frame.height == 544))
        {
            color = &slot.offscreen->surfaces[0];
        }
        else
        {
            sync = displaySync;
        }
        // Scenes whose draws never test depth (filter passes, the HUD) neither load nor store it.
        SceGxmDepthStencilSurface depth;
        const Depth* d = m_plan[i].depthUsed ? static_cast<const Depth*>(scene.target.depth) : nullptr;
        if (d)
        {
            depth = d->surface;
            if (scene.depthCleared)
                sceGxmDepthStencilSurfaceSetBackgroundDepth(&depth, scene.clearDepth);
            sceGxmDepthStencilSurfaceSetForceLoadMode(&depth, scene.depthCleared ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                                                 : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            // Stored only when the next scene of the frame using this depth continues with it.
            bool store = false;
            for (uint32_t j = i + 1; j < frame.sceneCount; ++j)
            {
                if (frame.scenes[j].target.depth == scene.target.depth && m_plan[j].depthUsed)
                {
                    store = !frame.scenes[j].depthCleared;
                    break;
                }
            }
            sceGxmDepthStencilSurfaceSetForceStoreMode(&depth, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                                     : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        sceGxmBeginScene(m_context, 0, renderTarget(width, height), nullptr, nullptr, sync, color, d ? &depth : nullptr);
        sceGxmSetViewportEnable(m_context, SCE_GXM_VIEWPORT_ENABLED);
        sceGxmSetRegionClip(m_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, width - 1, height - 1);
        sceGxmSetTwoSidedEnable(m_context, SCE_GXM_TWO_SIDED_DISABLED);
        m_lastValid = false;
        m_lastViewport = 0xffff;
        for (uint32_t n = 0; n < scene.drawCount; ++n)
            draw(frame, frame.draws[scene.firstDraw + n]);
        const bool last = i + 1 == frame.sceneCount;
        sceGxmEndScene(m_context, nullptr, last && lastWritesDone ? &slot.done : nullptr);
    }
}

}}
