#ifndef PLATFORM_FILESYSTEM_H_
#define PLATFORM_FILESYSTEM_H_

#include <cstdint>
#include <string>

namespace pf
{

// The on-disk spelling of a lower-cased path relative to root (a folder ending in '/'). The Vita file system
// ignores case, so the path is returned as it is.
std::string resolveCase(const std::string& root, const std::string& path);

// Total and free bytes of the volume that holds the folder; false if unknown.
bool diskSpace(const std::string& folder, uint64_t* total, uint64_t* free);

}

#endif
