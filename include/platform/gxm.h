#ifndef PLATFORM_GXM_H_
#define PLATFORM_GXM_H_

// The display device: the game window's front-buffer image (565 pixels) scaled to the display through a gamma
// table. Everything in this header is platform neutral; the host harness implements the same Device.

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
    // stretch=1, else the largest centred rectangle of the window's aspect (4:3 windows are pillarboxed).
    static Rect windowRect(uint32_t width, uint32_t height);

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
