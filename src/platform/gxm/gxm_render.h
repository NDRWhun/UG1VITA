#ifndef PLATFORM_GXM_RENDER_H_
#define PLATFORM_GXM_RENDER_H_

// Direct3D frames on SceGxm (platform/render.h): GPU memory, resources, frame slots (game thread) and the
// drawing of a frame's scenes (render thread). Used by gxm_device.cpp.

#include <platform/render.h>
#include <platform/sync.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <atomic>
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

// GPU-read memory (MAIN_NC) for what frames in flight read: chunks are reused once every frame that used them is
// finished. One thread uses a pool.
class FramePool
{
public:
    typedef bool (*Finished)(void* context, uint32_t frameIndex);

    FramePool(const char* name, uint32_t chunkSize) : m_name(name), m_chunkSize(chunkSize) {}
    ~FramePool();
    // The frame the next allocations belong to; the current chunk is read by it too.
    void begin(uint32_t frameIndex, Finished finished, void* context);
    void* allocate(uint32_t size, uint32_t alignment);
    // A whole free chunk of at least minimum bytes, which becomes the current one.
    void* take(uint32_t minimum, uint32_t* size);

private:
    struct Chunk
    {
        SceUID      uid;
        uint8_t*    base;
        uint32_t    size, used, lastUse;
    };
    int freeChunk(uint32_t minimum);

    const char*         m_name;
    uint32_t            m_chunkSize;
    std::vector<Chunk>  m_chunks;
    int                 m_current = -1;
    uint32_t            m_frame = 0;
    Finished            m_finished = nullptr;
    void*               m_context = nullptr;
};

// Cached host memory for the CPU-side words of precomputed objects; valid until reset (the next frame).
class HostArena
{
public:
    ~HostArena();
    void reset() { m_block = 0; m_used = 0; }
    void* allocate(uint32_t size);

private:
    enum { BlockSize = 64 * 1024 };
    std::vector<uint8_t*>   m_blocks;
    size_t                  m_block = 0;
    uint32_t                m_used = 0;
};

struct Texture : render::TextureStorage
{
    void*               data;
    uint32_t            bytes;
    uint32_t            faceStride;
    uint32_t            stride;         // targets: row bytes of a 2D target
    SceGxmTexture       texture;
    SceGxmColorSurface  surfaces[6];    // targets: per face
    uint32_t            lastTicket = 0; // game thread: the last queued upload that writes or reads it
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
    uint32_t lastIndex() const { return m_lastIndex; }
    // Program list file: the vertex programs (with their layouts) and fragment programs created, so that a later run
    // creates them before the game draws. Load: before the first frame, while no other thread draws or registers
    // layouts; the number created. Save: after the render thread has stopped.
    uint32_t loadPrograms(const char* path);
    void savePrograms(const char* path) const;
    render::TextureStorage* createTexture(const render::TextureDesc& desc);
    void writeTexture(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data, uint32_t pitch);
    void readTexture(const render::TextureStorage* texture, uint32_t face, uint32_t level, void* data, uint32_t pitch);
    void copyTexture(render::TextureStorage* to, const render::TextureStorage* from);
    // The same write and copy on a job worker; the result is the upload's ticket. Uploads that touch one texture
    // run in the order they were queued; data must stay unchanged until uploadDone(ticket).
    uint32_t writeTextureAsync(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data,
                               uint32_t pitch);
    uint32_t copyTextureAsync(render::TextureStorage* to, const render::TextureStorage* from);
    render::DepthStorage* createDepth(uint32_t width, uint32_t height);
    render::BufferStorage* createBuffer(uint32_t size);
    void release(render::TextureStorage* texture);
    void release(render::DepthStorage* depth);
    void release(render::BufferStorage* buffer);

    // Any thread. Ticket 0 is always done.
    bool uploadDone(uint32_t ticket);
    void waitUpload(uint32_t ticket);
    // Until the queued writes into the texture are finished.
    void waitWrites(const render::TextureStorage* texture);

    // Render thread. Back buffer scenes go to the display surface when the frame's back buffer has the display
    // size, or when stretchDirect (the back buffer fills the display, no gamma) and none of them uses depth;
    // else to the slot's offscreen target. Shown::presentScene: the caller must add a scene that shows the frame
    // (the offscreen target scaled, or black when nothing drew the display); that scene then writes Shown::done,
    // else the frame's last scene does. Shown is read before the frame's scenes are submitted: once the last one
    // writes the done notification, the game thread may reuse the slot and its frame.
    struct Shown
    {
        bool                presentScene;
        bool                offscreen;      // texture holds the offscreen back buffer
        SceGxmTexture       texture;
        SceGxmNotification* done;
        uint32_t            vblanks;
    };
    Shown drawFrame(const render::Frame& frame, const SceGxmColorSurface* display, SceGxmSyncObject* displaySync,
                    bool stretchDirect);

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
        bool                            direct = false;         // back buffer scenes drawn to the display
    };
    // Per scene of the frame being drawn (render thread).
    struct ScenePlan
    {
        bool        depthUsed;      // cleared or tested by a draw
        bool        skipped;        // only colour clears: drawn at the start of scene carriedTo instead
        uint32_t    carriedTo;
        // Where it draws.
        const SceGxmColorSurface*   color;
        SceGxmSyncObject*           sync;
        uint32_t                    width, height;
        float                       scaleX, scaleY;
        uint32_t                    firstOrder, orderCount;     // its draws in m_order
        uint32_t                    firstJob, jobCount;
    };
    void planFrame(const render::Frame& frame, bool stretchDirect);
    // After planFrame.
    bool presentScene(const render::Frame& frame) const;
    const SceGxmTexture* offscreen(const render::Frame& frame) const;
    // Moves the alpha-tested draws of a scene's order after the opaque draws that follow them up to the next draw
    // that is not depth-tested LESS/LESSEQUAL with depth write, full colour mask and no blend (setting alpha_draw_order).
    void sortScene(const render::Frame& frame, uint32_t scene);
    // timing_log: HSR flush transitions (alpha-tested to opaque or blended draw) before and after sortScene.
    struct SortStats
    {
        uint32_t    frames = 0;
        uint64_t    draws = 0, discards = 0, before = 0, after = 0;     // largest scene of each frame
        uint64_t    allBefore = 0, allAfter = 0;                        // every scene
        uint32_t    largest = 0, largestDiscards = 0, largestBefore = 0, largestAfter = 0;  // this frame
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
        uint32_t                        uniformBytes = 0;   // default uniform buffer
    };
    struct FragmentProgram
    {
        SceGxmShaderPatcherId           id = nullptr;
        const SceGxmProgram*            program = nullptr;
        std::vector<const SceGxmProgramParameter*> fields;
        uint32_t                        uniformBytes = 0;
        unsigned int                    units[render::MaxSamplers] = {};
        bool                            owned = false;      // registered here (not a copy of another entry)
    };
    enum
    {
        Variants = 1 + render::FactorCount      // plain, alpha test, the blend factor outputs
    };

    // A patched program and what its precomputed objects need.
    struct VertexEntry
    {
        SceGxmVertexProgram*    program = nullptr;
        uint8_t                 source = 0;         // render::VertexProgram
        uint32_t                drawSize = 0, stateSize = 0, uniformBytes = 0;
    };
    struct FragmentEntry
    {
        SceGxmFragmentProgram*  program = nullptr;
        uint32_t                stateSize = 0, uniformBytes = 0;
    };
    struct DrawPrograms
    {
        const VertexEntry*      vertex;
        const FragmentEntry*    fragment;
    };
    // What a context was last given within a scene or command list.
    struct ContextState
    {
        render::DrawState       last;
        bool                    valid = false;
        uint32_t                vertexConstants = 0, fragmentConstants = 0;
        uint16_t                viewport = 0xffff;
        const VertexEntry*      vertex = nullptr;
        const FragmentEntry*    fragment = nullptr;
        float                   scaleX = 1.0f, scaleY = 1.0f;   // back buffer to display, this scene

        void reset(float x, float y) { valid = false; viewport = 0xffff; scaleX = x; scaleY = y; }
    };
    // A thread that records draws: its context and the memory of its precomputed objects.
    struct Recorder
    {
        SceGxmContext*          context = nullptr;
        ContextState            state;
        bool                    precomputed = false;    // m_precomputed at the start of the frame
        FramePool               data{"gxm_draws", 512 * 1024};
        HostArena               words;
    };
    // A thread recording command lists: a deferred context and the pools its memory callbacks draw from.
    struct Worker
    {
        FrameRenderer*          owner = nullptr;
        Recorder                recorder;
        FramePool               vdm{"gxm_list_vdm", 64 * 1024};
        FramePool               vertex{"gxm_list_vertex", 256 * 1024};
        FramePool               fragment{"gxm_list_fragment", 64 * 1024};
        void*                   hostMem = nullptr;
        uint32_t                frame = 0;      // frame its pools were begun for
        Thread*                 thread = nullptr;
        uint8_t                 index = 0;      // in m_workers
        std::atomic<bool>       abandon{false}; // the job it records was finished by another recorder: stop
    };
    enum JobState : uint8_t
    {
        JobFree,
        JobClaimed,     // a worker records it
        JobDone
    };
    // A range of a scene's draws recorded into one command list.
    struct Job
    {
        uint32_t                scene, begin, end;  // m_order range
        bool                    ok;
        SceGxmCommandList       list;
        JobState                state;
        uint8_t                 worker;             // the claiming worker
    };
    enum : uint32_t
    {
        NoJob = 0xffffffffu
    };

    // A queued write (from null) or copy.
    struct Upload
    {
        FrameRenderer*      owner;
        uint32_t            ticket, after;      // after: the upload it waits for, 0 for none
        Texture*            to;
        const Texture*      from;
        uint32_t            face, level, pitch;
        const void*         data;
    };
    uint32_t queueUpload(Upload* upload);
    static void runUpload(void* upload);
    // Game thread: before the CPU reads or writes a texture directly.
    void settle(const Texture* texture);
    // Render thread: before the frame's scenes are drawn.
    void waitFrameUploads(const render::Frame& frame);
    void freePending();
    void destroy(int kind, void* object);
    const VertexEntry* vertexProgram(uint8_t program, uint8_t layout);
    const FragmentEntry* fragmentProgram(const render::DrawState& state);
    // Creates the fragment program of a cache key (fragmentKey) and adds it to the cache.
    const FragmentEntry* createFragment(uint64_t key);
    SceGxmRenderTarget* renderTarget(uint32_t width, uint32_t height);
    static bool finished(void* self, uint32_t frameIndex);
    // Patched programs of every draw (render thread; creates them on first use).
    void preparePrograms(const render::Frame& frame);
    void draw(Recorder& recorder, const render::Frame& frame, uint32_t index);
    void drawPrecomputed(Recorder& recorder, const render::Frame& frame, uint32_t index);
    void setFixedState(ContextState& c, SceGxmContext* context, const render::Frame& frame, const render::DrawCall& d,
                       bool first);
    SceGxmTexture samplerTexture(const render::DrawState& s, uint32_t i) const;
    void upload(void* buffer, const render::UniformField* fields, uint8_t count,
                const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants);
    // Command lists (setting gxm_lists): workers on the render core, core 3 (core3_tasks=helpers) and core 2 record the
    // frame's scenes in ranges; the immediate context executes the lists in order.
    void initLists();
    void shutdownLists();
    bool createWorker(Worker& worker);
    static int workerEntry(void* worker);
    static void* listMemory(FramePool& pool, SceSize requested, SceSize* size);
    static void* vdmMemory(void* worker, SceSize requested, SceSize* size);
    static void* vertexMemory(void* worker, SceSize requested, SceSize* size);
    static void* fragmentMemory(void* worker, SceSize requested, SceSize* size);
    bool recordLists(const render::Frame& frame);
    void runJobs(Worker& worker);
    // Records a job into list; stops early (the list is not used) when the worker's abandon flag is set.
    bool recordJob(Worker& worker, const render::Frame& frame, const Job& job, SceGxmCommandList* list);
    void beginWorkerFrame(Worker& worker, const render::Frame& frame);
    // A recorder ended job index of generation: the first to end it gives the job its list.
    void finishJob(Worker& worker, uint32_t generation, uint32_t index, bool ok, const SceGxmCommandList& list);
    // Render thread: records claimed jobs the workers have not finished, until every job is done.
    void stealJobs(const render::Frame& frame);
    // Waits until no recorder reads a frame's draws any more.
    void waitRecorders();

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
    uint32_t                m_nextTicket = 1;
    Mutex*                  m_uploadLock = nullptr;
    CondVar*                m_uploadDone = nullptr;
    std::vector<uint32_t>   m_uploading;                // tickets queued and not finished, ascending (m_uploadLock)
    std::atomic<uint32_t>   m_firstUploading{0xffffffffu};  // m_uploading.front(), all ones when empty

    VertexProgram           m_vertexPrograms[render::VpCount];
    FragmentProgram         m_fragmentPrograms[render::FpCount][Variants];
    std::unordered_map<uint32_t, VertexEntry>              m_vertexCache;
    std::unordered_map<uint64_t, FragmentEntry>            m_fragmentCache;
    bool                    m_loadingPrograms = false;  // loadPrograms runs: creations are not render thread work
    std::map<uint32_t, SceGxmRenderTarget*>                 m_targets;

    // Render thread (and workers, read only while they record).
    std::atomic<bool>           m_precomputed{true};    // precomputed states and draws (setting gxm_precomputed)
    bool                        m_cullSwap = false;     // D3DCULL_CW drawn as SCE_GXM_CULL_CCW and back (cull_swap)
    Recorder                    m_main;                 // the immediate context
    std::vector<DrawPrograms>   m_programs;             // per draw of the frame being drawn
    std::vector<ScenePlan>      m_plan;
    std::vector<uint32_t>       m_order;                // draw indices of the scenes, carried clears first
    bool                        m_drawSort = true;      // setting alpha_draw_order
    std::vector<uint32_t>       m_held;                 // sortScene: alpha-tested draws not placed yet
    SortStats                   m_sortStats;

    // Command lists. m_workers[0] is the render thread's own deferred context.
    std::vector<Worker*>        m_workers;
    Mutex*                      m_jobLock = nullptr;
    CondVar*                    m_jobsDone = nullptr;
    Semaphore*                  m_wake = nullptr;
    std::vector<Job>            m_jobs;                 // guarded by m_jobLock while workers run
    const render::Frame*        m_jobFrame = nullptr;
    uint32_t                    m_nextJob = 0, m_doneJobs = 0;
    uint32_t                    m_jobGeneration = 0;    // counts recordLists calls
    uint32_t                    m_recording = 0;        // recorders inside recordJob
    uint32_t                    m_stealing = NoJob;     // the job the render thread records for a worker
    bool                        m_stop = false;
};

}}

#endif
