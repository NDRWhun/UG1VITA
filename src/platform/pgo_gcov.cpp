// Writes the gcc profile counters as one gcov-tool merge-stream file.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <gcov.h>

// Linker-made bounds of the gcov_info section (-fprofile-info-section=gcov_info).
extern const gcov_info* const __start_gcov_info[];
extern const gcov_info* const __stop_gcov_info[];

int nfsu_profile_write(FILE* file);
}

namespace
{

const size_t BufferSize = 1024 * 1024;

struct Stream
{
    FILE* file;
    std::vector<unsigned char> buffer;
    bool failed;
};

void flush(Stream& stream)
{
    const size_t size = stream.buffer.size();
    if (size && fwrite(stream.buffer.data(), 1, size, stream.file) != size)
        stream.failed = true;
    stream.buffer.clear();
}

void dump(const void* data, unsigned size, void* arg)
{
    Stream& stream = *static_cast<Stream*>(arg);
    if (stream.buffer.size() + size > BufferSize)
        flush(stream);
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    stream.buffer.insert(stream.buffer.end(), bytes, bytes + size);
}

bool separator(char c) { return c == '/' || c == '\\'; }

// The object's .gcda path relative to the build folder, with '/' separators; other paths keep their full name.
std::string relativeName(const char* name)
{
    const char* base = NFSU_PGO_BUILD_DIR;
    const char* rest = name;
    for (; *base && *rest; ++base, ++rest)
    {
        if (separator(*base) ? !separator(*rest) : tolower((unsigned char)*base) != tolower((unsigned char)*rest))
            break;
    }
    if (*base == '\0' && separator(*rest))
        name = rest + 1;
    std::string result(name);
    for (char& c : result)
    {
        if (c == '\\')
            c = '/';
    }
    return result;
}

void filename(const char* name, void* arg)
{
    __gcov_filename_to_gcfn(relativeName(name).c_str(), dump, arg);
}

void* allocate(unsigned size, void*)
{
    return malloc(size);
}

}

// Each translation unit: its file name record, then its .gcda data. Returns 0 when everything was written.
int nfsu_profile_write(FILE* file)
{
    Stream stream{file, {}, false};
    stream.buffer.reserve(BufferSize);
    const gcov_info* const* info = __start_gcov_info;
    const gcov_info* const* end = __stop_gcov_info;
    __asm__("" : "+r"(info));
    for (; info != end; ++info)
        __gcov_info_to_gcda(*info, filename, dump, allocate, &stream);
    flush(stream);
    return stream.failed ? 1 : 0;
}
