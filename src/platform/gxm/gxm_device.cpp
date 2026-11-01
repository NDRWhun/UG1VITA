#include <platform/cores.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/ime.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include "gxm_render.h"
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Embedded shader binaries (generated from shaders/*.cg, cmake/shaders.cmake).
extern "C" {
extern const unsigned char shader_blit_v[];
extern const unsigned char shader_blit_f[];
extern const unsigned char shader_clear_v[];
extern const unsigned char shader_clear_f[];
}

namespace pf { namespace gxm {

void Rect::unite(const Rect& r)
{
    if (r.empty())
        return;
    if (empty())
    {
        *this = r;
        return;
    }
    x0 = r.x0 < x0 ? r.x0 : x0;
    y0 = r.y0 < y0 ? r.y0 : y0;
    x1 = r.x1 > x1 ? r.x1 : x1;
    y1 = r.y1 > y1 ? r.y1 : y1;
}

namespace
{

const uint32_t DisplayBuffers       = 3;
const uint32_t DisplayStride        = DisplayWidth;
const uint32_t WindowStride         = MaxWindowWidth;      // pixels per row of the front image
const uint32_t PatcherBufferSize    = 64 * 1024;
const uint32_t PatcherUsseSize      = 256 * 1024;
const uint32_t QuadRingVertices     = 4096;                 // present quads, reused in a ring
const uint32_t LayerBuffers         = 2;
const uint32_t DialogRefreshMs      = 50;   // longest wait for a game frame while a system dialog is up
const uint32_t GpuDoneGapUs         = 1000; // display callback started this long after the previous one returned
const SceUID   InvalidUid           = -1;

struct DisplayData
{
    void*       address;
    uint32_t    vblanks;
    uint32_t    frameIndex;     // game image shown by this display buffer; 0: a repeat
    uint32_t    endSceneTime;   // perf::now() after the image's sceGxmEndScene
};

struct QuadVertex
{
    float x, y;
    float u, v;
};

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void* memBlockBase(SceUID uid)
{
    void* mem = nullptr;
    const int result = sceKernelGetMemBlockBase(uid, &mem);
    if (result < 0)
        fatal("sceKernelGetMemBlockBase failed: 0x%08x", result);
    return mem;
}

void* gpuAlloc(SceKernelMemBlockType type, uint32_t size, uint32_t alignment, uint32_t attribs, SceUID* uid)
{
    const uint32_t granularity = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) ? 256 * 1024 : 4096;
    size = alignUp(size, granularity);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    *uid = sceKernelAllocMemBlock("gxm_gpu", type, size, alignment > granularity ? &opt : nullptr);
    if (*uid < 0)
        fatal("sceKernelAllocMemBlock(%u bytes, type 0x%08x) failed: 0x%08x", size, type, *uid);
    void* mem = memBlockBase(*uid);
    int result = sceGxmMapMemory(mem, size, SceGxmMemoryAttribFlags(attribs));
    if (result < 0)
        fatal("sceGxmMapMemory failed: 0x%08x", result);
    return mem;
}

void gpuFree(SceUID uid)
{
    if (uid < 0)
        return;
    void* mem = nullptr;
    sceKernelGetMemBlockBase(uid, &mem);
    sceGxmUnmapMemory(mem);
    sceKernelFreeMemBlock(uid);
}

void* gpuUsseAlloc(uint32_t size, SceUID* uid, unsigned int* offset, bool vertex)
{
    size = alignUp(size, 4096);
    *uid = sceKernelAllocMemBlock(vertex ? "gxm_vusse" : "gxm_fusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size,
                                  nullptr);
    if (*uid < 0)
        fatal("sceKernelAllocMemBlock(%u bytes, USSE) failed: 0x%08x", size, *uid);
    void* mem = memBlockBase(*uid);
    const int result = vertex ? sceGxmMapVertexUsseMemory(mem, size, offset)
                              : sceGxmMapFragmentUsseMemory(mem, size, offset);
    if (result < 0)
        fatal("sceGxmMap%sUsseMemory failed: 0x%08x", vertex ? "Vertex" : "Fragment", result);
    return mem;
}

void gpuUsseFree(SceUID uid, bool vertex)
{
    if (uid < 0)
        return;
    void* mem = nullptr;
    sceKernelGetMemBlockBase(uid, &mem);
    if (vertex)
        sceGxmUnmapVertexUsseMemory(mem);
    else
        sceGxmUnmapFragmentUsseMemory(mem);
    sceKernelFreeMemBlock(uid);
}

void* patcherHostAlloc(void*, SceSize size)   { return malloc(size); }
void  patcherHostFree(void*, void* mem)       { free(mem); }

// Display queue thread only: the last two callbacks' end times, the last start, and whether that start was
// the GPU's completion time of its frame.
uint32_t s_lastCallbackStart = 0;
uint32_t s_lastCallbackEnd = 0;
uint32_t s_secondLastCallbackEnd = 0;
bool     s_lastStartWasGpuDone = false;

uint32_t later(uint32_t a, uint32_t b)
{
    return int32_t(a - b) > 0 ? a : b;
}

void displayCallback(const void* data)
{
    const DisplayData* display = static_cast<const DisplayData*>(data);
    const bool perfOn = perf::enabled();
    const bool timed = display->frameIndex && perfOn;
    const uint32_t start = perfOn ? perf::now() : 0;
    // The callback runs once the frame is rendered and the previous callback returned. Starting clearly after
    // that return, it starts at the GPU's completion of the frame.
    const bool startIsGpuDone = perfOn && int32_t(start - s_lastCallbackEnd) > int32_t(GpuDoneGapUs);
    if (timed)
    {
        perf::add(perf::GpuLatency, start - display->endSceneTime);
        if (startIsGpuDone && s_lastStartWasGpuDone)
        {
            const uint32_t gpuStart = later(later(display->endSceneTime, s_lastCallbackStart), s_secondLastCallbackEnd);
            perf::gpuBusy(start - gpuStart);
        }
    }
    SceDisplayFrameBuf frameBuffer;
    memset(&frameBuffer, 0, sizeof(frameBuffer));
    frameBuffer.size = sizeof(frameBuffer);
    frameBuffer.base = display->address;
    frameBuffer.pitch = DisplayStride;
    frameBuffer.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frameBuffer.width = DisplayWidth;
    frameBuffer.height = DisplayHeight;
    sceDisplaySetFrameBuf(&frameBuffer, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStartMulti(display->vblanks ? display->vblanks : 1);
    if (perfOn)
    {
        const uint32_t end = perf::now();
        if (timed)
            perf::add(perf::FlipWait, end - start);
        s_lastCallbackStart = start;
        s_secondLastCallbackEnd = s_lastCallbackEnd;
        s_lastCallbackEnd = end;
        s_lastStartWasGpuDone = startIsGpuDone;
    }
}

unsigned int resourceIndex(const SceGxmProgram* program, const char* name)
{
    const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(program, name);
    if (!parameter)
        fatal("shader parameter %s not found", name);
    return sceGxmProgramParameterGetResourceIndex(parameter);
}

}

struct Device::Impl
{
    // Core
    SceGxmContext*          context = nullptr;
    void*                   hostMem = nullptr;
    SceUID                  vdmRingUid = InvalidUid;
    SceUID                  vertexRingUid = InvalidUid;
    SceUID                  fragmentRingUid = InvalidUid;
    SceUID                  fragmentUsseRingUid = InvalidUid;
    SceGxmRenderTarget*     displayTarget = nullptr;

    // Display
    void*                   displayData[DisplayBuffers] = {};
    SceUID                  displayUid[DisplayBuffers] = { InvalidUid, InvalidUid, InvalidUid };
    SceGxmColorSurface      displaySurface[DisplayBuffers];
    SceGxmSyncObject*       displaySync[DisplayBuffers] = {};
    uint32_t                backBuffer = 0;
    uint32_t                frontBuffer = DisplayBuffers - 1;

    // Front image: master copy (host, guarded by layerLock) and GPU copies (render thread)
    LwMutex                 layerLock{"gxm_layer"};
    std::vector<uint16_t>   layerMaster;                    // WindowStride x MaxWindowHeight
    uint32_t                layerWidth = 0, layerHeight = 0;
    Rect                    layerStale[LayerBuffers] = {};  // master pixels not yet copied into each GPU copy
    void*                   layerData[LayerBuffers] = {};
    SceUID                  layerUid[LayerBuffers] = { InvalidUid, InvalidUid };
    SceGxmNotification      layerDone[LayerBuffers] = {};
    bool                    layerUsed[LayerBuffers] = {};
    uint32_t                layerNext = 0;

    // Shader patcher and programs
    SceGxmShaderPatcher*    patcher = nullptr;
    SceUID                  patcherBufferUid = InvalidUid;
    SceUID                  patcherVertexUsseUid = InvalidUid;
    SceUID                  patcherFragmentUsseUid = InvalidUid;
    SceGxmShaderPatcherId   blitVId = nullptr, blitFId = nullptr, clearVId = nullptr, clearFId = nullptr;
    SceGxmVertexProgram*    blitV = nullptr;
    SceGxmFragmentProgram*  blitF = nullptr;
    SceGxmVertexProgram*    clearV = nullptr;
    SceGxmFragmentProgram*  clearF = nullptr;
    const SceGxmProgramParameter* blitGammaParam = nullptr;
    const SceGxmProgramParameter* clearColorParam = nullptr;
    unsigned int            blitTextureUnit = 0;

    // Static geometry
    uint16_t*               indices = nullptr;              // 0, 1, 2, 3
    SceUID                  indicesUid = InvalidUid;
    QuadVertex*             quadRing = nullptr;
    SceUID                  quadRingUid = InvalidUid;
    uint32_t                quadNext = 0;

    // The present waiting for the render thread; writeFront grows its area until it is taken.
    struct Present
    {
        uint32_t    width, height;  // window size of the image
        Rect        area;           // valid area of the image
    };
    Mutex                   jobLock{"gxm_jobs", false};
    CondVar                 jobReady{"gxm_ready", jobLock};
    bool                    pending = false;
    Present                 pendingPresent = {};
    Rect                    layerValid = {};            // valid area of the front image of the current window
    std::atomic<uint32_t>   gammaBits{0x3f800000};      // float

    // Direct3D frames: recorded on the game thread, queued (at most one per slot) for the render thread.
    FrameRenderer           frames;
    render::Frame*          frameJobs[FrameRenderer::Slots] = {};
    uint32_t                frameJobFirst = 0, frameJobCount = 0;
    bool                    frameShown = false;         // the display shows a Direct3D frame, not the layer

    // Render thread
    Thread*                 thread = nullptr;
    volatile bool           running = false;
    uint32_t                endSceneTime = 0;
    uint32_t                presentIndex = 0;           // game presents shown so far
    uint32_t                queueUs = 0;                // render thread: display queue wait of the current present
    std::string             programList;                // usePrograms path
    Present                 shown = {};                 // image last presented

    bool initCore();
    bool initSurfaces();
    bool initPrograms();
    bool initBuffers();

    static int threadEntry(void* self);
    void run();
    void present(const Present& image, uint32_t frameIndex);
    void presentFrame(const render::Frame& frame, uint32_t frameIndex);
    void blitPrograms();
    int  updateLayer(uint32_t width, uint32_t height);
    void drawQuad(const Rect& display, float u0, float v0, float u1, float v1, const SceGxmTexture* texture,
                  bool textured);
    void queueDisplay(uint32_t vblanks, uint32_t frameIndex);
};

// ---- Initialisation ------------------------------------------------------------------------------------------------

bool Device::Impl::initCore()
{
    SceGxmInitializeParams init;
    memset(&init, 0, sizeof(init));
    init.flags = 0;
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = &displayCallback;
    init.displayQueueCallbackDataSize = sizeof(DisplayData);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int result = sceGxmInitialize(&init);
    if (result < 0)
    {
        logError("sceGxmInitialize failed: 0x%08x", result);
        return false;
    }

    hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    if (!hostMem)
        fatal("out of memory for the GXM context");
    void* vdmRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &vdmRingUid);
    void* vertexRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &vertexRingUid);
    void* fragmentRing = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &fragmentRingUid);
    unsigned int fragmentUsseOffset = 0;
    void* fragmentUsseRing = gpuUsseAlloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &fragmentUsseRingUid, &fragmentUsseOffset, false);

    SceGxmContextParams params;
    memset(&params, 0, sizeof(params));
    params.hostMem = hostMem;
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = vdmRing;
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = vertexRing;
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = fragmentRing;
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferMem = fragmentUsseRing;
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = fragmentUsseOffset;
    result = sceGxmCreateContext(&params, &context);
    if (result < 0)
    {
        logError("sceGxmCreateContext failed: 0x%08x", result);
        return false;
    }

    SceGxmRenderTargetParams target;
    memset(&target, 0, sizeof(target));
    target.flags = 0;
    target.width = DisplayWidth;
    target.height = DisplayHeight;
    target.scenesPerFrame = 1;
    target.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    target.multisampleLocations = 0;
    target.driverMemBlock = InvalidUid;
    result = sceGxmCreateRenderTarget(&target, &displayTarget);
    if (result < 0)
    {
        logError("sceGxmCreateRenderTarget failed: 0x%08x", result);
        return false;
    }
    return true;
}

bool Device::Impl::initSurfaces()
{
    for (uint32_t i = 0; i < DisplayBuffers; ++i)
    {
        displayData[i] = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DisplayStride * DisplayHeight * 4,
                                  SCE_GXM_COLOR_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW, &displayUid[i]);
        memset(displayData[i], 0, DisplayStride * DisplayHeight * 4);
        int result = sceGxmColorSurfaceInit(&displaySurface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                            SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                            DisplayWidth, DisplayHeight, DisplayStride, displayData[i]);
        if (result < 0)
        {
            logError("sceGxmColorSurfaceInit failed: 0x%08x", result);
            return false;
        }
        result = sceGxmSyncObjectCreate(&displaySync[i]);
        if (result < 0)
        {
            logError("sceGxmSyncObjectCreate failed: 0x%08x", result);
            return false;
        }
    }

    // Front image: 565, read by the present pass.
    layerMaster.assign(WindowStride * MaxWindowHeight, 0);
    for (uint32_t i = 0; i < LayerBuffers; ++i)
    {
        layerData[i] = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, WindowStride * MaxWindowHeight * 2,
                                SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &layerUid[i]);
        memset(layerData[i], 0, WindowStride * MaxWindowHeight * 2);
    }
    return true;
}

bool Device::Impl::initPrograms()
{
    void* patcherBuffer = gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PatcherBufferSize, 4, SCE_GXM_MEMORY_ATTRIB_RW, &patcherBufferUid);
    unsigned int vertexUsseOffset = 0, fragmentUsseOffset = 0;
    void* vertexUsse = gpuUsseAlloc(PatcherUsseSize, &patcherVertexUsseUid, &vertexUsseOffset, true);
    void* fragmentUsse = gpuUsseAlloc(PatcherUsseSize, &patcherFragmentUsseUid, &fragmentUsseOffset, false);

    SceGxmShaderPatcherParams params;
    memset(&params, 0, sizeof(params));
    params.hostAllocCallback = &patcherHostAlloc;
    params.hostFreeCallback = &patcherHostFree;
    params.bufferMem = patcherBuffer;
    params.bufferMemSize = PatcherBufferSize;
    params.vertexUsseMem = vertexUsse;
    params.vertexUsseMemSize = PatcherUsseSize;
    params.vertexUsseOffset = vertexUsseOffset;
    params.fragmentUsseMem = fragmentUsse;
    params.fragmentUsseMemSize = PatcherUsseSize;
    params.fragmentUsseOffset = fragmentUsseOffset;
    int result = sceGxmShaderPatcherCreate(&params, &patcher);
    if (result < 0)
    {
        logError("sceGxmShaderPatcherCreate failed: 0x%08x", result);
        return false;
    }

    auto registerProgram = [this](const unsigned char* data, SceGxmShaderPatcherId* id, const char* name) -> const SceGxmProgram*
    {
        const SceGxmProgram* program = reinterpret_cast<const SceGxmProgram*>(data);
        int r = sceGxmProgramCheck(program);
        if (r < 0)
            fatal("shader %s is invalid: 0x%08x", name, r);
        r = sceGxmShaderPatcherRegisterProgram(patcher, program, id);
        if (r < 0)
            fatal("registering shader %s failed: 0x%08x", name, r);
        return program;
    };
    const SceGxmProgram* blitVProgram = registerProgram(shader_blit_v, &blitVId, "blit_v");
    const SceGxmProgram* blitFProgram = registerProgram(shader_blit_f, &blitFId, "blit_f");
    const SceGxmProgram* clearVProgram = registerProgram(shader_clear_v, &clearVId, "clear_v");
    const SceGxmProgram* clearFProgram = registerProgram(shader_clear_f, &clearFId, "clear_f");

    // Vertex layout mirrors QuadVertex.
    SceGxmVertexAttribute quadAttributes[2];
    quadAttributes[0] = { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(blitVProgram, "aPosition")) };
    quadAttributes[1] = { 0, 8, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(blitVProgram, "aTexCoord")) };
    SceGxmVertexStream quadStream = { sizeof(QuadVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, blitVId, quadAttributes, 2, &quadStream, 1, &blitV);
    if (result < 0)
    {
        logError("blit vertex program creation failed: 0x%08x", result);
        return false;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, blitFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, nullptr, blitVProgram, &blitF);
    if (result < 0)
    {
        logError("blit fragment program creation failed: 0x%08x", result);
        return false;
    }
    blitTextureUnit = resourceIndex(blitFProgram, "uTexture");
    blitGammaParam = sceGxmProgramFindParameterByName(blitFProgram, "uInvGamma");

    SceGxmVertexAttribute clearAttribute = { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, uint16_t(resourceIndex(clearVProgram, "aPosition")) };
    result = sceGxmShaderPatcherCreateVertexProgram(patcher, clearVId, &clearAttribute, 1, &quadStream, 1, &clearV);
    if (result < 0)
    {
        logError("clear vertex program creation failed: 0x%08x", result);
        return false;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(patcher, clearFId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, nullptr, clearVProgram, &clearF);
    if (result < 0)
    {
        logError("clear fragment program creation failed: 0x%08x", result);
        return false;
    }
    clearColorParam = sceGxmProgramFindParameterByName(clearFProgram, "uColor");
    if (!clearColorParam || !blitGammaParam)
    {
        logError("shader uniforms not found");
        return false;
    }
    return true;
}

bool Device::Impl::initBuffers()
{
    indices = static_cast<uint16_t*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(uint16_t) * 4, 4,
                                              SCE_GXM_MEMORY_ATTRIB_READ, &indicesUid));
    for (uint16_t i = 0; i < 4; ++i)
        indices[i] = i;
    quadRing = static_cast<QuadVertex*>(gpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, sizeof(QuadVertex) * QuadRingVertices, 4,
                                                 SCE_GXM_MEMORY_ATTRIB_READ, &quadRingUid));

    // Notification words: one per GPU copy of the front image. Values start at 1, so zero never matches.
    volatile unsigned int* notifications = sceGxmGetNotificationRegion();
    for (uint32_t i = 0; i < LayerBuffers; ++i)
    {
        layerDone[i].address = notifications + i;
        *layerDone[i].address = 0;
        layerDone[i].value = 0;
    }
    return true;
}

// ---- Render thread -------------------------------------------------------------------------------------------------

// The calling thread's CPU time in microseconds.
static uint64_t threadClocks()
{
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    return sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 ? 0 : info.runClocks;
}

int Device::Impl::threadEntry(void* self)
{
    if (perf::enabled())
        perf::trackThread("render");
    static_cast<Impl*>(self)->run();
    return 0;
}

void Device::Impl::run()
{
    while (true)
    {
        Present image;
        bool queued = false;
        render::Frame* frame = nullptr;
        {
            LockGuard guard(jobLock);
            while (running && !pending && !frameJobCount)
            {
                if (ime::pending() && !frameShown)
                {
                    // A system dialog needs frames even while the game presents none.
                    if (!jobReady.waitFor(DialogRefreshMs) && !pending)
                        break;
                }
                else
                {
                    jobReady.wait();
                }
            }
            if (!running)
                break;
            if (frameJobCount)
            {
                // Direct3D frames first, in order; a layer present waits for them.
                frame = frameJobs[frameJobFirst];
                frameJobFirst = (frameJobFirst + 1) % FrameRenderer::Slots;
                --frameJobCount;
                queued = true;
            }
            else
            {
                if (!running)
                    break;
                queued = pending;
                image = queued ? pendingPresent : shown;
                pending = false;
            }
        }

        const bool timed = perf::timed() && queued;
        const uint32_t start = timed ? perf::now() : 0;
        const uint32_t index = frame ? frame->index : 0;
        queueUs = 0;
        const bool parts = timed && perf::hitch();
        const uint64_t cpuStart = parts ? threadClocks() : 0;
        if (frame && frame->resolve)
        {
            frame->resolve(*frame);
            if (parts)
                perf::renderPart(perf::RenderResolve, perf::now() - start);
        }
        uint32_t calls = 0, triangles = 0;
        if (frame && timed && perf::enabled())
        {
            // Counted before the frame is drawn: its slot may be reused once the GPU is done.
            calls = frame->drawCount;
            for (uint32_t i = 0; i < frame->drawCount; ++i)
            {
                const render::DrawCall& d = frame->draws[i];
                triangles += d.state.primitive == render::PrimitiveTriangles ? d.count / 3 : d.count > 2 ? d.count - 2 : 0;
            }
        }
        if (frame)
            presentFrame(*frame, ++presentIndex);
        else
            present(image, queued ? ++presentIndex : 0);
        if (parts)
            perf::renderPart(perf::RenderCpu, uint32_t(threadClocks() - cpuStart));
        if (timed)
        {
            const uint32_t renderUs = perf::now() - start;
            perf::add(perf::Render, renderUs);
            perf::framePresented(index, renderUs, queueUs, calls, triangles);
        }
        else if (queued && perf::frameObserved())
        {
            perf::framePresented(index, 0, 0, 0, 0);
        }
    }
}

// Next 4 vertices of the quad ring for a display rectangle.
static void quad(QuadVertex* v, const Rect& r, float u0, float v0, float u1, float v1)
{
    const float x0 = float(r.x0) * 2.0f / DisplayWidth - 1.0f;
    const float x1 = float(r.x1) * 2.0f / DisplayWidth - 1.0f;
    const float y0 = 1.0f - float(r.y0) * 2.0f / DisplayHeight;
    const float y1 = 1.0f - float(r.y1) * 2.0f / DisplayHeight;
    v[0] = { x0, y0, u0, v0 };
    v[1] = { x1, y0, u1, v0 };
    v[2] = { x0, y1, u0, v1 };
    v[3] = { x1, y1, u1, v1 };
}

void Device::Impl::drawQuad(const Rect& display, float u0, float v0, float u1, float v1, const SceGxmTexture* texture,
                            bool textured)
{
    if (quadNext + 4 > QuadRingVertices)
        quadNext = 0;
    QuadVertex* v = quadRing + quadNext;
    quadNext += 4;
    quad(v, display, u0, v0, u1, v1);
    if (textured)
        sceGxmSetFragmentTexture(context, blitTextureUnit, texture);
    sceGxmSetVertexStream(context, 0, v);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
}

// Copies the stale part of the master image into the next GPU copy; returns its index.
int Device::Impl::updateLayer(uint32_t width, uint32_t height)
{
    const int index = int(layerNext);
    layerNext = (layerNext + 1) % LayerBuffers;
    if (layerUsed[index])
        sceGxmNotificationWait(&layerDone[index]);
    uint16_t* out = static_cast<uint16_t*>(layerData[index]);
    LwLockGuard guard(layerLock);
    Rect& stale = layerStale[index];
    if (!stale.empty())
    {
        const int x0 = stale.x0, x1 = stale.x1 < int(width) ? stale.x1 : int(width);
        const int y1 = stale.y1 < int(height) ? stale.y1 : int(height);
        for (int y = stale.y0; y < y1 && x0 < x1; ++y)
            memcpy(out + y * WindowStride + x0, &layerMaster[y * WindowStride + x0], size_t(x1 - x0) * 2);
        stale = Rect{};
    }
    return index;
}

// Shows the valid area of the front image in its window rectangle, through the gamma table; the rest is black.
void Device::Impl::present(const Present& image, uint32_t frameIndex)
{
    frameShown = false;
    const bool showLayer = !image.area.empty() && image.width && image.height;
    const int layerBuffer = showLayer ? updateLayer(image.width, image.height) : -1;
    const Rect box = Device::windowRect(image.width, image.height);
    const bool fullImage = showLayer && image.area.x0 <= 0 && image.area.y0 <= 0
                        && image.area.x1 >= int(image.width) && image.area.y1 >= int(image.height);

    sceGxmBeginScene(context, 0, displayTarget, nullptr, nullptr, displaySync[backBuffer], &displaySurface[backBuffer], nullptr);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(context, 0.5f * DisplayWidth, 0.5f * DisplayWidth, 0.5f * DisplayHeight, -0.5f * DisplayHeight, 0.0f, 1.0f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);

    if (!fullImage || box.x0 > 0 || box.y0 > 0)
    {
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        sceGxmSetVertexProgram(context, clearV);
        sceGxmSetFragmentProgram(context, clearF);
        void* uniforms = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearColorParam, 0, 4, black);
        if (!fullImage)
        {
            drawQuad(Rect{ 0, 0, DisplayWidth, DisplayHeight }, 0, 0, 0, 0, nullptr, false);
        }
        else
        {
            const Rect bars[2] = { box.x0 > 0 ? Rect{ 0, 0, box.x0, DisplayHeight } : Rect{ 0, 0, DisplayWidth, box.y0 },
                                   box.x0 > 0 ? Rect{ box.x1, 0, DisplayWidth, DisplayHeight } : Rect{ 0, box.y1, DisplayWidth, DisplayHeight } };
            for (const Rect& bar : bars)
                drawQuad(bar, 0, 0, 0, 0, nullptr, false);
        }
    }

    if (showLayer)
    {
        sceGxmSetVertexProgram(context, blitV);
        sceGxmSetFragmentProgram(context, blitF);
        {
            uint32_t bits = gammaBits.load();
            float gamma;
            memcpy(&gamma, &bits, sizeof(gamma));
            const float invGamma = gamma > 0.0f ? 1.0f / gamma : 1.0f;
            void* uniforms = nullptr;
            sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
            sceGxmSetUniformDataF(uniforms, blitGammaParam, 0, 1, &invGamma);
        }
        // Scaled windows are filtered; a window shown 1:1 (960x544) is copied texel for texel.
        const SceGxmTextureFilter filter = (box.x1 - box.x0 == int(image.width) && box.y1 - box.y0 == int(image.height))
                                         ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
        SceGxmTexture texture;
        sceGxmTextureInitLinearStrided(&texture, layerData[layerBuffer], SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB,
                                       image.width, image.height, WindowStride * 2);
        sceGxmTextureSetMinFilter(&texture, filter);
        sceGxmTextureSetMagFilter(&texture, filter);
        const Rect& layer = image.area;
        const float sx = float(box.x1 - box.x0) / float(image.width);
        const float sy = float(box.y1 - box.y0) / float(image.height);
        const Rect area = { box.x0 + int(float(layer.x0) * sx + 0.5f), box.y0 + int(float(layer.y0) * sy + 0.5f),
                            box.x0 + int(float(layer.x1) * sx + 0.5f), box.y0 + int(float(layer.y1) * sy + 0.5f) };
        drawQuad(area, float(layer.x0) / float(image.width), float(layer.y0) / float(image.height),
                 float(layer.x1) / float(image.width), float(layer.y1) / float(image.height), &texture, true);

        layerUsed[layerBuffer] = true;
        layerDone[layerBuffer].value++;
        sceGxmEndScene(context, nullptr, &layerDone[layerBuffer]);
    }
    else
    {
        sceGxmEndScene(context, nullptr, nullptr);
    }
    if (perf::enabled())
        endSceneTime = perf::now();
    shown = image;

    // A running system dialog is drawn over the finished image.
    if (ime::prepareFrame())
        ime::drawFrame(displayData[backBuffer], DisplayWidth, DisplayHeight, DisplayStride, displaySync[backBuffer]);
    queueDisplay(1, frameIndex);
}

void Device::Impl::blitPrograms()
{
    sceGxmSetVertexProgram(context, blitV);
    sceGxmSetFragmentProgram(context, blitF);
    uint32_t bits = gammaBits.load();
    float gamma;
    memcpy(&gamma, &bits, sizeof(gamma));
    const float invGamma = gamma > 0.0f ? 1.0f / gamma : 1.0f;
    void* uniforms = nullptr;
    sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, blitGammaParam, 0, 1, &invGamma);
}

// A frame with a scene that clears or tests depth (3D).
static bool usesDepth(const render::Frame& frame)
{
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        if (!scene.target.depth)
            continue;
        if (scene.depthCleared)
            return true;
        for (uint32_t n = 0; n < scene.drawCount; ++n)
            if (frame.draws[scene.firstDraw + n].state.depthEnable)
                return true;
    }
    return false;
}

// A Direct3D frame: its scenes, then (for a back buffer drawn offscreen, or when nothing drew the back buffer) a scene
// that shows the offscreen back buffer in its window rectangle or black.
void Device::Impl::presentFrame(const render::Frame& frame, uint32_t frameIndex)
{
    frameShown = true;
    // A stretched back buffer without gamma can be drawn to the display directly.
    const Rect box = Device::windowRect(frame.width, frame.height);
    const bool stretched = box.x0 == 0 && box.y0 == 0 && box.x1 == DisplayWidth && box.y1 == DisplayHeight;
    // A frame at the display's size is drawn into the display buffer, which keeps what an earlier frame left. The
    // game's frames without depth (movies, loading and other 2D-only screens) cover the 4:3 part only: the display
    // buffer is cleared black first.
    if (frame.width == DisplayWidth && frame.height == DisplayHeight && !usesDepth(frame))
    {
        sceGxmBeginScene(context, 0, displayTarget, nullptr, nullptr, displaySync[backBuffer], &displaySurface[backBuffer],
                         nullptr);
        sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
        sceGxmSetViewport(context, 0.5f * DisplayWidth, 0.5f * DisplayWidth, 0.5f * DisplayHeight, -0.5f * DisplayHeight,
                          0.0f, 1.0f);
        sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
        sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
        sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        sceGxmSetVertexProgram(context, clearV);
        sceGxmSetFragmentProgram(context, clearF);
        void* uniforms = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearColorParam, 0, 4, black);
        drawQuad(Rect{ 0, 0, DisplayWidth, DisplayHeight }, 0, 0, 0, 0, nullptr, false);
        sceGxmEndScene(context, nullptr, nullptr);
    }
    // The frame is not read after drawFrame: its last scene may let the game thread reuse it.
    const FrameRenderer::Shown drawn =
        frames.drawFrame(frame, &displaySurface[backBuffer], displaySync[backBuffer], stretched && gammaBits.load() == 0x3f800000);
    if (drawn.presentScene)
    {
        sceGxmBeginScene(context, 0, displayTarget, nullptr, nullptr, displaySync[backBuffer], &displaySurface[backBuffer],
                         nullptr);
        sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
        sceGxmSetViewport(context, 0.5f * DisplayWidth, 0.5f * DisplayWidth, 0.5f * DisplayHeight, -0.5f * DisplayHeight,
                          0.0f, 1.0f);
        sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, DisplayWidth - 1, DisplayHeight - 1);
        sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
        sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        sceGxmSetVertexProgram(context, clearV);
        sceGxmSetFragmentProgram(context, clearF);
        void* uniforms = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, clearColorParam, 0, 4, black);
        drawQuad(Rect{ 0, 0, DisplayWidth, DisplayHeight }, 0, 0, 0, 0, nullptr, false);
        if (drawn.offscreen)
        {
            blitPrograms();
            SceGxmTexture texture = drawn.texture;
            sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
            drawQuad(box, 0, 0, 1, 1, &texture, true);
        }
        sceGxmEndScene(context, nullptr, drawn.done);
    }
    if (perf::enabled())
        endSceneTime = perf::now();
    shown = Present{};
    if (ime::prepareFrame())
        ime::drawFrame(displayData[backBuffer], DisplayWidth, DisplayHeight, DisplayStride, displaySync[backBuffer]);
    queueDisplay(drawn.vblanks, frameIndex);
}

void Device::Impl::queueDisplay(uint32_t vblanks, uint32_t frameIndex)
{
    sceGxmPadHeartbeat(&displaySurface[backBuffer], displaySync[backBuffer]);
    DisplayData data;
    data.address = displayData[backBuffer];
    data.vblanks = vblanks;
    data.frameIndex = frameIndex;
    data.endSceneTime = endSceneTime;
    const bool timed = frameIndex && perf::timed();
    const uint32_t start = timed ? perf::now() : 0;
    const int result = sceGxmDisplayQueueAddEntry(displaySync[frontBuffer], displaySync[backBuffer], &data);
    if (result < 0)
        fatal("sceGxmDisplayQueueAddEntry failed: 0x%08x", result);
    if (timed)
    {
        queueUs = perf::now() - start;
        perf::add(perf::QueueWait, queueUs);
    }
    frontBuffer = backBuffer;
    backBuffer = (backBuffer + 1) % DisplayBuffers;
}

// ---- Device --------------------------------------------------------------------------------------------------------

Device& Device::get()
{
    static Device device;
    return device;
}

Device::Device()
    :   m_impl(nullptr)
{
}

Device::~Device()
{
}

bool Device::init()
{
    if (m_impl)
        return true;
    m_impl = new Impl();
    if (!m_impl->initCore() || !m_impl->initSurfaces() || !m_impl->initPrograms() || !m_impl->initBuffers()
        || !m_impl->frames.init(m_impl->context, m_impl->patcher, sceGxmGetNotificationRegion()))
    {
        shutdown();
        return false;
    }

    m_impl->running = true;
    m_impl->thread = new Thread("render", &Impl::threadEntry, m_impl, Thread::PriorityHigh, 256 * 1024,
                                cores::renderMask());
    m_impl->thread->start();
    logInfo("gxm device ready: %dx%d", DisplayWidth, DisplayHeight);
    return true;
}

void Device::shutdown()
{
    if (!m_impl)
        return;
    Impl& impl = *m_impl;
    if (impl.thread)
    {
        {
            LockGuard guard(impl.jobLock);
            impl.running = false;
            impl.jobReady.signal();
        }
        impl.thread->join();
        delete impl.thread;
        impl.thread = nullptr;
    }
    ime::shutdown();
    if (impl.context)
    {
        sceGxmFinish(impl.context);
        sceGxmDisplayQueueFinish();
    }
    if (impl.patcher)
    {
        if (!impl.programList.empty())
            impl.frames.savePrograms(impl.programList.c_str());
        impl.frames.shutdown();
        if (impl.blitV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.blitV);
        if (impl.blitF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.blitF);
        if (impl.clearV) sceGxmShaderPatcherReleaseVertexProgram(impl.patcher, impl.clearV);
        if (impl.clearF) sceGxmShaderPatcherReleaseFragmentProgram(impl.patcher, impl.clearF);
        SceGxmShaderPatcherId ids[] = { impl.blitVId, impl.blitFId, impl.clearVId, impl.clearFId };
        for (SceGxmShaderPatcherId id : ids)
            if (id) sceGxmShaderPatcherUnregisterProgram(impl.patcher, id);
        sceGxmShaderPatcherDestroy(impl.patcher);
    }
    gpuUsseFree(impl.patcherVertexUsseUid, true);
    gpuUsseFree(impl.patcherFragmentUsseUid, false);
    gpuFree(impl.patcherBufferUid);
    gpuFree(impl.indicesUid);
    gpuFree(impl.quadRingUid);
    for (uint32_t i = 0; i < DisplayBuffers; ++i)
    {
        if (impl.displaySync[i]) sceGxmSyncObjectDestroy(impl.displaySync[i]);
        gpuFree(impl.displayUid[i]);
    }
    for (SceUID uid : impl.layerUid)
        gpuFree(uid);
    if (impl.displayTarget) sceGxmDestroyRenderTarget(impl.displayTarget);
    if (impl.context) sceGxmDestroyContext(impl.context);
    gpuUsseFree(impl.fragmentUsseRingUid, false);
    gpuFree(impl.fragmentRingUid);
    gpuFree(impl.vertexRingUid);
    gpuFree(impl.vdmRingUid);
    free(impl.hostMem);
    sceGxmTerminate();
    delete m_impl;
    m_impl = nullptr;
}

void Device::writeFront(const uint16_t* pixels, uint32_t stride, uint32_t width, uint32_t height, const Rect& area)
{
    Impl& impl = *m_impl;
    width = std::min<uint32_t>(width, MaxWindowWidth);
    height = std::min<uint32_t>(height, MaxWindowHeight);
    Rect r = { area.x0 > 0 ? area.x0 : 0, area.y0 > 0 ? area.y0 : 0,
               area.x1 < int(width) ? area.x1 : int(width), area.y1 < int(height) ? area.y1 : int(height) };
    if (r.empty())
        return;
    bool newWindow = false;
    {
        LwLockGuard guard(impl.layerLock);
        if (width != impl.layerWidth || height != impl.layerHeight)
        {
            // A new window: the old pixels are gone.
            impl.layerWidth = width;
            impl.layerHeight = height;
            std::fill(impl.layerMaster.begin(), impl.layerMaster.end(), 0);
            newWindow = true;
        }
        for (int y = r.y0; y < r.y1; ++y)
            memcpy(&impl.layerMaster[y * WindowStride + r.x0], pixels + y * stride + r.x0, size_t(r.x1 - r.x0) * 2);
        for (Rect& stale : impl.layerStale)
            stale.unite(r);
    }
    LockGuard guard(impl.jobLock);
    if (newWindow)
        impl.layerValid = Rect{};
    impl.layerValid.unite(r);
    impl.pendingPresent = Impl::Present{ width, height, impl.layerValid };
    impl.pending = true;
    impl.jobReady.signal();
}

void Device::setGamma(float gamma)
{
    uint32_t bits;
    memcpy(&bits, &gamma, sizeof(bits));
    m_impl->gammaBits.store(bits);
}

Rect Device::windowRect(uint32_t width, uint32_t height)
{
    static const char* const Fits[] = { "stretch", "pillarbox" };
    static const bool stretch = Config::getChoice("fit_4_3", Fits, 2, 0) == 0;
    if (stretch || !width || !height || (width == DisplayWidth && height == DisplayHeight))
        return Rect{ 0, 0, DisplayWidth, DisplayHeight };
    if (width * DisplayHeight > height * DisplayWidth)
    {
        const int h = int(DisplayWidth * height / width) & ~1;
        const int y = (DisplayHeight - h) / 2;
        return Rect{ 0, y, DisplayWidth, y + h };
    }
    const int w = int(DisplayHeight * width / height) & ~1;
    const int x = (DisplayWidth - w) / 2;
    return Rect{ x, 0, x + w, DisplayHeight };
}

// ---- Direct3D frames -----------------------------------------------------------------------------------------------

render::Frame* Device::beginFrame(uint32_t width, uint32_t height)
{
    return m_impl->frames.beginFrame(width, height);
}

void Device::submitFrame(render::Frame* frame, uint32_t vblanks)
{
    Impl& impl = *m_impl;
    impl.frames.submitted(frame, vblanks);
    LockGuard guard(impl.jobLock);
    // beginFrame waited for the slot, so the queue has room.
    impl.frameJobs[(impl.frameJobFirst + impl.frameJobCount) % FrameRenderer::Slots] = frame;
    ++impl.frameJobCount;
    impl.jobReady.signal();
}

uint32_t Device::completedFrame()
{
    return m_impl->frames.completedFrame();
}

uint32_t Device::lastBegunFrame() const
{
    return m_impl->frames.lastIndex();
}

void Device::usePrograms(const char* path)
{
    Impl& impl = *m_impl;
    impl.programList = path;
    const uint32_t start = perf::now();
    const uint32_t created = impl.frames.loadPrograms(path);
    if (created)
        logInfo("gxm: %u programs created from %s in %.1f ms", unsigned(created), path,
                double(perf::now() - start) / 1000.0);
}

render::TextureStorage* Device::createTexture(const render::TextureDesc& desc)
{
    return m_impl->frames.createTexture(desc);
}

void Device::writeTexture(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data,
                          uint32_t pitch)
{
    m_impl->frames.writeTexture(texture, face, level, data, pitch);
}

void Device::readTexture(const render::TextureStorage* texture, uint32_t face, uint32_t level, void* data,
                         uint32_t pitch)
{
    m_impl->frames.readTexture(texture, face, level, data, pitch);
}

void Device::copyTexture(render::TextureStorage* to, const render::TextureStorage* from)
{
    m_impl->frames.copyTexture(to, from);
}

uint32_t Device::writeTextureAsync(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data,
                                   uint32_t pitch)
{
    return m_impl->frames.writeTextureAsync(texture, face, level, data, pitch);
}

uint32_t Device::copyTextureAsync(render::TextureStorage* to, const render::TextureStorage* from)
{
    return m_impl->frames.copyTextureAsync(to, from);
}

bool Device::uploadDone(uint32_t ticket)
{
    return m_impl->frames.uploadDone(ticket);
}

void Device::waitUpload(uint32_t ticket)
{
    m_impl->frames.waitUpload(ticket);
}

render::DepthStorage* Device::createDepth(uint32_t width, uint32_t height)
{
    return m_impl->frames.createDepth(width, height);
}

render::BufferStorage* Device::createBuffer(uint32_t size)
{
    return m_impl->frames.createBuffer(size);
}

void Device::release(render::TextureStorage* texture)
{
    m_impl->frames.release(texture);
}

void Device::release(render::DepthStorage* depth)
{
    m_impl->frames.release(depth);
}

void Device::release(render::BufferStorage* buffer)
{
    m_impl->frames.release(buffer);
}

}}
