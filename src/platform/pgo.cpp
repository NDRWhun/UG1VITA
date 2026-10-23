#include <platform/pgo.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <cstdio>
#include <string>

extern "C" {
int __llvm_profile_runtime = 0;
// src/platform/pgo_writer.c
int nfsu_profile_write(FILE* file);
}

namespace pf { namespace pgo
{

namespace
{

const uint32_t IntervalMs = 30 * 1000;

std::string s_path;
LwMutex s_lock("pgo");
Thread* s_thread = nullptr;

int writerEntry(void*)
{
    for (;;)
    {
        sleepMs(IntervalMs);
        write();
    }
    return 0;
}

}

void start(const char* path)
{
    s_path = path;
    s_thread = new Thread("pgo_writer", &writerEntry, nullptr, Thread::PriorityLow, 64 * 1024);
    s_thread->start();
}

// Writes path.part, then renames it to path.
void write()
{
    LwLockGuard guard(s_lock);
    if (s_path.empty())
        return;
    const std::string part = s_path + ".part";
    FILE* file = fopen(part.c_str(), "wb");
    if (!file)
    {
        logError("pgo: cannot create %s", part.c_str());
        return;
    }
    const bool written = nfsu_profile_write(file) == 0;
    const long size = ftell(file);
    if (fclose(file) != 0 || !written)
    {
        logError("pgo: writing %s failed", part.c_str());
        return;
    }
    remove(s_path.c_str());
    if (rename(part.c_str(), s_path.c_str()) != 0)
    {
        logError("pgo: renaming %s failed", part.c_str());
        return;
    }
    logInfo("pgo: %ld bytes written to %s", size, s_path.c_str());
}

}}
