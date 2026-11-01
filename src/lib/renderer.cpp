#include <lib/renderer.h>
#include <lib/window.h>
#include <lib/gamepad.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <algorithm>
#include <cstring>

namespace win32
{

Renderer::Renderer(WinApplication* application, Window*)
    :   m_application(application)
    ,   m_videoMemory(new MemMap(pf::gxm::MaxWindowWidth * pf::gxm::MaxWindowHeight * 2 * 2)) // double buffer 16 bits
    ,   m_currentBuffer(0)
    ,   m_width(640)
    ,   m_height(480)
    ,   m_depth(16)
    ,   m_colorPalette()
{
}

Renderer::~Renderer()
{
    delete m_videoMemory;
}

void Renderer::setVideoMode(x86::reg32 w, x86::reg32 h, x86::reg32 bpp)
{
    RC_ASSERT(w <= pf::gxm::MaxWindowWidth && h <= pf::gxm::MaxWindowHeight);
    m_width = w;
    m_height = h;
    m_depth = bpp;
    pf::logInfo("video mode %ux%u %u bpp", w, h, bpp);
}

void Renderer::updatePalette(x86::reg32 colorCount, const x86::reg32* colors)
{
    RC_ASSERT(colorCount <= 256);
    memcpy(m_colorPalette, colors, colorCount*4);
}

x86::reg32 Renderer::getFrontBuffer() const
{
    return m_videoMemory->getBlockStart() + m_currentBuffer * m_width * m_height * 2;
}

x86::reg32 Renderer::getBackBuffer() const
{
    return m_videoMemory->getBlockStart() + (1 - m_currentBuffer) * m_width * m_height * 2;
}

// Shows the guest front buffer through the device's front-buffer layer (565; 8-bit pages through the palette).
void Renderer::update()
{
    const x86::reg8* source = &m_application->getMemory<x86::reg8>(getFrontBuffer());
    const uint32_t width = std::min<uint32_t>(m_width, pf::gxm::MaxWindowWidth);
    const uint32_t height = std::min<uint32_t>(m_height, pf::gxm::MaxWindowHeight);
    m_pixels.resize(size_t(width) * height);
    if (m_depth == 16)
    {
        memcpy(m_pixels.data(), source, m_pixels.size() * 2);
    }
    else
    {
        // Palette entries arrive as R<<24|G<<16|B<<8|A.
        for (size_t i = 0; i < m_pixels.size(); ++i)
        {
            const x86::reg32 c = m_colorPalette[source[i]];
            m_pixels[i] = uint16_t((c >> 16 & 0xf800) | (c >> 13 & 0x07e0) | (c >> 11 & 0x001f));
        }
    }
    pf::gxm::Device::get().writeFront(m_pixels.data(), width, width, height,
                                      pf::gxm::Rect{ 0, 0, int(width), int(height) });
    Gamepad::updateKeys();
}

x86::reg32 Renderer::lock(x86::reg32 index)
{
    return index == 0 ? getFrontBuffer() : getBackBuffer();
}

void Renderer::unlock(x86::reg32 index)
{
    if (index == 0)
        update();
}

void Renderer::swap()
{
    m_currentBuffer = 1 - m_currentBuffer;
    update();
}

}
