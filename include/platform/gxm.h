#ifndef PLATFORM_GXM_H_
#define PLATFORM_GXM_H_

// The display device: the game window's front-buffer image (565 pixels) scaled to the display through a gamma
// table, and the Direct3D frames of platform/render.h. Everything in this header is platform neutral.

#include <platform/render.h>
#include <cstdint>

namespace pf { namespace gxm {

enum
{
    DisplayWidth    = 960,
    DisplayHeight   = 544,
    MaxWindowWidth  = 1024,     // largest game window
    MaxWindowHeight = 768
};

struct Rect
{
    int x0, y0, x1, y1;     // [x0, x1) x [y0, y1); empty when x0 >= x1 or y0 >= y1

    bool empty() const { return x0 >= x1 || y0 >= y1; }
    void unite(const Rect& r);
};

class Device
{
public:
    static Device& get();

    bool init();
    void shutdown();

    // Writes rows of 565 pixels (stride in pixels) into the front image of a width x height window over the
    // rectangle area and shows the result at the next vblank. A new window size clears the image. Any thread;
    // calls are serialised by the caller. Never waits for the GPU.
    void writeFront(const uint16_t* pixels, uint32_t stride, uint32_t width, uint32_t height, const Rect& area);

    // Everything shown from now on is colour^(1/gamma).
    void setGamma(float gamma);
    // Display rectangle a width x height window is shown in: the whole display for 960x544 or with the setting
    // fit_4_3=stretch (default), else the largest centred rectangle of the window's aspect (fit_4_3=pillarbox: 4:3
    // windows are pillarboxed).
    static Rect windowRect(uint32_t width, uint32_t height);

    // ---- Direct3D frames and resources: game thread only ----

    // A frame slot to record a frame with a width x height back buffer into. Waits while every slot is in flight;
    // the caller releases the execution lock around the call.
    render::Frame* beginFrame(uint32_t width, uint32_t height);
    // Queues the recorded frame: drawn on the render thread and shown vblanks vblanks after the previous image
    // (1: 60 Hz, 2: 30 Hz), in the window rectangle of its back buffer. Never waits.
    void submitFrame(render::Frame* frame, uint32_t vblanks);
    // Index of the newest frame the GPU has finished; every older frame is finished too.
    uint32_t completedFrame();
    bool busy(const render::Resource& resource) { return resource.lastUse > completedFrame(); }

    // Null when out of memory. Target textures are rendered to and sampled; the others are written by
    // writeTexture. Contents start undefined.
    render::TextureStorage* createTexture(const render::TextureDesc& desc);
    // One level of one face (0 unless a cube) in the Direct3D layout (rows of pitch bytes), to or from the
    // texture. Writing a busy texture changes what frames in flight show.
    void writeTexture(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data,
                      uint32_t pitch);
    void readTexture(const render::TextureStorage* texture, uint32_t face, uint32_t level, void* data,
                     uint32_t pitch);
    // Every level and face of a texture of the same description.
    void copyTexture(render::TextureStorage* to, const render::TextureStorage* from);
    // The same write and copy on a worker thread; the result is the upload's ticket (0: done at once). Uploads
    // touching one texture run in the order they were queued, and the direct calls above wait for them. data must
    // stay unchanged until uploadDone(ticket). TextureStorage::uploads counts a texture's queued writes; frames
    // recorded while it is not 0 set Frame::uploads, and the render thread waits for them before drawing.
    uint32_t writeTextureAsync(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data,
                               uint32_t pitch);
    uint32_t copyTextureAsync(render::TextureStorage* to, const render::TextureStorage* from);
    bool uploadDone(uint32_t ticket);
    void waitUpload(uint32_t ticket);
    render::DepthStorage* createDepth(uint32_t width, uint32_t height);
    // Vertex or index data; BufferStorage::data is written directly.
    render::BufferStorage* createBuffer(uint32_t size);
    // Freed once the frames that may still read the resource and its queued uploads are finished.
    void release(render::TextureStorage* texture);
    void release(render::DepthStorage* depth);
    void release(render::BufferStorage* buffer);

private:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    struct Impl;
    Impl*       m_impl;
};

}}

#endif
