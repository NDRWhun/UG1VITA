#ifndef PLATFORM_TEXTURE_H_
#define PLATFORM_TEXTURE_H_

// Texture formats of the Direct3D layer and their GPU layout.
//
// Source data is Direct3D's: levels of rows of 4x4 blocks (BC1/BC2) or of 32-bit pixels (ARGB8888, bytes B, G, R,
// A), each level with its own pitch. The GPU layout is the swizzled layout of SceGxm textures (the same bytes
// psp2gxt writes): every level in Morton order, y in the lowest bit, square min x min tiles one after the other
// along the longer side; block formats are swizzled in units of blocks, every level at least one block; levels
// follow each other without padding. Cube faces follow each other at faceStride(). Only power-of-two sizes.

#include <cstdint>

namespace pf { namespace texture {

enum Format : uint8_t
{
    FormatNone = 0,
    FormatBC1,          // DXT1, 8 bytes per 4x4 block
    FormatBC2,          // DXT3, 16 bytes per 4x4 block
    FormatARGB8888      // A8R8G8B8, 4 bytes per pixel
};

inline bool isBlock(Format format)            { return format == FormatBC1 || format == FormatBC2; }
// Bytes of one block (4x4 pixels) or of one pixel.
inline uint32_t unitBytes(Format format)      { return format == FormatBC1 ? 8 : format == FormatBC2 ? 16 : 4; }

inline uint32_t levelDim(uint32_t size, uint32_t level)  { const uint32_t d = size >> level; return d ? d : 1; }
// Units (blocks or pixels) across one dimension of a level.
inline uint32_t levelUnits(Format format, uint32_t size, uint32_t level)
{
    const uint32_t d = levelDim(size, level);
    return isBlock(format) ? (d + 3) / 4 : d;
}

// Bytes of one level, the same in the Direct3D layout (rows packed) and the GPU layout.
uint32_t levelBytes(Format format, uint32_t width, uint32_t height, uint32_t level);
// Offset of a level in a face of the GPU layout.
uint32_t levelOffset(Format format, uint32_t width, uint32_t height, uint32_t level);
// Bytes of all levels of one face.
uint32_t chainBytes(Format format, uint32_t width, uint32_t height, uint32_t levels);
// Distance between cube faces in the GPU layout.
uint32_t faceStride(Format format, uint32_t width, uint32_t height, uint32_t levels);
// Bytes of the GPU layout: one face, or six for a cube.
uint32_t totalBytes(Format format, uint32_t width, uint32_t height, uint32_t levels, bool cube);
// Direct3D pitch of a level: bytes of one row of blocks or pixels.
uint32_t levelPitch(Format format, uint32_t width, uint32_t level);

// Morton index of unit (x, y) in a swizzled level of w x h units (powers of two).
uint32_t swizzleIndex(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Copies one level from the Direct3D layout (pitch bytes per row of units) into the GPU layout, or back.
void swizzleLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, uint32_t pitch,
                  void* dst);
void unswizzleLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, void* dst,
                    uint32_t pitch);

// Decodes one level from the Direct3D layout into 0xAABBGGRR pixels (w x h of the level, rows packed).
void decodeLevel(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, uint32_t pitch,
                 uint32_t* rgba);

// Smallest alpha (0..255) of the texels of one level in the Direct3D layout; BC1 texels of index 3 in three-colour
// blocks have alpha 0, all others 255.
uint8_t minAlpha(Format format, uint32_t width, uint32_t height, uint32_t level, const void* src, uint32_t pitch);

}}

#endif
