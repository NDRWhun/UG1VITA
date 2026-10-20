#ifndef NFSU_MOVIE_YUV_H_
#define NFSU_MOVIE_YUV_H_

// The movie player's YUV420 -> 32-bit converter (guest 0x647CB0) in plain C++, for mode 1 (A8R8G8B8). The guest
// has only MMX kernels; this computes the same bytes: for each pixel the words Y[y] + U[u] + V[v] of the colour
// tables are added with 16-bit wrap-around (paddw) and saturated to 0..255 (packuswb), giving B, G, R, A.

#include <cstdint>

namespace nfsu
{
namespace movie
{

// Guest addresses of the colour tables: 256 entries of four 16-bit words each, and the swap state.
struct Tables
{
    uint32_t y, u, v;
    uint32_t swapped;   // dword: the state the swap routine (0x648517) last left the tables in
};

// Mode 1 of the converter on guest memory at base: planes y, u, v (chroma at half resolution, row stride
// yStride / 2), width x height pixels to dst with dstPitch bytes per row. Returns the guest's result code (0 done,
// 2 odd stride, 3 pitch not a multiple of 8, 4 dst not 4-aligned). Leaves the tables in state 0 like the guest.
uint32_t convertFrame(uint8_t* base, const Tables& tables, uint32_t y, uint32_t u, uint32_t v, int32_t yStride,
                      int32_t width, int32_t height, uint32_t dst, int32_t dstPitch);

// One row like the MMX kernel 0x648208: pixels (even) pixels from y, pixels / 2 chroma samples from u and v.
void convertRow(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint8_t* dst, uint32_t pixels,
                const uint8_t* tableY, const uint8_t* tableU, const uint8_t* tableV);

// The swap routine 0x648517 to the given state: exchanges words 0 and 1 of table entries 0..254 (the guest
// loop stops before entry 255) when the state differs.
void setTableState(uint8_t* base, const Tables& tables, uint32_t state);

}
}

#endif /* !NFSU_MOVIE_YUV_H_ */
