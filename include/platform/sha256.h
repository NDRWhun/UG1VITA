#ifndef PLATFORM_SHA256_H_
#define PLATFORM_SHA256_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace pf
{

// SHA-256 of a buffer as 64 lower-case hex digits.
std::string sha256Hex(const uint8_t* data, size_t size);

}

#endif
