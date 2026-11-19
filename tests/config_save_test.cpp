#include <platform/config.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace pf {
void logInfo(const char*, ...) {}
void logError(const char*, ...) {}
}

static std::string read(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const char* path = argv[1];
    const std::string original = "\xef\xbb\xbf# settings\r\nframe_rate=30\r\n rear_touch =off\r\n"
                                 "# keep this comment\r\nunknown_setting=unchanged\r\nrear_touch=on\r\n";
    { std::ofstream file(path, std::ios::binary); file << original; }
    pf::Config::load(path);
    assert(!pf::Config::getBool("rear_touch", true));
    assert(pf::Config::setBool("rear_touch", true));
    assert(pf::Config::getBool("rear_touch", false));
    assert(read(path) == "\xef\xbb\xbf# settings\r\nframe_rate=30\r\n rear_touch =on\r\n"
                         "# keep this comment\r\nunknown_setting=unchanged\r\nrear_touch=on\r\n");
    pf::Config::load(path);
    assert(pf::Config::getBool("rear_touch", false));
    assert(pf::Config::setBool("rear_touch", false));
    pf::Config::load(path);
    assert(!pf::Config::getBool("rear_touch", true));
    assert(pf::Config::getInt("frame_rate", 0) == 30);
    remove(path);
    pf::Config::load(path);
    assert(pf::Config::setBool("rear_touch", true));
    assert(read(path) == "rear_touch=on\n");
    pf::Config::load("/no-such-directory/nfsu/settings.cfg");
    assert(!pf::Config::setBool("rear_touch", true));
    assert(!pf::Config::getBool("rear_touch", false));
    std::puts("config: persisted toggles, preserved unrelated content, handled missing/unwritable file");
}
