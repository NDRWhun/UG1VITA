#include "movie_yuv.h"

#include <cstring>

namespace nfsu
{
namespace movie
{

namespace
{

inline int16_t word(const uint8_t* table, uint32_t index, uint32_t lane)
{
    int16_t value;
    memcpy(&value, table + index * 8 + lane * 2, sizeof(value));
    return value;
}

inline uint8_t saturate(int16_t value)
{
    return value < 0 ? 0 : value > 255 ? 255 : uint8_t(value);
}

inline uint32_t load32(const uint8_t* base, uint32_t address)
{
    uint32_t value;
    memcpy(&value, base + address, sizeof(value));
    return value;
}

}

void convertRow(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint8_t* dst, uint32_t pixels,
                const uint8_t* tableY, const uint8_t* tableU, const uint8_t* tableV)
{
    for (uint32_t i = 0; i < pixels / 2; ++i)
    {
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            const int16_t chroma = int16_t(word(tableU, u[i], lane) + word(tableV, v[i], lane));
            dst[lane] = saturate(int16_t(word(tableY, y[0], lane) + chroma));
            dst[4 + lane] = saturate(int16_t(word(tableY, y[1], lane) + chroma));
        }
        y += 2;
        dst += 8;
    }
}

void setTableState(uint8_t* base, const Tables& tables, uint32_t state)
{
    if (load32(base, tables.swapped) == state)
        return;
    const uint32_t all[] = {tables.y, tables.u, tables.v};
    for (uint32_t table : all)
    {
        for (uint32_t i = 0; i < 255; ++i)
        {
            uint8_t* entry = base + table + i * 8;
            uint8_t first[2];
            memcpy(first, entry, 2);
            memmove(entry, entry + 2, 2);
            memcpy(entry + 2, first, 2);
        }
    }
    memcpy(base + tables.swapped, &state, sizeof(state));
}

uint32_t convertFrame(uint8_t* base, const Tables& tables, uint32_t y, uint32_t u, uint32_t v, int32_t yStride,
                      int32_t width, int32_t height, uint32_t dst, int32_t dstPitch)
{
    // The checks and their order are the guest's (0x647CC6..0x647DB6, mode 1).
    if (yStride & 1)
        return 2;
    if (dstPitch & 7)
        return 3;
    if (dst & 7)
    {
        if (dst & 3)
            return 4;
        dst += 4;
        width -= 1;
    }
    width &= ~1;
    if (width < 2 || height < 1)
        return 0;
    setTableState(base, tables, 0);
    const int32_t chromaStride = yStride >> 1;
    for (int32_t row = 0; row < height; ++row)
    {
        convertRow(base + y, base + u, base + v, base + dst, uint32_t(width), base + tables.y, base + tables.u,
                   base + tables.v);
        y += yStride;
        if (row & 1)
        {
            u += chromaStride;
            v += chromaStride;
        }
        dst += dstPitch;
    }
    return 0;
}

}
}
