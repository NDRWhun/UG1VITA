#ifndef PLATFORM_GXM_RENDER_H_
#define PLATFORM_GXM_RENDER_H_

// Direct3D frames on SceGxm (platform/render.h): GPU memory, resources, frame slots (game thread) and the
// drawing of a frame's scenes (render thread). Used by gxm_device.cpp.

#include <platform/render.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace pf { namespace gxm {

// Sub-allocates GPU-mapped memory blocks (first fit, coalescing free list); game thread only.
class GpuHeap
{
public:
    GpuHeap(const char* name, SceKernelMemBlockType type, uint32_t chunkSize, uint32_t attributes);
    ~GpuHeap();
    void* allocate(uint32_t size, uint32_t alignment);   // null when no block can be added
    void free(void* pointer);

private:
    struct Chunk
    {
        SceUID                          uid;
        uint8_t*                        base;
        uint32_t                        size;
        std::map<uint32_t, uint32_t>    holes;      // offset -> size
    };
    bool addChunk(uint32_t minimum);

    const char*                         m_name;
    SceKernelMemBlockType               m_type;
    uint32_t                            m_chunkSize;
    uint32_t                            m_attributes;
    std::vector<Chunk*>                 m_chunks;
    std::unordered_map<void*, std::pair<Chunk*, uint32_t>> m_used;   // pointer -> chunk, size
};

struct Texture : render::TextureStorage
{
    void*               data;
    uint32_t            bytes;
    uint32_t            faceStride;
    uint32_t            stride;         // targets: row bytes of a 2D target
    SceGxmTexture       texture;
    SceGxmColorSurface  surfaces[6];    // targets: per face
};

struct Depth : render::DepthStorage
{
    void*                       data;
    void*                       stencil;
    SceGxmDepthStencilSurface   surface;
};

struct Buffer : render::BufferStorage
{
};

class FrameRenderer
{
public:
    enum
    {
        Slots = 3
    };

    bool init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications);
    void shutdown();

    // Game thread.
    render::Frame* beginFrame(uint32_t width, uint32_t height);
    // The slot of a frame begun; the device queues it for the render thread.
    void submitted(render::Frame* frame, uint32_t vblanks);
    uint32_t completedFrame();
    render::TextureStorage* createTexture(const render::TextureDesc& desc);
    void writeTexture(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data, uint32_t pitch);
    void readTexture(const render::TextureStorage* texture, uint32_t face, uint32_t level, void* data, uint32_t pitch);
    void copyTexture(render::TextureStorage* to, const render::TextureStorage* from);
    render::DepthStorage* createDepth(uint32_t width, uint32_t height);
    render::BufferStorage* createBuffer(uint32_t size);
    void release(render::TextureStorage* texture);
    void release(render::DepthStorage* depth);
    void release(render::BufferStorage* buffer);

    // Render thread. Back buffer scenes go to the display surface when the frame's back buffer has the display
    // size, else to the slot's offscreen target. presentScene(): the caller must add a scene that shows the
    // frame (the offscreen target scaled, or black when nothing drew the display); that scene then writes the
    // done notification, else the frame's last scene does.
    bool presentScene(const render::Frame& frame) const;
    const SceGxmTexture* offscreen(const render::Frame& frame) const;
    void drawFrame(const render::Frame& frame, const SceGxmColorSurface* display, SceGxmSyncObject* displaySync);
    SceGxmNotification* doneNotification(const render::Frame& frame);
    uint32_t vblanks(const render::Frame& frame) const;

private:
    struct Slot
    {
        render::Frame                   frame;
        std::vector<render::Scene>      scenes;
        std::vector<render::DrawCall>   draws;
        std::vector<render::Viewport>   viewports;
        std::vector<float>              constants;
        void*                           vertices = nullptr;
        SceGxmNotification              done = {};
        bool                            inFlight = false;
        uint32_t                        vblanks = 2;
        Texture*                        offscreen = nullptr;    // back buffer when not the display size
    };
    struct Pending
    {
        int         kind;           // 0 texture, 1 depth, 2 buffer
        void*       object;
        uint32_t    after;          // freed once this frame is finished
    };
    struct VertexProgram
    {
        SceGxmShaderPatcherId           id = nullptr;
        const SceGxmProgram*            program = nullptr;
        std::vector<const SceGxmProgramParameter*> fields;
    };
    struct FragmentProgram
    {
        SceGxmShaderPatcherId           id = nullptr;
        const SceGxmProgram*            program = nullptr;
        std::vector<const SceGxmProgramParameter*> fields;
        unsigned int                    units[render::MaxSamplers] = {};
        bool                            owned = false;      // registered here (not a copy of another entry)
    };
    enum
    {
        Variants = 1 + render::FactorCount      // plain, alpha test, the blend factor outputs
    };

    void freePending();
    void destroy(int kind, void* object);
    SceGxmVertexProgram* vertexProgram(uint8_t program, uint8_t layout);
    SceGxmFragmentProgram* fragmentProgram(const render::DrawState& state);
    SceGxmRenderTarget* renderTarget(uint32_t width, uint32_t height);
    void draw(const render::Frame& frame, const render::DrawCall& draw);
    void upload(void* buffer, const render::UniformField* fields, uint8_t count,
                const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants);

    SceGxmContext*          m_context = nullptr;
    SceGxmShaderPatcher*    m_patcher = nullptr;
    GpuHeap*                m_textureHeap = nullptr;
    GpuHeap*                m_bufferHeap = nullptr;
    Slot                    m_slots[Slots];
    uint32_t                m_lastIndex = 0;
    uint32_t                m_completed = 0;
    std::vector<Pending>    m_pending;
    SceUID                  m_sequenceUid = -1;
    uint16_t*               m_sequence = nullptr;       // 0..65535
    SceUID                  m_arenaUid = -1;
    Texture*                m_black = nullptr;          // read by samplers without a texture

    VertexProgram           m_vertexPrograms[render::VpCount];
    FragmentProgram         m_fragmentPrograms[render::FpCount][Variants];
    std::unordered_map<uint32_t, SceGxmVertexProgram*>     m_vertexCache;
    std::unordered_map<uint64_t, SceGxmFragmentProgram*>   m_fragmentCache;
    std::map<uint32_t, SceGxmRenderTarget*>                 m_targets;

    // Render thread: the GPU state of the current scene.
    render::DrawState       m_last;
    bool                    m_lastValid = false;
    uint32_t                m_lastVertexConstants = 0, m_lastFragmentConstants = 0;
    uint16_t                m_lastViewport = 0xffff;
};

}}

#endif
