#include <platform/texture.h>
#include <cstring>

namespace pf { namespace texture {

uint32_t levelBytes(Format format, uint32_t width, uint32_t height, uint32_t level)
{
    return levelUnits(format, width, level) * levelUnits(format, height, level) * unitBytes(format);
}

uint32_t levelOffset(Format format, uint32_t width, uint32_t height, uint32_t level)
{
    uint32_t offset = 0;
    for (uint32_t l = 0; l < level; ++l)
        offset += levelBytes(format, width, height, l);
    return offset;
}

uint32_t chainBytes(Format format, uint32_t width, uint32_t height, uint32_t levels)
{
    return levelOffset(format, width, height, levels);
}

// Measured on psp2gxt output: faces of mipmapped cubes larger than 512 bytes start on 2 KB boundaries, other
// faces follow each other directly.
uint32_t faceStride(Format format, uint32_t width, uint32_t height, uint32_t levels)
{
    const uint32_t bytes = chainBytes(format, width, height, levels);
    return (levels > 1 && bytes > 512) ? (bytes + 2047) & ~2047u : bytes;
}

uint32_t totalBytes(Format format, uint32_t width, uint32_t height, uint32_t levels, bool cube)
{
    return cube ? 5 * faceStride(format, width, height, levels) + chainBytes(format, width, height, levels)
                : chainBytes(format, width, height, levels);
}

uint32_t levelPitch(Format format, uint32_t width, uint32_t level)
{
    return levelUnits(format, width, level) * unitBytes(format);
}

namespace
{

// Spreads the low 16 bits of v to the even bits.
inline uint32_t spread(uint32_t v)
{
    v &= 0xffff;
    v = (v | (v << 8)) & 0x00ff00ff;
    v = (v | (v << 4)) & 0x0f0f0f0f;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

inline uint32_t log2(uint32_t v)
{
    uint32_t n = 0;
    while ((1u << n) < v)
        ++n;
    return n;
}

}

uint32_t swizzleIndex(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t minSize = w < h ? w : h;
    const uint32_t k = log2(minSize);
    const uint32_t mask = minSize - 1;
    const uint32_t tile = w > h ? x >> k : y >> k;
    return (tile << (2 * k)) | spread(y & mask) | (spread(x & mask) << 1);
}

void swizzleLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, uint32_t pitch,
                  void* dst)
{
    const uint32_t w = levelUnits(format, width, level), h = levelUnits(format, height, level);
    const uint32_t unit = unitBytes(format);
    const uint8_t* in = static_cast<const uint8_t*>(src);
    uint8_t* out = static_cast<uint8_t*>(dst);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* row = in + size_t(y) * pitch;
        for (uint32_t x = 0; x < w; ++x)
            memcpy(out + size_t(swizzleIndex(x, y, w, h)) * unit, row + size_t(x) * unit, unit);
    }
}

void unswizzleLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, void* dst,
                    uint32_t pitch)
{
    const uint32_t w = levelUnits(format, width, level), h = levelUnits(format, height, level);
    const uint32_t unit = unitBytes(format);
    const uint8_t* in = static_cast<const uint8_t*>(src);
    uint8_t* out = static_cast<uint8_t*>(dst);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* row = out + size_t(y) * pitch;
        for (uint32_t x = 0; x < w; ++x)
            memcpy(row + size_t(x) * unit, in + size_t(swizzleIndex(x, y, w, h)) * unit, unit);
    }
}

namespace
{

inline uint32_t rgb565(uint16_t c)
{
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3) | (r >> 2)) | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2)) << 16;
}

inline uint32_t mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t d)
{
    uint32_t out = 0;
    for (uint32_t shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * wa + ((b >> shift) & 255) * wb) / d) << shift;
    return out;
}

// Colours of a BC1 colour block (alpha set); 3-colour mode with transparent black only when allowed.
void blockColors(const uint8_t* block, bool threeColor, uint32_t colors[4])
{
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    const uint32_t a = rgb565(c0), b = rgb565(c1);
    colors[0] = a | 0xff000000u;
    colors[1] = b | 0xff000000u;
    if (c0 > c1 || !threeColor)
    {
        colors[2] = mix(a, b, 2, 1, 3) | 0xff000000u;
        colors[3] = mix(a, b, 1, 2, 3) | 0xff000000u;
    }
    else
    {
        colors[2] = mix(a, b, 1, 1, 2) | 0xff000000u;
        colors[3] = 0;
    }
}

}

void decodeLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, uint32_t pitch,
                 uint32_t* rgba)
{
    const uint32_t w = levelDim(width, level), h = levelDim(height, level);
    const uint8_t* in = static_cast<const uint8_t*>(src);
    if (format == FormatARGB8888)
    {
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t* row = in + size_t(y) * pitch;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = row + x * 4;
                rgba[size_t(y) * w + x] = uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16 | uint32_t(p[3]) << 24;
            }
        }
        return;
    }
    if (!isBlock(format))
        return;
    const uint32_t bw = levelUnits(format, width, level), bh = levelUnits(format, height, level);
    for (uint32_t by = 0; by < bh; ++by)
    {
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t* block = in + size_t(by) * pitch + size_t(bx) * unitBytes(format);
            const uint8_t* color = format == FormatBC2 ? block + 8 : block;
            uint32_t colors[4];
            blockColors(color, format == FormatBC1, colors);
            const uint32_t bits = uint32_t(color[4]) | uint32_t(color[5]) << 8 | uint32_t(color[6]) << 16
                                | uint32_t(color[7]) << 24;
            for (uint32_t py = 0; py < 4; ++py)
            {
                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= w || y >= h)
                        continue;
                    const uint32_t i = py * 4 + px;
                    uint32_t c = colors[(bits >> (2 * i)) & 3];
                    if (format == FormatBC2)
                    {
                        const uint32_t a4 = (block[i / 2] >> ((i & 1) * 4)) & 15;
                        c = (c & 0x00ffffffu) | (a4 * 17) << 24;
                    }
                    rgba[size_t(y) * w + x] = c;
                }
            }
        }
    }
}

}}
