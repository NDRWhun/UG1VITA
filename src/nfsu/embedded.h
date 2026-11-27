// The game executable the code was generated from, embedded in builds with NFSU_EMBED_EXE (nfsu_image.cpp, written by
// tools/recompiler/generate.py). main.cpp uses it when the player's executable is a version 1.4 build whose data
// cannot be read (a copy-protected one).
#ifndef NFSU_EMBEDDED_H_
#define NFSU_EMBEDDED_H_

#include <cstdint>

namespace nfsu { namespace image {

// The executable's bytes and size; nullptr and 0 in builds without NFSU_EMBED_EXE.
const uint8_t* exe(uint32_t& size);

}}

#endif /* !NFSU_EMBEDDED_H_ */
