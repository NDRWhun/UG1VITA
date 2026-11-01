#ifndef LIB_RENDERER_H_
#define LIB_RENDERER_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <lib/memmap.h>
#include <vector>

namespace win32
{

class Window;

// Guest video memory (two DirectDraw pages, 8 or 16 bits) shown through the gxm device's front-buffer layer.
class Renderer: public GenericResource
{
public:
    Renderer(WinApplication* application, Window* window);
    ~Renderer();

    x86::reg32 lock(x86::reg32 index);
    void unlock(x86::reg32 index);

    void swap();
    void setVideoMode(x86::reg32 w, x86::reg32 h, x86::reg32 bpp);
    void updatePalette(x86::reg32 colorCount, const x86::reg32* colors);
    x86::reg32 getVideoMemorySize() const { return m_videoMemory->getBlockSize(); }

    x86::reg32 getWidth() const     { return m_width; }
    x86::reg32 getHeight() const    { return m_height; }

private:
    void update();
    x86::reg32 getFrontBuffer() const;
    x86::reg32 getBackBuffer() const;

private:
    WinApplication*         m_application;
    MemMap*                 m_videoMemory;
    x86::reg32              m_currentBuffer;
    x86::reg32              m_width;
    x86::reg32              m_height;
    x86::reg32              m_depth;
    x86::reg32              m_colorPalette[256];
    std::vector<uint16_t>   m_pixels;       // the front page as 565
};

}

#endif
