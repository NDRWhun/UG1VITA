// Direct3D frames on SceGxm: GPU heaps, resources, frame slots and scene drawing (gxm_render.h).

#include "gxm_render.h"
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/jobs.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/texture.h>
#include <psp2/kernel/processmgr.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define NFSU_SHADERS(X) \
    X(ff_v) X(ff_screen_v) X(world_v) X(world_proj_v) X(worldheadlight_v) X(worldreflect_v) \
    X(worldheadlightreflect_v) X(worldbone_v) X(car_v) X(filter_v) X(clearz_v) \
    X(ff_color_f) X(ff_color_f_atest) X(ff_tex_f) X(ff_tex_f_atest) X(ff_proj_f) X(ff_proj_f_atest) \
    X(world_f) X(world_f_atest) X(world_proj_f) X(world_proj_f_atest) X(worldheadlight_f) X(worldheadlight_f_atest) \
    X(worldreflect_f) X(worldreflect_f_atest) X(worldheadlightreflect_f) X(worldheadlightreflect_f_atest) X(car_f) \
    X(car_f_atest) \
    X(filter1_f) X(filter1_f_atest) X(filter2_f) X(filter2_f_atest) X(filter3_f) X(filter3_f_atest) \
    X(filter4_f) X(filter4_f_atest) X(clear_f) \
    X(filter1_f_bfcolor) X(filter1_f_bfalpha) X(filter1_f_bfscale) X(filter2_f_bfcolor) X(filter2_f_bfalpha) \
    X(filter2_f_bfscale) X(filter3_f_bfcolor) X(filter3_f_bfalpha) X(filter3_f_bfscale) X(filter4_f_bfcolor) \
    X(filter4_f_bfalpha) X(filter4_f_bfscale) X(filter1_f_bfmask) X(world_half_f)     X(ff_color_f_half) X(ff_color_f_atest_half) X(ff_tex_f_half) X(ff_tex_f_atest_half) X(ff_proj_f_half)     X(ff_proj_f_atest_half) X(world_f_half) X(world_f_atest_half) X(world_proj_f_half) X(world_proj_f_atest_half)     X(worldheadlight_f_half) X(worldheadlight_f_atest_half) X(worldreflect_f_half) X(worldreflect_f_atest_half)     X(worldheadlightreflect_f_half) X(worldheadlightreflect_f_atest_half) X(car_f_half) X(car_f_atest_half)     X(filter1_f_half) X(filter1_f_atest_half) X(filter2_f_half) X(filter2_f_atest_half) X(filter3_f_half)     X(filter3_f_atest_half) X(filter4_f_half) X(filter4_f_atest_half)     X(filter1_f_bfcolor_half) X(filter1_f_bfalpha_half) X(filter1_f_bfscale_half) X(filter2_f_bfcolor_half)     X(filter2_f_bfalpha_half) X(filter2_f_bfscale_half) X(filter3_f_bfcolor_half) X(filter3_f_bfalpha_half)     X(filter3_f_bfscale_half) X(filter4_f_bfcolor_half) X(filter4_f_bfalpha_half) X(filter4_f_bfscale_half)     X(filter1_f_bfmask_half)

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

// Hitch log: a pool or heap grew by bytes in us.
void growth(uint32_t bytes, uint32_t us)
{
    perf::event(perf::EventGpuGrowths, 1);
    perf::event(perf::EventGpuGrowthKb, bytes / 1024);
    perf::event(perf::EventGpuGrowthUs, us);
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

// One level of a swizzled texture from rows of pitch bytes.
void writeSwizzled(const Texture& t, uint32_t face, uint32_t level, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    uint8_t* base = static_cast<uint8_t*>(t.data) + face * t.faceStride;
    texture::swizzleLevel(d.format, d.width, d.height, level, data, pitch,
                          base + texture::levelOffset(d.format, d.width, d.height, level));
}

// A linear texture's rows (stride bytes apart) from rows of pitch bytes.
void writeLinear(const Texture& t, uint32_t face, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    uint8_t* base = static_cast<uint8_t*>(t.data) + face * t.faceStride;
    const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
    if (pitch == t.stride)
        memcpy(base, data, size_t(rowBytes) + size_t(t.stride) * (d.height - 1));
    else
        for (uint32_t y = 0; y < d.height; ++y)
            memcpy(base + y * t.stride, static_cast<const uint8_t*>(data) + y * pitch, rowBytes);
}

// The smallest alpha of a written level of a 2D texture (render::TextureStorage::levelAlpha).
void noteAlpha(Texture& t, uint32_t level, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    if (d.cube || d.target || level >= 16)
        return;
    t.levelAlpha[level].store(texture::minAlpha(d.format, d.width, d.height, level, data, pitch),
                              std::memory_order_relaxed);
}

void copyAlpha(Texture& to, const Texture& from)
{
    for (uint32_t level = 0; level < 16; ++level)
        to.levelAlpha[level].store(from.levelAlpha[level].load(std::memory_order_relaxed), std::memory_order_relaxed);
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

// Default uniform buffer bytes of a program: its own size, and at least what the fields write.
uint32_t uniformBytes(const SceGxmProgram* program, const render::UniformField* fields,
                      const std::vector<const SceGxmProgramParameter*>& parameters)
{
    uint32_t bytes = sceGxmProgramGetDefaultUniformBufferSize(program);
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (parameters[i])
            bytes = std::max<uint32_t>(bytes, (sceGxmProgramParameterGetResourceIndex(parameters[i]) + fields[i].count) * 4);
    }
    return alignUp(bytes, 16);
}

// Every cache line of a draw.
inline void prefetchDraw(const render::DrawCall* d)
{
    const char* p = reinterpret_cast<const char*>(d);
    __builtin_prefetch(p);
    __builtin_prefetch(p + 32);
    __builtin_prefetch(p + 64);
    __builtin_prefetch(p + sizeof(render::DrawCall) - 1);
}

}

// ---- HostArena -----------------------------------------------------------------------------------------------------

HostArena::~HostArena()
{
    for (uint8_t* b : m_blocks)
        delete[] b;
}

void* HostArena::allocate(uint32_t size)
{
    size = alignUp(size, 16);
    if (m_block < m_blocks.size() && m_used + size > BlockSize)
    {
        ++m_block;
        m_used = 0;
    }
    if (m_block == m_blocks.size())
        m_blocks.push_back(new uint8_t[BlockSize]);
    void* p = m_blocks[m_block] + m_used;
    m_used += size;
    return p;
}

// ---- FramePool -----------------------------------------------------------------------------------------------------

FramePool::~FramePool()
{
    for (Chunk& c : m_chunks)
    {
        sceGxmUnmapMemory(c.base);
        sceKernelFreeMemBlock(c.uid);
    }
}

void FramePool::begin(uint32_t frameIndex, Finished finished, void* context)
{
    m_frame = frameIndex;
    m_finished = finished;
    m_context = context;
    if (m_current >= 0)
        m_chunks[m_current].lastUse = frameIndex;
}

int FramePool::freeChunk(uint32_t minimum)
{
    for (size_t i = 0; i < m_chunks.size(); ++i)
    {
        Chunk& c = m_chunks[i];
        if (int(i) != m_current && c.size >= minimum && (c.lastUse == 0 || m_finished(m_context, c.lastUse)))
        {
            c.used = 0;
            return int(i);
        }
    }
    const uint32_t size = alignUp(std::max(minimum, m_chunkSize), 4096);
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
    const SceUID uid = sceKernelAllocMemBlock(m_name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, nullptr);
    if (uid < 0)
        fatal("gxm: %s: out of memory for %u KB: 0x%08x", m_name, size / 1024, uid);
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0)
        fatal("gxm: %s: sceGxmMapMemory failed", m_name);
    m_chunks.push_back(Chunk{ uid, static_cast<uint8_t*>(base), size, 0, 0 });
    logInfo("gxm: %s pool +%u KB", m_name, size / 1024);
    if (timed)
        growth(size, perf::now() - start);
    return int(m_chunks.size() - 1);
}

void* FramePool::allocate(uint32_t size, uint32_t alignment)
{
    if (m_current >= 0)
    {
        Chunk& c = m_chunks[m_current];
        const uint32_t start = alignUp(c.used, alignment);
        if (start + size <= c.size)
        {
            c.used = start + size;
            c.lastUse = m_frame;
            return c.base + start;
        }
    }
    m_current = freeChunk(size + alignment);
    Chunk& c = m_chunks[m_current];
    c.used = size;
    c.lastUse = m_frame;
    return c.base;
}

void* FramePool::take(uint32_t minimum, uint32_t* size)
{
    m_current = freeChunk(minimum);
    Chunk& c = m_chunks[m_current];
    c.used = c.size;
    c.lastUse = m_frame;
    *size = c.size;
    return c.base;
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
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
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
    if (timed)
        growth(size, perf::now() - start);
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

namespace
{

// Scene log (gpu_scene_log=on): every scene of a frame ends with a vertex and a fragment notification, a watcher
// thread notes when each is written and logs the frames whose GPU work ended more than SceneLogSlowMs after the
// frame before, scene by scene, and an average per scene every SceneLogAverageFrames frames.
const uint32_t SceneLogVertexWords = 64;        // notification words 64.. (TA done), 192.. (3D done)
const uint32_t SceneLogFragmentWords = 192;
const uint32_t SceneLogScenes = 120;
const uint32_t SceneLogFrames = 8;
const float SceneLogSlowMs = 40.0f;
const uint32_t SceneLogAverageFrames = 300;

struct SceneLogScene
{
    uint16_t width, height, draws, blended, alphaTested;
    uint8_t samples;                            // 1, or the msaa sample count
    char program[20];                           // fragment program of the first draw that is not a clear
};

struct SceneLogFrame
{
    uint32_t index, count, submitUs;
    volatile unsigned int* done[SceneLogScenes];
    SceneLogScene scenes[SceneLogScenes];
};

SceneLogFrame s_sceneLog[SceneLogFrames];
std::atomic<uint32_t> s_sceneLogHead{ 0 }, s_sceneLogTail{ 0 };
volatile unsigned int* s_notifications = nullptr;
pf::Thread* s_sceneLogThread = nullptr;

// Notification values are frame indices, written in order.
inline bool reached(volatile unsigned int* word, uint32_t index)
{
    return int32_t(*word - index) >= 0;
}

int sceneLogEntry(void*)
{
    static uint32_t ta[SceneLogScenes], done[SceneLogScenes];
    static double sums[SceneLogScenes];
    uint32_t averaged = 0, previousEnd = 0;
    char line[2048];
    for (;;)
    {
        const uint32_t tail = s_sceneLogTail.load(std::memory_order_acquire);
        if (tail == s_sceneLogHead.load(std::memory_order_acquire))
        {
            sceKernelDelayThread(200);
            continue;
        }
        const SceneLogFrame& f = s_sceneLog[tail % SceneLogFrames];
        memset(ta, 0, sizeof(ta));
        memset(done, 0, sizeof(done));
        uint32_t finished = 0, now = f.submitUs;
        while (finished < f.count && now - f.submitUs < 2000000)
        {
            now = sceKernelGetProcessTimeLow();
            for (uint32_t k = 0; k < f.count; ++k)
            {
                if (!ta[k] && reached(s_notifications + SceneLogVertexWords + k, f.index))
                    ta[k] = now;
                if (!done[k] && reached(f.done[k], f.index))
                {
                    done[k] = now;
                    ++finished;
                }
            }
            sceKernelDelayThread(80);
        }
        if (finished == f.count && f.count)
        {
            // A scene's 3D work starts after the scene before it and after its own TA work.
            float ms[SceneLogScenes], total = 0;
            for (uint32_t k = 0; k < f.count; ++k)
            {
                uint32_t start = k ? done[k - 1] : std::max(previousEnd, f.submitUs);
                if (int32_t(ta[k] - start) > 0)
                    start = ta[k];
                ms[k] = int32_t(done[k] - start) > 0 ? float(done[k] - start) / 1000.0f : 0.0f;
                total += ms[k];
                sums[k] += ms[k];
            }
            const float interval = previousEnd ? float(int32_t(done[f.count - 1] - previousEnd)) / 1000.0f : 0.0f;
            if (interval > SceneLogSlowMs)
            {
                int n = snprintf(line, sizeof(line), "gxm: scene log frame %u: GPU done %.1f ms after the frame before, "
                                 "3D %.1f ms; scene WxH program draws/blended/alpha-tested 3D ms:", f.index, interval,
                                 total);
                for (uint32_t k = 0; k < f.count && n < int(sizeof(line)) - 80; ++k)
                {
                    const SceneLogScene& s = f.scenes[k];
                    n += snprintf(line + n, sizeof(line) - n, " %u %ux%u%s %s %u/%u/%u %.2f", k, s.width, s.height,
                                  s.samples == 4 ? "/4x" : s.samples == 2 ? "/2x" : "", s.program, s.draws, s.blended,
                                  s.alphaTested, ms[k]);
                }
                logInfo("%s", line);
            }
            previousEnd = done[f.count - 1];
            if (++averaged == SceneLogAverageFrames)
            {
                int n = snprintf(line, sizeof(line), "gxm: scene log average of %u frames, scene WxH 3D ms:",
                                 SceneLogAverageFrames);
                for (uint32_t k = 0; k < f.count && n < int(sizeof(line)) - 40; ++k)
                    n += snprintf(line + n, sizeof(line) - n, " %u %ux%u%s %.2f", k, f.scenes[k].width,
                                  f.scenes[k].height,
                                  f.scenes[k].samples == 4 ? "/4x" : f.scenes[k].samples == 2 ? "/2x" : "",
                                  sums[k] / SceneLogAverageFrames);
                logInfo("%s", line);
                memset(sums, 0, sizeof(sums));
                averaged = 0;
            }
        }
        else
        {
            previousEnd = 0;
        }
        s_sceneLogTail.store(tail + 1, std::memory_order_release);
    }
    return 0;
}

}

bool FrameRenderer::init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications)
{
    m_context = context;
    m_patcher = patcher;
    m_main.context = context;
    m_uploadLock = new Mutex("gxm_uploads");
    m_uploadDone = new CondVar("gxm_uploads", *m_uploadLock);
    m_precomputed = Config::getBool("gxm_precomputed", true);
    logInfo("gxm: %s draws", m_precomputed ? "precomputed" : "immediate");
    m_cullSwap = Config::getBool("cull_swap", false);
    if (m_cullSwap)
        logInfo("gxm: cull winding swapped (cull_swap=on)");
    static const char* const DrawOrders[] = { "sorted", "game" };
    m_drawSort = Config::getChoice("alpha_draw_order", DrawOrders, 2, 0) == 0;
    m_sceneLog = Config::getBool("gpu_scene_log", false);
    if (m_sceneLog)
    {
        s_notifications = notifications;
        for (uint32_t k = 0; k < SceneLogScenes; ++k)
            notifications[SceneLogVertexWords + k] = notifications[SceneLogFragmentWords + k] = 0;
        s_sceneLogThread = new Thread("gxm_scene_log", &sceneLogEntry, nullptr, Thread::PriorityHigh, 32 * 1024,
                                      cores::mask(cores::Helper));
        s_sceneLogThread->start();
        logInfo("gxm: scene log on (gpu_scene_log): frames whose GPU work ends over %.0f ms after the frame before, "
                "scene by scene", SceneLogSlowMs);
    }
    logInfo("gxm: alpha-tested draws %s (alpha_draw_order=%s)", m_drawSort ? "after opaque draws" : "in game order",
            m_drawSort ? "sorted" : "game");
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
        p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
    }
    // shader_precision=half: the effect and fixed-function fragment programs with half-precision colours (the
    // _half builds; same uniforms and texture units).
    static const char* const ShaderPrecisions[] = { "half", "full" };
    const bool shaderHalf = Config::getChoice("shader_precision", ShaderPrecisions, 2, 0) == 0;
    logInfo("gxm: effect and fixed-function fragment programs in %s precision (shader_precision=%s)",
            shaderHalf ? "half" : "full", shaderHalf ? "half" : "full");
    for (int f = 0; f < render::FpCount; ++f)
    {
        const render::FragmentProgramInfo& info = render::fragmentProgram(render::FragmentProgram(f));
        for (int v = 0; v < Variants; ++v)
        {
            // Without the variant: the plain program (the layer never selects it).
            if ((v == 1 && f == render::FpClear) || (v >= 2 && !info.blendFactor)
                || (v == 1 + render::FactorAlphaMask && f != render::FpFilter1))
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
            snprintf(name, sizeof(name), "%s%s%s", info.name,
                     v == 1 ? "_atest" : render::factorSuffix(render::FactorOutput(v >= 2 ? v - 1 : 0)),
                     shaderHalf && f != render::FpClear ? "_half" : "");
            FragmentProgram& p = m_fragmentPrograms[f][v];
            p.owned = true;
            p.program = registerProgram(name, &p.id);
            for (uint8_t i = 0; i < info.fieldCount; ++i)
                p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
            p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
            for (uint8_t s = 0; s < info.samplers; ++s)
            {
                char sampler[8] = "uTex0";
                sampler[4] = char('0' + s);
                const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program, sampler);
                p.units[s] = parameter ? sceGxmProgramParameterGetResourceIndex(parameter) : 0;
            }
        }
    }
    // The draws keep world_f's texture unit and uniform layout, so the half program must match them.
    static const char* const Precisions[] = { "half", "full" };
    m_particleHalf = Config::getChoice("particle_precision", Precisions, 2, 0) == 0;
    if (m_particleHalf)
    {
        const FragmentProgram& world = m_fragmentPrograms[render::FpWorld][0];
        const SceGxmProgram* half = registerProgram("world_half_f", &m_worldHalf);
        const SceGxmProgramParameter* sampler = sceGxmProgramFindParameterByName(half, "uTex0");
        if (!sampler || sceGxmProgramParameterGetResourceIndex(sampler) != world.units[0]
            || sceGxmProgramGetDefaultUniformBufferSize(half) > world.uniformBytes)
            fatal("shader world_half_f does not match world_f's texture unit and uniforms");
    }
    logInfo("gxm: depth-tested SRCALPHA/INVSRCALPHA world draws in %s precision "
            "(particle_precision=%s)", m_particleHalf ? "half" : "full", m_particleHalf ? "half" : "full");

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
    initLists();
    return true;
}

void FrameRenderer::shutdown()
{
    if (m_uploadLock)
    {
        LockGuard guard(*m_uploadLock);
        while (!m_uploading.empty())
            m_uploadDone->wait();
    }
    shutdownLists();
    for (auto& v : m_vertexCache)
        sceGxmShaderPatcherReleaseVertexProgram(m_patcher, v.second.program);
    for (auto& f : m_fragmentCache)
        sceGxmShaderPatcherReleaseFragmentProgram(m_patcher, f.second.program);
    for (auto& t : m_targets)
        sceGxmDestroyRenderTarget(t.second);
    for (VertexProgram& p : m_vertexPrograms)
        if (p.id)
            sceGxmShaderPatcherUnregisterProgram(m_patcher, p.id);
    for (int f = 0; f < render::FpCount; ++f)
        for (int v = 0; v < Variants; ++v)
            if (m_fragmentPrograms[f][v].owned && m_fragmentPrograms[f][v].id)
                sceGxmShaderPatcherUnregisterProgram(m_patcher, m_fragmentPrograms[f][v].id);
    if (m_worldHalf)
        sceGxmShaderPatcherUnregisterProgram(m_patcher, m_worldHalf);
    delete m_textureHeap;
    delete m_bufferHeap;
    delete m_uploadDone;
    delete m_uploadLock;
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
        const bool timed = perf::hitch();
        const uint32_t start = timed ? perf::now() : 0;
        sceGxmNotificationWait(&s.done);
        if (timed)
            perf::event(perf::EventFenceWaitUs, perf::now() - start);
        s.inFlight = false;
        // A worker whose job was taken over may still read the slot's draws.
        waitRecorders();
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
    f.resolve = nullptr;
    f.commands = nullptr;
    f.commandCount = 0;
    f.uploads = false;
    f.msaaSamples = 1;
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
        // A texture is freed once no queued upload touches it either.
        if (m_pending[i].after <= m_completed
            && (m_pending[i].kind != 0 || uploadDone(static_cast<Texture*>(m_pending[i].object)->lastTicket)))
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
        memset(t->data, 0, t->bytes);
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
    // Cube mip count 0: one level per face; any other count: faces with mips down to 1x1.
    if (desc.cube)
        sceGxmTextureInitCube(&t->texture, t->data, format, desc.width, desc.height, desc.levels > 1 ? desc.levels : 0);
    else
        sceGxmTextureInitSwizzled(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    // LOD bias 0, the Direct3D default: the field holds bias * 8 + 31.
    sceGxmTextureSetLodBias(&t->texture, 31);
    if (desc.target)
    {
        // Cube target: each face a swizzled colour surface, black until the game draws it.
        memset(t->data, 0, t->bytes);
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
    settle(t);
    if (perf::timed())
    {
        perf::upload(perf::UploadLevels, 1);
        perf::upload(perf::UploadBytes, texture::levelBytes(d.format, d.width, d.height, level));
    }
    noteAlpha(*t, level, data, pitch);
    if (t->stride)
        writeLinear(*t, face, data, pitch);
    else
        writeSwizzled(*t, face, level, data, pitch);
}

void FrameRenderer::readTexture(const render::TextureStorage* storage, uint32_t face, uint32_t level, void* data,
                                uint32_t pitch)
{
    const Texture* t = static_cast<const Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    settle(t);
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
    settle(dst);
    settle(src);
    if (dst->bytes == src->bytes)
    {
        memcpy(dst->data, src->data, dst->bytes);
        copyAlpha(*dst, *src);
    }
}

uint32_t FrameRenderer::writeTextureAsync(render::TextureStorage* storage, uint32_t face, uint32_t level,
                                          const void* data, uint32_t pitch)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return 0;
    if (perf::timed())
    {
        perf::upload(perf::UploadLevels, 1);
        perf::upload(perf::UploadBytes, texture::levelBytes(d.format, d.width, d.height, level));
    }
    t->uploads.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ticket = queueUpload(new Upload{ this, 0, t->lastTicket, t, nullptr, face, level, pitch, data });
    t->lastTicket = ticket;
    return ticket;
}

uint32_t FrameRenderer::copyTextureAsync(render::TextureStorage* to, const render::TextureStorage* from)
{
    Texture* dst = static_cast<Texture*>(to);
    Texture* src = const_cast<Texture*>(static_cast<const Texture*>(from));
    if (dst->bytes != src->bytes)
        return 0;
    dst->uploads.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ticket = queueUpload(new Upload{ this, 0, std::max(dst->lastTicket, src->lastTicket), dst, src,
                                                    0, 0, 0, nullptr });
    dst->lastTicket = ticket;
    src->lastTicket = ticket;
    return ticket;
}

uint32_t FrameRenderer::queueUpload(Upload* upload)
{
    const uint32_t ticket = m_nextTicket++;
    upload->ticket = ticket;
    {
        LockGuard guard(*m_uploadLock);
        m_uploading.push_back(ticket);
        m_firstUploading.store(m_uploading.front(), std::memory_order_relaxed);
    }
    if (perf::timed())
        perf::upload(perf::UploadAsync, 1);
    jobs::post(&runUpload, upload);
    return ticket;
}

void FrameRenderer::runUpload(void* argument)
{
    Upload* u = static_cast<Upload*>(argument);
    FrameRenderer* self = u->owner;
    self->waitUpload(u->after);
    const bool timed = perf::timed();
    const uint32_t start = timed ? perf::now() : 0;
    if (u->from)
    {
        memcpy(u->to->data, u->from->data, u->to->bytes);
        copyAlpha(*u->to, *u->from);
    }
    else
    {
        if (u->to->stride)
            writeLinear(*u->to, u->face, u->data, u->pitch);
        else
            writeSwizzled(*u->to, u->face, u->level, u->data, u->pitch);
        noteAlpha(*u->to, u->level, u->data, u->pitch);
    }
    __asm__ volatile("dsb" : : : "memory");
    if (timed)
        perf::upload(perf::UploadWorkerUs, perf::now() - start);
    {
        LockGuard guard(*self->m_uploadLock);
        const auto it = std::lower_bound(self->m_uploading.begin(), self->m_uploading.end(), u->ticket);
        if (it != self->m_uploading.end() && *it == u->ticket)
            self->m_uploading.erase(it);
        self->m_firstUploading.store(self->m_uploading.empty() ? 0xffffffffu : self->m_uploading.front(),
                                     std::memory_order_release);
        u->to->uploads.fetch_sub(1, std::memory_order_release);
        self->m_uploadDone->broadcast();
    }
    delete u;
}

bool FrameRenderer::uploadDone(uint32_t ticket)
{
    if (ticket < m_firstUploading.load(std::memory_order_acquire))
        return true;
    LockGuard guard(*m_uploadLock);
    return !std::binary_search(m_uploading.begin(), m_uploading.end(), ticket);
}

void FrameRenderer::waitUpload(uint32_t ticket)
{
    if (!ticket)
        return;
    LockGuard guard(*m_uploadLock);
    while (std::binary_search(m_uploading.begin(), m_uploading.end(), ticket))
        m_uploadDone->wait();
}

void FrameRenderer::waitWrites(const render::TextureStorage* texture)
{
    if (!texture->uploads.load(std::memory_order_acquire))
        return;
    LockGuard guard(*m_uploadLock);
    while (texture->uploads.load(std::memory_order_acquire))
        m_uploadDone->wait();
}

void FrameRenderer::settle(const Texture* texture)
{
    if (uploadDone(texture->lastTicket))
        return;
    const bool timed = perf::timed();
    const uint32_t start = timed ? perf::now() : 0;
    waitUpload(texture->lastTicket);
    if (timed)
    {
        perf::upload(perf::UploadGameWaits, 1);
        perf::upload(perf::UploadGameWaitUs, perf::now() - start);
    }
}

render::DepthStorage* FrameRenderer::createDepth(uint32_t width, uint32_t height, uint32_t samples)
{
    const uint32_t w = alignUp(width, SCE_GXM_TILE_SIZEX), h = alignUp(height, SCE_GXM_TILE_SIZEY);
    samples = samples == 4 ? 4 : samples == 2 ? 2 : 1;
    Depth* d = new Depth();
    d->width = uint16_t(width);
    d->height = uint16_t(height);
    // Depth plane only, no stencil plane; with msaa a value per sample: 2x doubles the rows, 4x the rows and the row
    // length.
    const uint32_t bytes = w * h * 4 * samples;
    d->data = m_textureHeap->allocate(bytes, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    d->stencil = nullptr;
    if (!d->data)
    {
        delete d;
        return nullptr;
    }
    sceGxmDepthStencilSurfaceInit(&d->surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                  w, d->data, nullptr);
    if (samples > 1)
    {
        d->samples = uint8_t(samples);
        sceGxmDepthStencilSurfaceInit(&d->msaaSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M,
                                      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, samples == 4 ? w * 2 : w, d->data,
                                      nullptr);
        if (m_depthLogs < 16)
        {
            ++m_depthLogs;
            logInfo("gxm: depth %ux%u for msaa %ux: %u bytes (%u without msaa)", unsigned(width), unsigned(height),
                    unsigned(samples), unsigned(bytes), unsigned(w * h * 4));
        }
    }
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

const SceGxmTexture* FrameRenderer::offscreen(const render::Frame& frame) const
{
    const Slot& s = m_slots[frame.index % Slots];
    return s.direct || !s.offscreen ? nullptr : &s.offscreen->texture;
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

SceGxmRenderTarget* FrameRenderer::renderTarget(uint32_t width, uint32_t height, bool msaa)
{
    // Key: width << 16 | height, the multisample mode (of the frame being drawn) in bits 30 and 31.
    const uint32_t key = width << 16 | height | (msaa ? uint32_t(m_msaa) << 30 : 0);
    auto it = m_targets.find(key);
    if (it != m_targets.end())
        return it->second;
    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = uint16_t(width);
    params.height = uint16_t(height);
    params.scenesPerFrame = 8;
    params.multisampleMode = msaa ? m_msaa : SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    const int result = sceGxmCreateRenderTarget(&params, &target);
    if (result < 0 && msaa)
    {
        logError("gxm: sceGxmCreateRenderTarget(%ux%u, msaa %ux) failed: 0x%08x", width, height,
                 unsigned(m_msaaSamples), result);
        return nullptr;
    }
    if (result < 0)
        fatal("sceGxmCreateRenderTarget(%ux%u) failed: 0x%08x", width, height, result);
    if (msaa)
    {
        unsigned int driverBytes = 0, plainBytes = 0;
        sceGxmGetRenderTargetMemSize(&params, &driverBytes);
        params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
        sceGxmGetRenderTargetMemSize(&params, &plainBytes);
        logInfo("gxm: render target %ux%u msaa %ux: driver memory %u bytes (%u without msaa)", width, height,
                unsigned(m_msaaSamples), driverBytes, plainBytes);
    }
    m_targets[key] = target;
    return target;
}

const FrameRenderer::VertexEntry* FrameRenderer::vertexProgram(uint8_t program, uint8_t layoutId)
{
    const uint32_t key = uint32_t(program) << 8 | layoutId;
    auto it = m_vertexCache.find(key);
    if (it != m_vertexCache.end())
        return &it->second;
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
    const bool timed = perf::hitch() && !m_loadingPrograms;
    const uint32_t start = timed ? perf::now() : 0;
    const int error = sceGxmShaderPatcherCreateVertexProgram(m_patcher, p.id, attributes, count, &stream, 1, &result);
    if (timed)
    {
        perf::renderPart(perf::RenderProgramCreates, 1);
        perf::renderPart(perf::RenderProgramCreateUs, perf::now() - start);
    }
    if (error < 0)
        fatal("gxm: vertex program %s for layout %u: 0x%08x", render::vertexProgram(render::VertexProgram(program)).name,
              layoutId, error);
    VertexEntry& entry = m_vertexCache[key];
    entry.program = result;
    entry.source = uint8_t(program < render::VpCount ? program : 0);
    entry.drawSize = sceGxmGetPrecomputedDrawSize(result);
    entry.stateSize = sceGxmGetPrecomputedVertexStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

// Fragment program cache key: the programs, the variant, blending, the colour mask, world_half_f in place of the
// program and the multisample mode.
static uint64_t fragmentKey(uint8_t vertexProgram, uint8_t fragmentProgram, uint32_t variant, bool blend, uint8_t mask,
                            uint8_t srcBlend, uint8_t dstBlend, bool half, uint32_t multisample)
{
    return uint64_t(vertexProgram) | uint64_t(fragmentProgram) << 8 | uint64_t(variant) << 16 | uint64_t(blend) << 19
         | uint64_t(mask) << 20 | (blend ? uint64_t(srcBlend) << 24 | uint64_t(dstBlend) << 32 : 0)
         | uint64_t(half) << 40 | uint64_t(multisample & 3) << 41;
}

// particle_precision=half: a depth-tested world_f draw without alpha test, blended SRCALPHA/INVSRCALPHA (or
// BOTHSRCALPHA), runs world_half_f. Depth-tested excludes the HUD and screen overlays drawn with the world effect.
bool FrameRenderer::halfPrecision(const render::DrawState& state) const
{
    return m_particleHalf && state.blendEnable && state.depthEnable && state.fragmentProgram == render::FpWorld
        && variant(state) == 0 && ((state.srcBlend == 5 && state.dstBlend == 6) || state.srcBlend == 12);
}

const FrameRenderer::FragmentEntry* FrameRenderer::fragmentProgram(const render::DrawState& state, bool msaa)
{
    const uint64_t key = fragmentKey(state.vertexProgram, state.fragmentProgram, variant(state),
                                     state.blendEnable != 0, state.colorMask, state.srcBlend, state.dstBlend,
                                     halfPrecision(state), msaa ? uint32_t(m_msaa) : 0);
    auto it = m_fragmentCache.find(key);
    if (it != m_fragmentCache.end())
        return &it->second;
    return createFragment(key);
}

const FrameRenderer::FragmentEntry* FrameRenderer::createFragment(uint64_t key)
{
    const uint8_t vertexProgram = uint8_t(key);
    const uint8_t fragmentProgram = uint8_t(key >> 8);
    const uint32_t v = uint32_t(key >> 16) & 7;
    const bool blend = (key >> 19) & 1;
    // With blending the factors follow the mask's 4 bits.
    const uint8_t mask = blend ? uint8_t(key >> 20) & 0xf : uint8_t(key >> 20);
    const uint8_t srcBlend = uint8_t(key >> 24), dstBlend = uint8_t(key >> 32);
    const FragmentProgram& p = m_fragmentPrograms[fragmentProgram < render::FpCount ? fragmentProgram : 0][v];
    SceGxmBlendInfo info;
    memset(&info, 0, sizeof(info));
    info.colorMask = uint8_t(((mask & 1) ? SCE_GXM_COLOR_MASK_R : 0) | ((mask & 2) ? SCE_GXM_COLOR_MASK_G : 0)
                           | ((mask & 4) ? SCE_GXM_COLOR_MASK_B : 0) | ((mask & 8) ? SCE_GXM_COLOR_MASK_A : 0));
    info.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    info.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    uint8_t src = blend ? srcBlend : 2, dst = blend ? dstBlend : 1;
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
    const bool half = (key >> 40) & 1;
    const SceGxmMultisampleMode multisample = SceGxmMultisampleMode((key >> 41) & 3);
    const SceGxmProgram* vertex = m_vertexPrograms[vertexProgram < render::VpCount ? vertexProgram : 0].program;
    SceGxmFragmentProgram* result = nullptr;
    const bool timed = perf::hitch() && !m_loadingPrograms;
    const uint32_t start = timed ? perf::now() : 0;
    const int error = sceGxmShaderPatcherCreateFragmentProgram(m_patcher, half ? m_worldHalf : p.id,
                                                               SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                               multisample, plain ? nullptr : &info,
                                                               vertex, &result);
    if (timed)
    {
        perf::renderPart(perf::RenderProgramCreates, 1);
        perf::renderPart(perf::RenderProgramCreateUs, perf::now() - start);
    }
    if (error < 0)
        fatal("gxm: fragment program %u (vertex %u): 0x%08x", fragmentProgram, vertexProgram, error);
    FragmentEntry& entry = m_fragmentCache[key];
    entry.program = result;
    entry.stateSize = sceGxmGetPrecomputedFragmentStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

namespace
{

// Program list file (loadPrograms, savePrograms): the header, the vertex entries, then the fragment cache keys.
const char ProgramListMagic[4] = { 'G', 'X', 'P', 'L' };
const uint32_t ProgramListVersion = 1;
const uint32_t ProgramListMaxEntries = 4096;

struct ProgramListHeader
{
    char        magic[4];
    uint32_t    version;
    uint32_t    vertexPrograms, fragmentPrograms, variants, layoutBytes;   // of the build that wrote it
    uint32_t    vertexCount, fragmentCount;
};

struct ProgramListVertex
{
    uint8_t                 program;
    uint8_t                 unused[3];
    render::VertexLayout    layout;
};

}

uint32_t FrameRenderer::loadPrograms(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    ProgramListHeader h;
    uint32_t created = 0;
    if (fread(&h, sizeof(h), 1, file) != 1 || memcmp(h.magic, ProgramListMagic, 4) != 0
        || h.version != ProgramListVersion || h.vertexPrograms != render::VpCount
        || h.fragmentPrograms != render::FpCount || h.variants != Variants
        || h.layoutBytes != sizeof(render::VertexLayout) || h.vertexCount > ProgramListMaxEntries
        || h.fragmentCount > ProgramListMaxEntries)
    {
        logInfo("gxm: %s is not a program list of this build, not used", path);
        fclose(file);
        return 0;
    }
    m_loadingPrograms = true;
    for (uint32_t i = 0; i < h.vertexCount; ++i)
    {
        ProgramListVertex e;
        if (fread(&e, sizeof(e), 1, file) != 1)
            break;
        if (e.program >= render::VpCount || e.layout.count > render::MaxVertexElements)
            continue;
        const uint8_t layout = render::registerLayout(e.layout);
        if (m_vertexCache.find(uint32_t(e.program) << 8 | layout) == m_vertexCache.end())
        {
            vertexProgram(e.program, layout);
            ++created;
        }
    }
    for (uint32_t i = 0; i < h.fragmentCount; ++i)
    {
        uint64_t key;
        if (fread(&key, sizeof(key), 1, file) != 1)
            break;
        const uint8_t vertex = uint8_t(key), fragment = uint8_t(key >> 8);
        const uint32_t v = uint32_t(key >> 16) & 7;
        const bool blend = (key >> 19) & 1;
        const uint8_t mask = blend ? uint8_t(key >> 20) & 0xf : uint8_t(key >> 20);
        // world_half_f entries only with particle_precision=half; multisampled ones of the 2x and 4x modes (the mode
        // can change at run time).
        const bool half = (key >> 40) & 1;
        const uint32_t multisample = uint32_t(key >> 41) & 3;
        if (vertex >= render::VpCount || fragment >= render::FpCount || v >= Variants || (half && !m_worldHalf)
            || multisample > uint32_t(SCE_GXM_MULTISAMPLE_4X)
            || fragmentKey(vertex, fragment, v, blend, mask, uint8_t(key >> 24), uint8_t(key >> 32), half,
                           multisample) != key
            || m_fragmentCache.find(key) != m_fragmentCache.end())
            continue;
        createFragment(key);
        ++created;
    }
    m_loadingPrograms = false;
    fclose(file);
    return created;
}

void FrameRenderer::savePrograms(const char* path) const
{
    FILE* file = fopen(path, "wb");
    if (!file)
    {
        logError("gxm: %s cannot be written", path);
        return;
    }
    ProgramListHeader h = {};
    memcpy(h.magic, ProgramListMagic, 4);
    h.version = ProgramListVersion;
    h.vertexPrograms = render::VpCount;
    h.fragmentPrograms = render::FpCount;
    h.variants = Variants;
    h.layoutBytes = sizeof(render::VertexLayout);
    h.vertexCount = uint32_t(std::min<size_t>(m_vertexCache.size(), ProgramListMaxEntries));
    h.fragmentCount = uint32_t(std::min<size_t>(m_fragmentCache.size(), ProgramListMaxEntries));
    bool ok = fwrite(&h, sizeof(h), 1, file) == 1;
    uint32_t written = 0;
    for (const auto& v : m_vertexCache)
    {
        if (written++ == h.vertexCount)
            break;
        ProgramListVertex e = {};
        e.program = uint8_t(v.first >> 8);
        e.layout = render::layout(uint8_t(v.first));
        ok = ok && fwrite(&e, sizeof(e), 1, file) == 1;
    }
    written = 0;
    for (const auto& f : m_fragmentCache)
    {
        if (written++ == h.fragmentCount)
            break;
        const uint64_t key = f.first;
        ok = ok && fwrite(&key, sizeof(key), 1, file) == 1;
    }
    ok = fclose(file) == 0 && ok;
    if (ok)
        logInfo("gxm: %u vertex and %u fragment programs listed in %s", unsigned(h.vertexCount),
                unsigned(h.fragmentCount), path);
    else
        logError("gxm: writing %s failed", path);
}

bool FrameRenderer::finished(void* self, uint32_t frameIndex)
{
    const Slot& s = static_cast<FrameRenderer*>(self)->m_slots[frameIndex % Slots];
    return *s.done.address >= frameIndex;
}

void FrameRenderer::preparePrograms(const render::Frame& frame)
{
    m_programs.resize(frame.drawCount);
    // The draws of msaa scenes, and the colour clears carried to one of them, use programs of the msaa mode.
    bool msaa = false;
    for (uint32_t i = 0; i < frame.sceneCount && !msaa; ++i)
        msaa = m_plan[i].msaa;
    if (msaa)
    {
        m_drawMsaa.assign(frame.drawCount, 0);
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            const render::Scene& scene = frame.scenes[i];
            if (m_plan[m_plan[i].skipped ? m_plan[i].carriedTo : i].msaa)
                memset(m_drawMsaa.data() + scene.firstDraw, 1, scene.drawCount);
        }
    }
    DrawPrograms last = { nullptr, nullptr };
    const render::DrawState* previous = nullptr;
    bool previousMsaa = false;
    for (uint32_t i = 0; i < frame.drawCount; ++i)
    {
        prefetchDraw(&frame.draws[i + 4]);
        const render::DrawState& s = frame.draws[i].state;
        const bool drawMsaa = msaa && m_drawMsaa[i];
        if (!frame.draws[i].count)
        {
            m_programs[i] = DrawPrograms{ nullptr, nullptr };
            continue;
        }
        // Programs looked up where the state differs from the previous draw's.
        if (!previous || s.vertexProgram != previous->vertexProgram || s.layout != previous->layout)
            last.vertex = vertexProgram(s.vertexProgram, s.layout);
        if (!previous || s.vertexProgram != previous->vertexProgram || s.fragmentProgram != previous->fragmentProgram
            || s.alphaTest != previous->alphaTest || s.factorOutput != previous->factorOutput
            || s.blendEnable != previous->blendEnable || s.srcBlend != previous->srcBlend
            || s.dstBlend != previous->dstBlend || s.colorMask != previous->colorMask
            || (m_particleHalf && s.depthEnable != previous->depthEnable) || drawMsaa != previousMsaa)
            last.fragment = fragmentProgram(s, drawMsaa);
        m_programs[i] = last;
        previous = &s;
        previousMsaa = drawMsaa;
    }
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

// Texture of sampler i of a draw with the sampler's modes; black without a texture.
SceGxmTexture FrameRenderer::samplerTexture(const render::DrawState& s, uint32_t i) const
{
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
    return texture;
}

// Viewport, depth test and culling of a draw, where they changed.
void FrameRenderer::setFixedState(ContextState& c, SceGxmContext* context, const render::Frame& frame,
                                  const render::DrawCall& d, bool first)
{
    const render::DrawState& s = d.state;
    if (first || d.viewport != c.viewport)
    {
        // Direct3D window coordinates moved by half a pixel, scaled by the scene's display scale.
        const render::Viewport& v = frame.viewports[d.viewport];
        const float sx = c.scaleX, sy = c.scaleY;
        sceGxmSetViewport(context, sx * (v.x + 0.5f * v.width + 0.5f), sx * 0.5f * v.width,
                          sy * (v.y + 0.5f * v.height + 0.5f), sy * -0.5f * v.height, v.minZ, v.maxZ - v.minZ);
        c.viewport = d.viewport;
    }
    if (first || s.depthEnable != c.last.depthEnable || s.depthWrite != c.last.depthWrite
        || s.depthFunc != c.last.depthFunc)
    {
        const uint32_t func = s.depthEnable && s.depthFunc >= 1 && s.depthFunc <= 8 ? s.depthFunc - 1u : 7u;
        sceGxmSetFrontDepthFunc(context, SceGxmDepthFunc(func << 22));
        sceGxmSetFrontDepthWriteEnable(context, s.depthEnable && s.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED
                                                                               : SCE_GXM_DEPTH_WRITE_DISABLED);
    }
    if (first || s.cullMode != c.last.cullMode)
    {
        const SceGxmCullMode cw = m_cullSwap ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
        const SceGxmCullMode ccw = m_cullSwap ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
        sceGxmSetCullMode(context, s.cullMode == 2 ? cw : s.cullMode == 3 ? ccw : SCE_GXM_CULL_NONE);
    }
}

void FrameRenderer::draw(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    if (!d.count)
        return;     // dropped by the layer
    if (!d.indices && d.count > 65536)
    {
        logError("gxm: draw of %u vertices without indices skipped", d.count);
        return;
    }
    if (r.precomputed)
    {
        drawPrecomputed(r, frame, index);
        return;
    }
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    if ((programs || d.vertexConstants != c.vertexConstants) && vinfo.fieldCount && d.vertexConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
               frame.constants + d.vertexConstants);
    }
    if ((programs || d.fragmentConstants != c.fragmentConstants) && finfo.fieldCount
        && d.fragmentConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
        upload(buffer, finfo.fields, finfo.fieldCount, m_fragmentPrograms[s.fragmentProgram][variant(s)].fields,
               frame.constants + d.fragmentConstants);
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;

    const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
    for (uint8_t i = 0; i < finfo.samplers; ++i)
    {
        if (!programs && s.textures[i] == c.last.textures[i]
            && memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) == 0)
            continue;
        const SceGxmTexture texture = samplerTexture(s, i);
        sceGxmSetFragmentTexture(context, fp.units[i], &texture);
    }
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmSetVertexStream(context, 0, d.vertices);
    sceGxmDraw(context, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16, d.indices ? d.indices : m_sequence, d.count);
}

// The same draw with precomputed objects: a vertex state when the vertex constants change, a fragment state when
// its constants, textures or samplers change, and a precomputed draw. Their memory and uniform buffers come from the
// recorder's pool and live until the frame is finished.
void FrameRenderer::drawPrecomputed(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    FramePool& pool = r.data;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    int error = 0;

    if (programs || d.vertexConstants != c.vertexConstants)
    {
        auto* state = static_cast<SceGxmPrecomputedVertexState*>(r.words.allocate(sizeof(SceGxmPrecomputedVertexState)));
        void* memory = pr.vertex->stateSize ? pool.allocate(pr.vertex->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedVertexStateInit(state, pr.vertex->program, memory);
        if (!error && pr.vertex->uniformBytes)
        {
            void* buffer = pool.allocate(pr.vertex->uniformBytes, 16);
            if (vinfo.fieldCount && d.vertexConstants != render::NoConstants)
                upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
                       frame.constants + d.vertexConstants);
            else
                memset(buffer, 0, pr.vertex->uniformBytes);
            sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedVertexState(context, state);
    }

    bool textures = programs || d.fragmentConstants != c.fragmentConstants;
    for (uint8_t i = 0; i < finfo.samplers && !textures; ++i)
        textures = s.textures[i] != c.last.textures[i]
                || memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) != 0;
    if (!error && textures)
    {
        const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
        auto* state = static_cast<SceGxmPrecomputedFragmentState*>(r.words.allocate(sizeof(SceGxmPrecomputedFragmentState)));
        void* memory = pr.fragment->stateSize ? pool.allocate(pr.fragment->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedFragmentStateInit(state, pr.fragment->program, memory);
        for (uint8_t i = 0; i < finfo.samplers && !error; ++i)
        {
            const SceGxmTexture texture = samplerTexture(s, i);
            error = sceGxmPrecomputedFragmentStateSetTexture(state, fp.units[i], &texture);
        }
        if (!error && pr.fragment->uniformBytes)
        {
            void* buffer = pool.allocate(pr.fragment->uniformBytes, 16);
            if (finfo.fieldCount && d.fragmentConstants != render::NoConstants)
                upload(buffer, finfo.fields, finfo.fieldCount, fp.fields, frame.constants + d.fragmentConstants);
            else
                memset(buffer, 0, pr.fragment->uniformBytes);
            sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedFragmentState(context, state);
    }

    SceGxmPrecomputedDraw* draw = nullptr;
    if (!error)
    {
        draw = static_cast<SceGxmPrecomputedDraw*>(r.words.allocate(sizeof(SceGxmPrecomputedDraw)));
        void* memory = pr.vertex->drawSize ? pool.allocate(pr.vertex->drawSize, 16) : nullptr;
        error = sceGxmPrecomputedDrawInit(draw, pr.vertex->program, memory);
        const void* streams[1] = { d.vertices };
        if (!error)
            error = sceGxmPrecomputedDrawSetAllVertexStreams(draw, streams);
    }
    if (error)
    {
        // Unbinds the precomputed states and draws this and the following draws immediately.
        logError("gxm: precomputed objects failed (0x%08x): immediate draws from now on", error);
        m_precomputed.store(false, std::memory_order_relaxed);
        r.precomputed = false;
        sceGxmSetPrecomputedVertexState(context, nullptr);
        sceGxmSetPrecomputedFragmentState(context, nullptr);
        c.reset(c.scaleX, c.scaleY);
        this->draw(r, frame, index);
        return;
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmPrecomputedDrawSetParams(draw, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16,
                                   d.indices ? d.indices : m_sequence, d.count);
    sceGxmDrawPrecomputed(context, draw);
}

void FrameRenderer::planFrame(const render::Frame& frame, bool stretchDirect)
{
    m_plan.resize(frame.sceneCount);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool used = scene.target.depth && scene.depthCleared;
        for (uint32_t n = 0; n < scene.drawCount && scene.target.depth && !used; ++n)
            used = frame.draws[scene.firstDraw + n].state.depthEnable != 0;
        m_plan[i].depthUsed = used;
        m_plan[i].skipped = false;
        m_plan[i].carriedTo = i;
    }

    // A scene of colour clears alone (the game clears the back buffer right after Present) moves to the start of
    // the next scene on its target when nothing in between samples that target and its depth clear is dead.
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool clears = scene.drawCount != 0;
        for (uint32_t n = 0; n < scene.drawCount && clears; ++n)
        {
            const render::DrawState& s = frame.draws[scene.firstDraw + n].state;
            clears = s.vertexProgram == render::VpClear && !s.depthEnable;
        }
        if (!clears)
            continue;
        uint32_t next = i + 1;
        while (next < frame.sceneCount && (frame.scenes[next].target.color != scene.target.color
                                           || frame.scenes[next].target.face != scene.target.face))
            ++next;
        if (next == frame.sceneCount)
            continue;
        bool sampled = false;
        for (uint32_t k = i + 1; k < next && scene.target.color && !sampled; ++k)
        {
            const render::Scene& between = frame.scenes[k];
            for (uint32_t n = 0; n < between.drawCount && !sampled; ++n)
                for (const render::TextureStorage* t : frame.draws[between.firstDraw + n].state.textures)
                    sampled = sampled || t == scene.target.color;
        }
        if (sampled)
            continue;
        if (scene.target.depth && scene.depthCleared)
        {
            bool dead = true;
            for (uint32_t k = i + 1; k < frame.sceneCount; ++k)
            {
                if (frame.scenes[k].target.depth == scene.target.depth && m_plan[k].depthUsed)
                {
                    dead = frame.scenes[k].depthCleared;
                    break;
                }
            }
            if (!dead)
                continue;
        }
        m_plan[i].skipped = true;
        m_plan[i].carriedTo = next;
        m_plan[i].depthUsed = false;
    }
    // A carried scene that is itself carried passes its clears on.
    for (uint32_t i = frame.sceneCount; i-- > 0;)
    {
        if (m_plan[i].skipped && m_plan[m_plan[i].carriedTo].skipped)
            m_plan[i].carriedTo = m_plan[m_plan[i].carriedTo].carriedTo;
    }

    // Setting msaa (the frame's mode, Frame::msaaSamples): a scene drawing with depth into a 2D target of the back
    // buffer's size is multisampled when its depth holds the mode's samples. A depth continued by a later scene keeps
    // its sample layout, so both scenes are multisampled or neither is.
    m_msaaSamples = frame.msaaSamples == 4 ? 4 : frame.msaaSamples == 2 ? 2 : 1;
    m_msaa = m_msaaSamples == 4 ? SCE_GXM_MULTISAMPLE_4X
           : m_msaaSamples == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
    if (m_msaaSamples != m_msaaModeShown)
    {
        if (m_msaaSamples > 1)
            logInfo("gxm: msaa %ux from frame %u: scenes drawing with depth into a 2D target of the back buffer's size "
                    "are multisampled, colour downscaled when stored", unsigned(m_msaaSamples), unsigned(frame.index));
        else
            logInfo("gxm: msaa off from frame %u", unsigned(frame.index));
        m_msaaModeShown = m_msaaSamples;
        m_msaaShown = ~0u;
        m_msaaFailed = false;
    }
    const auto nextDepthScene = [&](uint32_t i) {
        uint32_t j = i + 1;
        while (j < frame.sceneCount
               && (frame.scenes[j].target.depth != frame.scenes[i].target.depth || !m_plan[j].depthUsed))
            ++j;
        return j;
    };
    uint32_t msaaScenes = 0, msaaStores = 0;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        const render::TextureStorage* color = scene.target.color;
        const Depth* depth = static_cast<const Depth*>(scene.target.depth);
        m_plan[i].msaa = m_msaaSamples > 1 && !m_msaaFailed && m_plan[i].depthUsed && color && color->desc.target
                      && !color->desc.cube && scene.width == frame.width && scene.height == frame.height
                      && depth->samples == m_msaaSamples && depth->width >= scene.width && depth->height >= scene.height;
        m_plan[i].storesDepth = false;
    }
    // Multisampled scenes draw into the frame's size; without its multisampled render target every scene stays 1x.
    if (m_msaaSamples > 1 && !m_msaaFailed)
    {
        bool any = false;
        for (uint32_t i = 0; i < frame.sceneCount && !any; ++i)
            any = m_plan[i].msaa;
        if (any && !renderTarget(frame.width, frame.height, true))
        {
            m_msaaFailed = true;
            logError("gxm: no %ux%u msaa %ux render target; msaa off from frame %u until the mode changes",
                     unsigned(frame.width), unsigned(frame.height), unsigned(m_msaaSamples), unsigned(frame.index));
            for (uint32_t i = 0; i < frame.sceneCount; ++i)
                m_plan[i].msaa = false;
        }
    }
    for (bool changed = m_msaaSamples > 1; changed;)
    {
        changed = false;
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            if (!m_plan[i].depthUsed)
                continue;
            const uint32_t j = nextDepthScene(i);
            if (j < frame.sceneCount && !frame.scenes[j].depthCleared && m_plan[i].msaa != m_plan[j].msaa)
            {
                m_plan[i].msaa = m_plan[j].msaa = false;
                changed = true;
            }
        }
    }
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        if (!m_plan[i].depthUsed)
            continue;
        const uint32_t j = nextDepthScene(i);
        m_plan[i].storesDepth = j < frame.sceneCount && !frame.scenes[j].depthCleared;
        msaaScenes += m_plan[i].msaa;
        msaaStores += m_plan[i].msaa && m_plan[i].storesDepth;
    }
    // The first 16 changes of the number of msaa scenes and of those storing depth, in the log.
    const uint32_t msaaShown = msaaScenes << 16 | msaaStores;
    if (m_msaaSamples > 1 && msaaShown != m_msaaShown && m_msaaLogs < 16)
    {
        ++m_msaaLogs;
        logInfo("gxm: msaa %ux: %u of %u scenes multisampled (back buffer %ux%u), %u of them store depth for a later "
                "scene", unsigned(m_msaaSamples), unsigned(msaaScenes), unsigned(frame.sceneCount),
                unsigned(frame.width), unsigned(frame.height), unsigned(msaaStores));
    }
    m_msaaShown = msaaShown;

    // A stretched back buffer whose scenes need no depth is drawn to the display at its resolution: no offscreen
    // store and no scaling pass.
    Slot& slot = m_slots[frame.index % Slots];
    slot.direct = frame.width == 960 && frame.height == 544;
    if (!slot.direct && stretchDirect)
    {
        bool any = false, depth = false;
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            if (frame.scenes[i].target.color || m_plan[i].skipped)
                continue;
            any = true;
            depth = depth || m_plan[i].depthUsed;
        }
        slot.direct = any && !depth;
    }
    // The first 16 changes of the back buffer size or of where it is drawn, in the log.
    const uint32_t size = uint32_t(frame.width) << 16 | frame.height;
    if ((size != m_shownSize || slot.direct != m_shownDirect) && m_shownLogs < 16)
    {
        ++m_shownLogs;
        logInfo("gxm: back buffer %ux%u drawn %s", unsigned(frame.width), unsigned(frame.height),
                !slot.direct ? "offscreen, scaled to the display when shown"
                : frame.width == 960 && frame.height == 544 ? "to the display" : "to the display, scaled per draw");
    }
    m_shownSize = size;
    m_shownDirect = slot.direct;
}

namespace
{

enum SortClass
{
    SortFixed,          // stays in place; no draw moves across it
    SortOpaque,
    SortDiscard
};

SortClass sortClass(const render::DrawCall& d, const render::TextureStorage* target)
{
    if (!d.count)
        return SortOpaque;
    const render::DrawState& s = d.state;
    // D3DCMP_LESS 2, D3DCMP_LESSEQUAL 4.
    if (s.vertexProgram == render::VpClear || s.blendEnable || s.colorMask != 0xf || !s.depthEnable || !s.depthWrite
        || (s.depthFunc != 2 && s.depthFunc != 4))
        return SortFixed;
    for (const render::TextureStorage* t : s.textures)
        if (t && t == target)
            return SortFixed;
    return s.alphaTest ? SortDiscard : SortOpaque;
}

// Hidden-surface pass of a draw: 2 alpha-tested, 1 blended, 0 opaque.
uint32_t hsrPass(const render::DrawState& s)
{
    return s.alphaTest ? 2 : s.blendEnable ? 1 : 0;
}

uint32_t flushTransitions(const render::Frame& frame, const uint32_t* order, uint32_t count)
{
    uint32_t transitions = 0, previous = 0;
    for (uint32_t n = 0; n < count; ++n)
    {
        const uint32_t pass = hsrPass(frame.draws[order[n]].state);
        transitions += previous == 2 && pass < 2;
        previous = pass;
    }
    return transitions;
}

// Bit of a draw's first vertex in the 256-bit filter of the held draws.
uint32_t geometrySlot(const void* vertices)
{
    const uintptr_t v = uintptr_t(vertices);
    return uint32_t((v >> 4) ^ (v >> 12)) & 255;
}

}

void FrameRenderer::sortScene(const render::Frame& frame, uint32_t scene)
{
    const ScenePlan& p = m_plan[scene];
    if (p.skipped || p.orderCount < 2)
        return;
    uint32_t* order = m_order.data() + p.firstOrder;
    const bool timed = perf::enabled();
    const uint32_t before = timed ? flushTransitions(frame, order, p.orderCount) : 0;
    if (m_drawSort && p.depthUsed)
    {
        const render::TextureStorage* target = frame.scenes[scene].target.color;
        m_held.clear();
        uint32_t held[8] = {};
        uint32_t out = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
        {
            const uint32_t index = order[n];
            if (n + 4 < p.orderCount)
                prefetchDraw(&frame.draws[order[n + 4]]);
            const render::DrawCall& d = frame.draws[index];
            const SortClass c = sortClass(d, target);
            const uint32_t slot = geometrySlot(d.vertices);
            if (c == SortDiscard)
            {
                m_held.push_back(HeldDraw{ d.vertices, d.indices, d.count, index });
                held[slot >> 5] |= 1u << (slot & 31);
                continue;
            }
            bool release = c == SortFixed;
            // The same geometry drawn again stays after its alpha-tested draw.
            if (!release && (held[slot >> 5] & (1u << (slot & 31))))
            {
                for (const HeldDraw& e : m_held)
                {
                    if (e.vertices == d.vertices && e.indices == d.indices && e.count == d.count)
                    {
                        release = true;
                        break;
                    }
                }
            }
            if (release)
            {
                for (const HeldDraw& e : m_held)
                    order[out++] = e.index;
                m_held.clear();
                memset(held, 0, sizeof(held));
            }
            order[out++] = index;
        }
        for (const HeldDraw& e : m_held)
            order[out++] = e.index;
        m_held.clear();
    }
    if (!timed)
        return;
    const uint32_t after = flushTransitions(frame, order, p.orderCount);
    SortStats& s = m_sortStats;
    s.allBefore += before;
    s.allAfter += after;
    if (p.orderCount > s.largest)
    {
        uint32_t discards = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
            discards += frame.draws[order[n]].state.alphaTest ? 1 : 0;
        s.largest = p.orderCount;
        s.largestDiscards = discards;
        s.largestBefore = before;
        s.largestAfter = after;
    }
}

FrameRenderer::Shown FrameRenderer::drawFrame(const render::Frame& frame, const SceGxmColorSurface* display,
                                              SceGxmSyncObject* displaySync, bool stretchDirect)
{
    Slot& slot = m_slots[frame.index % Slots];
    // Hitch log: the time since the last mark goes to a part.
    const bool parts = perf::hitch();
    uint32_t mark = parts ? perf::now() : 0;
    const auto lap = [parts, &mark](perf::RenderPart part)
    {
        if (!parts)
            return;
        const uint32_t now = perf::now();
        perf::renderPart(part, now - mark);
        mark = now;
    };
    if (parts)
    {
        perf::renderPart(perf::RenderSceneCount, frame.sceneCount);
        perf::renderPart(perf::RenderDrawCount, frame.drawCount);
    }
    // A worker whose job the previous frame took over may still read the plan, order and programs changed below.
    waitRecorders();
    if (parts)
    {
        const uint32_t now = perf::now();
        perf::renderPart(perf::RenderLists, now - mark);
        perf::renderPart(perf::RenderListWait, now - mark);
        mark = now;
    }
    planFrame(frame, stretchDirect);
    Shown shown = {};
    shown.presentScene = presentScene(frame);
    if (const SceGxmTexture* texture = offscreen(frame))
    {
        shown.offscreen = true;
        shown.texture = *texture;
    }
    shown.done = &slot.done;
    shown.vblanks = slot.vblanks;
    const uint32_t sceneCount = frame.sceneCount;
    lap(perf::RenderPlan);
    preparePrograms(frame);
    lap(perf::RenderPrograms);
    m_main.data.begin(frame.index, &finished, this);
    m_main.words.reset();
    m_main.precomputed = m_precomputed.load(std::memory_order_relaxed);
    for (Worker* w : m_workers)
        w->recorder.precomputed = m_main.precomputed;
    const bool lastWritesDone = !shown.presentScene;

    // Where each scene draws, and its draws in order: the colour clears carried to it first.
    m_order.clear();
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        ScenePlan& p = m_plan[i];
        const render::Scene& scene = frame.scenes[i];
        p.color = display;
        p.sync = nullptr;
        p.width = scene.width;
        p.height = scene.height;
        p.scaleX = p.scaleY = 1.0f;
        if (scene.target.color)
        {
            const Texture* t = static_cast<const Texture*>(scene.target.color);
            p.color = &t->surfaces[scene.target.face < 6 ? scene.target.face : 0];
            if (p.msaa)
            {
                p.msaaColor = t->surfaces[0];
                sceGxmColorSurfaceSetScaleMode(&p.msaaColor, SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE);
                p.color = &p.msaaColor;
            }
        }
        else if (slot.direct)
        {
            p.sync = displaySync;
            p.width = 960;
            p.height = 544;
            p.scaleX = 960.0f / float(scene.width);
            p.scaleY = 544.0f / float(scene.height);
        }
        else if (slot.offscreen)
        {
            p.color = &slot.offscreen->surfaces[0];
        }
        else
        {
            p.sync = displaySync;
        }
        p.firstOrder = uint32_t(m_order.size());
        p.firstJob = p.jobCount = 0;
        if (!p.skipped)
        {
            for (uint32_t c = 0; c < i; ++c)
            {
                if (m_plan[c].skipped && m_plan[c].carriedTo == i)
                    for (uint32_t n = 0; n < frame.scenes[c].drawCount; ++n)
                        m_order.push_back(frame.scenes[c].firstDraw + n);
            }
            for (uint32_t n = 0; n < scene.drawCount; ++n)
                m_order.push_back(scene.firstDraw + n);
        }
        p.orderCount = uint32_t(m_order.size()) - p.firstOrder;
        sortScene(frame, i);
    }
    if (perf::enabled())
    {
        SortStats& s = m_sortStats;
        s.draws += s.largest;
        s.discards += s.largestDiscards;
        s.before += s.largestBefore;
        s.after += s.largestAfter;
        s.largest = s.largestDiscards = s.largestBefore = s.largestAfter = 0;
        if (++s.frames == 300)
        {
            const double n = s.frames;
            logInfo("perf: draw sort %s, per frame: largest scene %.0f draws, %.1f alpha-tested, HSR flush "
                    "transitions %.1f -> %.1f; all scenes %.1f -> %.1f", m_drawSort ? "on" : "off", s.draws / n,
                    s.discards / n, s.before / n, s.after / n, s.allBefore / n, s.allAfter / n);
            s = SortStats();
        }
    }
    lap(perf::RenderPlan);
    const bool lists = recordLists(frame);
    lap(perf::RenderLists);
    if (frame.uploads)
        waitFrameUploads(frame);
    lap(perf::RenderUploadWait);

    SceneLogFrame* log = nullptr;
    if (m_sceneLog && s_sceneLogHead.load() - s_sceneLogTail.load(std::memory_order_acquire) < SceneLogFrames)
    {
        log = &s_sceneLog[s_sceneLogHead.load() % SceneLogFrames];
        log->index = frame.index;
        log->count = 0;
        log->submitUs = sceKernelGetProcessTimeLow();
    }
    for (uint32_t i = 0; i < sceneCount; ++i)
    {
        const ScenePlan& p = m_plan[i];
        if (p.skipped)
            continue;
        const render::Scene& scene = frame.scenes[i];
        if (log && log->count < SceneLogScenes)
        {
            SceneLogScene& s = log->scenes[log->count];
            memset(&s, 0, sizeof(s));
            s.width = uint16_t(p.width);
            s.height = uint16_t(p.height);
            s.draws = uint16_t(p.orderCount);
            s.samples = uint8_t(p.msaa ? m_msaaSamples : 1);
            for (uint32_t n = 0; n < p.orderCount; ++n)
            {
                const render::DrawState& st = frame.draws[m_order[p.firstOrder + n]].state;
                s.blended += st.blendEnable ? 1 : 0;
                s.alphaTested += st.alphaTest ? 1 : 0;
                if (!s.program[0] && st.vertexProgram != render::VpClear && st.fragmentProgram < render::FpCount)
                    snprintf(s.program, sizeof(s.program), "%s",
                             render::fragmentProgram(render::FragmentProgram(st.fragmentProgram)).name);
            }
            if (!s.program[0])
                snprintf(s.program, sizeof(s.program), "clear");
        }
        // Scenes whose draws never test depth (filter passes, the HUD) neither load nor store it.
        SceGxmDepthStencilSurface depth;
        const Depth* d = m_plan[i].depthUsed ? static_cast<const Depth*>(scene.target.depth) : nullptr;
        if (d)
        {
            depth = p.msaa ? d->msaaSurface : d->surface;
            if (scene.depthCleared)
                sceGxmDepthStencilSurfaceSetBackgroundDepth(&depth, scene.clearDepth);
            sceGxmDepthStencilSurfaceSetForceLoadMode(&depth, scene.depthCleared ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                                                 : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            // Stored only when the next scene of the frame using this depth continues with it.
            sceGxmDepthStencilSurfaceSetForceStoreMode(&depth, p.storesDepth
                                                                   ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                                   : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        const int begun = sceGxmBeginScene(m_context, 0, renderTarget(p.width, p.height, p.msaa), nullptr, nullptr,
                                           p.sync, p.color, d ? &depth : nullptr);
        const bool last = i + 1 == sceneCount;
        SceGxmNotification* fragmentDone = last && lastWritesDone ? &slot.done : nullptr;
        // A multisampled scene the GPU refuses turns msaa off from the next frame on, until the mode changes; its
        // draws are left out of this frame. When it would have written the frame's notification, an empty 1x scene
        // into its colour surface writes it.
        if (begun < 0 && p.msaa)
        {
            if (!m_msaaFailed)
            {
                m_msaaFailed = true;
                logError("gxm: sceGxmBeginScene of a %ux%u msaa scene failed: 0x%08x; its draws are left out, msaa off "
                         "from the next frame until the mode changes", unsigned(p.width), unsigned(p.height), begun);
            }
            if (fragmentDone)
            {
                const Texture* t = static_cast<const Texture*>(scene.target.color);
                if (sceGxmBeginScene(m_context, 0, renderTarget(p.width, p.height, false), nullptr, nullptr, nullptr,
                                     &t->surfaces[0], nullptr) >= 0)
                    sceGxmEndScene(m_context, nullptr, fragmentDone);
            }
            continue;
        }
        bool recorded = lists && p.jobCount;
        for (uint32_t j = 0; j < p.jobCount && recorded; ++j)
        {
            const Job& job = m_jobs[p.firstJob + j];
            recorded = job.ok[job.used.load(std::memory_order_acquire)];
        }
        if (recorded)
        {
            for (uint32_t j = 0; j < p.jobCount; ++j)
            {
                Job& job = m_jobs[p.firstJob + j];
                const int error = sceGxmExecuteCommandList(m_context, &job.lists[job.used.load(std::memory_order_relaxed)]);
                if (error < 0)
                    logError("gxm: sceGxmExecuteCommandList failed: 0x%08x", error);
            }
        }
        else
        {
            sceGxmSetViewportEnable(m_context, SCE_GXM_VIEWPORT_ENABLED);
            sceGxmSetRegionClip(m_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
            sceGxmSetTwoSidedEnable(m_context, SCE_GXM_TWO_SIDED_DISABLED);
            m_main.state.reset(p.scaleX, p.scaleY);
            for (uint32_t n = 0; n < p.orderCount; ++n)
                draw(m_main, frame, m_order[p.firstOrder + n]);
        }
        if (log && log->count < SceneLogScenes)
        {
            SceGxmNotification ta = { s_notifications + SceneLogVertexWords + log->count, frame.index };
            SceGxmNotification done = { s_notifications + SceneLogFragmentWords + log->count, frame.index };
            if (!fragmentDone)
                fragmentDone = &done;
            log->done[log->count++] = fragmentDone->address;
            sceGxmEndScene(m_context, &ta, fragmentDone);
        }
        else
        {
            sceGxmEndScene(m_context, nullptr, fragmentDone);
        }
    }
    if (log)
        s_sceneLogHead.fetch_add(1, std::memory_order_release);
    // Unbinds the precomputed states.
    sceGxmSetPrecomputedVertexState(m_context, nullptr);
    sceGxmSetPrecomputedFragmentState(m_context, nullptr);
    lap(perf::RenderScenes);
    return shown;
}

void FrameRenderer::waitFrameUploads(const render::Frame& frame)
{
    const bool timed = perf::timed();
    for (uint32_t i = 0; i < frame.drawCount; ++i)
    {
        for (const render::TextureStorage* t : frame.draws[i].state.textures)
        {
            if (!t || !t->uploads.load(std::memory_order_acquire))
                continue;
            const uint32_t start = timed ? perf::now() : 0;
            waitWrites(t);
            if (timed)
            {
                perf::upload(perf::UploadRenderWaits, 1);
                perf::upload(perf::UploadRenderWaitUs, perf::now() - start);
            }
        }
    }
}

// ---- Command lists -------------------------------------------------------------------------------------------------

namespace
{

const uint32_t ListMinimumDraws = 96;   // smaller frames are drawn on the immediate context
const uint32_t JobMinimumDraws = 32;    // smallest range a list records
const uint32_t WorkerHostMem = 16 * 1024;
// 80: below the job workers (76), level with guest threads at THREAD_PRIORITY_HIGHEST, above the other guest threads.
const int ListWorkerPriority = Thread::PriorityHigh + 16;

}

// Memory callbacks of the deferred contexts: a whole chunk of the worker's pool, kept until every frame that used it
// is finished. The chunk a context writes into is its pool's current one.
void* FrameRenderer::listMemory(FramePool& pool, SceSize requested, SceSize* size)
{
    uint32_t granted = 0;
    void* memory = pool.take(requested, &granted);
    *size = granted;
    return memory;
}

void* FrameRenderer::vdmMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vdm, requested, size);
}

void* FrameRenderer::vertexMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vertex, requested, size);
}

void* FrameRenderer::fragmentMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->fragment, requested, size);
}

bool FrameRenderer::createWorker(Worker& w)
{
    w.owner = this;
    w.hostMem = malloc(WorkerHostMem);
    SceGxmDeferredContextParams params;
    memset(&params, 0, sizeof(params));
    params.hostMem = w.hostMem;
    params.hostMemSize = WorkerHostMem;
    params.vdmCallback = &vdmMemory;
    params.vertexCallback = &vertexMemory;
    params.fragmentCallback = &fragmentMemory;
    params.callbackData = &w;
    uint32_t size = 0;
    params.vdmRingBufferMem = w.vdm.take(0, &size);
    params.vdmRingBufferMemSize = size;
    params.vertexRingBufferMem = w.vertex.take(0, &size);
    params.vertexRingBufferMemSize = size;
    params.fragmentRingBufferMem = w.fragment.take(0, &size);
    params.fragmentRingBufferMemSize = size;
    const int error = sceGxmCreateDeferredContext(&params, &w.recorder.context);
    if (error < 0)
    {
        logError("gxm: sceGxmCreateDeferredContext failed: 0x%08x", error);
        w.recorder.context = nullptr;
        return false;
    }
    return true;
}

void FrameRenderer::initLists()
{
    if (!Config::getBool("gxm_lists", true))
    {
        logInfo("gxm: command lists off (gxm_lists=off)");
        return;
    }
    // The render thread records too and takes whatever the helpers have not claimed.
    std::vector<int> cores;
    if (cores::core3Waited())
        cores.push_back(3);
    if (Config::getBool("gxm_core2", true))
        cores.push_back(2);
    for (size_t i = 0; i <= cores.size(); ++i)
    {
        Worker* w = new Worker();
        if (!createWorker(*w))
        {
            delete w;
            break;
        }
        w->index = uint8_t(m_workers.size());
        m_workers.push_back(w);
    }
    if (m_workers.empty())
    {
        logError("gxm: command lists off: no deferred context");
        return;
    }
    m_jobLock = new Mutex("gxm_jobs", false);
    m_recordersDone = new CondVar("gxm_recorders_done", *m_jobLock);
    m_wake = new Semaphore("gxm_wake", 0);
    m_jobs.reset(new Job[MaxJobs]);
    static const char* const names[] = { "gxm_list_core3", "gxm_list_core2" };
    for (size_t i = 1; i < m_workers.size(); ++i)
    {
        Worker* w = m_workers[i];
        const int core = cores[i - 1];
        w->core = uint8_t(core);
        w->thread = new Thread(core == 3 ? names[0] : names[1], &workerEntry, w, ListWorkerPriority,
                               64 * 1024, Thread::Cpu0 << core);
        w->thread->start();
    }
    char where[32] = "render core";
    for (size_t i = 1; i < m_workers.size(); ++i)
        snprintf(where + strlen(where), sizeof(where) - strlen(where), ", core %d", cores[i - 1]);
    logInfo("gxm: command lists recorded by %u thread(s): %s", uint32_t(m_workers.size()), where);
}

void FrameRenderer::shutdownLists()
{
    if (m_jobLock)
    {
        m_stop.store(true);
        m_wake->post(int(m_workers.size()));
    }
    for (Worker* w : m_workers)
    {
        if (w->thread)
        {
            w->thread->join();
            delete w->thread;
        }
        if (w->recorder.context)
            sceGxmDestroyDeferredContext(w->recorder.context);
        free(w->hostMem);
        delete w;
    }
    m_workers.clear();
    m_jobs.reset();
    delete m_recordersDone;
    delete m_jobLock;
    delete m_wake;
    m_recordersDone = nullptr;
    m_jobLock = nullptr;
    m_wake = nullptr;
}

int FrameRenderer::workerEntry(void* worker)
{
    Worker& w = *static_cast<Worker*>(worker);
    FrameRenderer& r = *w.owner;
    if (perf::enabled())
        perf::trackThread(w.core == 3 ? "lists_core3" : "lists_core2");
    while (true)
    {
        r.m_wake->wait();
        if (r.m_stop.load())
            break;
        r.runJobs(w);
    }
    return 0;
}

void FrameRenderer::beginWorkerFrame(Worker& w, const render::Frame& frame)
{
    if (w.frame == frame.index)
        return;
    w.frame = frame.index;
    w.recorder.data.begin(frame.index, &finished, this);
    w.vdm.begin(frame.index, &finished, this);
    w.vertex.begin(frame.index, &finished, this);
    w.fragment.begin(frame.index, &finished, this);
    w.recorder.words.reset();
}

// Claims jobs until none is left. A worker woken late finds the current frame's jobs, or none. The helper threads
// count in m_recording from before their first claim until they leave.
void FrameRenderer::runJobs(Worker& w)
{
    const bool counted = w.thread != nullptr;
    if (counted)
        m_recording.fetch_add(1);
    uint32_t claim = m_claim.load(std::memory_order_acquire);
    while (!m_stop.load(std::memory_order_relaxed))
    {
        const uint32_t index = claim & ClaimIndexMask;
        if (index >= ((claim >> ClaimCountShift) & ClaimIndexMask))
            break;
        if (!m_claim.compare_exchange_weak(claim, claim + 1, std::memory_order_acquire, std::memory_order_acquire))
            continue;
        const render::Frame& frame = *m_jobFrame;
        Job& job = m_jobs[index];
        beginWorkerFrame(w, frame);
        SceGxmCommandList list;
        const bool ok = recordJob(w, frame, job, &list);
        finishJob(job, 0, ok, list);
        claim = m_claim.load(std::memory_order_acquire);
    }
    if (counted)
        leaveRecording();
}

bool FrameRenderer::finishJob(Job& job, uint32_t recording, bool ok, const SceGxmCommandList& list)
{
    job.lists[recording] = list;
    job.ok[recording] = ok;
    int8_t none = -1;
    if (!job.used.compare_exchange_strong(none, int8_t(recording), std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
        return false;
    job.abandon.store(true, std::memory_order_relaxed);
    return true;
}

void FrameRenderer::stealJobs(const render::Frame& frame)
{
    Worker& self = *m_workers[0];
    const bool timed = perf::hitch();
    for (uint32_t i = 0; i < m_jobCount; ++i)
    {
        Job& job = m_jobs[i];
        if (job.used.load(std::memory_order_acquire) >= 0)
            continue;
        beginWorkerFrame(self, frame);
        SceGxmCommandList list;
        const bool ok = recordJob(self, frame, job, &list);
        const bool won = finishJob(job, 1, ok, list);
        if (timed)
        {
            perf::renderPart(perf::RenderListSteals, 1);
            if (won)
                perf::renderPart(perf::RenderListStealsWon, 1);
        }
    }
}

void FrameRenderer::leaveRecording()
{
    if (m_recording.fetch_sub(1) == 1 && m_recorderWaiters.load() != 0)
    {
        LockGuard guard(*m_jobLock);
        m_recordersDone->broadcast();
    }
}

void FrameRenderer::waitRecorders()
{
    if (!m_jobLock || m_recording.load() == 0)
        return;
    m_recorderWaiters.fetch_add(1);
    {
        LockGuard guard(*m_jobLock);
        while (m_recording.load() != 0)
            m_recordersDone->wait();
    }
    m_recorderWaiters.fetch_sub(1);
}

bool FrameRenderer::recordJob(Worker& w, const render::Frame& frame, const Job& job, SceGxmCommandList* list)
{
    SceGxmContext* context = w.recorder.context;
    const ScenePlan& p = m_plan[job.scene];
    int error = sceGxmBeginCommandList(context);
    if (error < 0)
    {
        logError("gxm: sceGxmBeginCommandList failed: 0x%08x", error);
        return false;
    }
    // A list starts from a reset GPU state.
    sceGxmSetDefaultRegionClipAndViewport(context, p.width - 1, p.height - 1);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
    sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
    w.recorder.state.reset(p.scaleX, p.scaleY);
    if (!w.recorder.precomputed)
    {
        sceGxmSetPrecomputedVertexState(context, nullptr);
        sceGxmSetPrecomputedFragmentState(context, nullptr);
    }
    for (uint32_t n = job.begin; n < job.end; ++n)
    {
        // Every 8 draws: another recorder finished the job, this list is not used.
        if (((n - job.begin) & 7) == 7 && job.abandon.load(std::memory_order_relaxed))
            break;
        if (n + 2 < job.end)
            prefetchDraw(&frame.draws[m_order[n + 2]]);
        draw(w.recorder, frame, m_order[n]);
    }
    error = sceGxmEndCommandList(context, list);
    if (error < 0)
    {
        logError("gxm: sceGxmEndCommandList failed: 0x%08x", error);
        return false;
    }
    return true;
}

// Splits the frame's scenes into jobs and records them on the workers and this thread; false: draw immediately.
// The jobs are rewritten while none is claimable: every job of the last call was claimed, and waitRecorders let the
// helpers that recorded them leave.
bool FrameRenderer::recordLists(const render::Frame& frame)
{
    if (m_workers.empty() || m_order.size() < ListMinimumDraws)
        return false;
    const uint32_t threads = uint32_t(m_workers.size());
    uint32_t count = 0;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        ScenePlan& p = m_plan[i];
        p.firstJob = count;
        if (p.skipped || !p.orderCount)
            continue;
        // About two ranges per thread.
        const uint32_t size = std::max(JobMinimumDraws, (p.orderCount + 2 * threads - 1) / (2 * threads));
        for (uint32_t begin = 0; begin < p.orderCount; begin += size)
        {
            if (count == MaxJobs)
            {
                static bool s_logged = false;
                if (!s_logged)
                    logInfo("gxm: more than %u command list jobs in a frame: drawn immediately", unsigned(MaxJobs));
                s_logged = true;
                return false;
            }
            Job& job = m_jobs[count++];
            job.scene = i;
            job.begin = p.firstOrder + begin;
            job.end = p.firstOrder + std::min(p.orderCount, begin + size);
            job.ok[0] = job.ok[1] = false;
            job.used.store(-1, std::memory_order_relaxed);
            job.abandon.store(false, std::memory_order_relaxed);
        }
        p.jobCount = count - p.firstJob;
    }
    m_jobCount = count;
    m_jobFrame = &frame;
    ++m_jobGeneration;
    m_claim.store((m_jobGeneration << ClaimGenerationShift) | (count << ClaimCountShift));
    if (threads > 1)
        m_wake->post(int(threads - 1));
    runJobs(*m_workers[0]);
    // A worker that stalls in its job (another thread runs on its core) is not waited for: this thread records the
    // job too, and the first to finish gives the job its list.
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
    stealJobs(frame);
    if (timed)
        perf::renderPart(perf::RenderListWait, perf::now() - start);
    return true;
}

}}
