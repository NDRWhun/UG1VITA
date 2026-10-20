#include <platform/filesystem.h>
#include <psp2/io/devctl.h>
#include <cstring>

namespace pf
{

std::string resolveCase(const std::string& root, const std::string& path)
{
    (void)root;
    return path;
}

bool diskSpace(const std::string& folder, uint64_t* total, uint64_t* free)
{
    // The device of the folder ("ux0:"), queried with devctl 0x3001.
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    const std::string device = folder.substr(0, folder.find(':') + 1);
    if (sceIoDevctl(device.c_str(), 0x3001, nullptr, 0, &info, sizeof(info)) < 0)
        return false;
    *total = uint64_t(info.max_size);
    *free = uint64_t(info.free_size);
    return true;
}

}
