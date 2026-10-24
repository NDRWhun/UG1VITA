#ifndef PLATFORM_PGO_H_
#define PLATFORM_PGO_H_

// Instrumented build (NFSU_PGO_GENERATE): writes the profile counters to a .profraw file (clang) or a gcov-tool
// merge-stream file (gcc, NFSU_PGO_GCOV).
namespace pf { namespace pgo
{

#ifdef NFSU_PGO_GCOV
constexpr const char* FileName = "nfsu.gcov";
#else
constexpr const char* FileName = "nfsu.profraw";
#endif

// Writes the profile to path every 30 seconds from a low-priority thread.
void start(const char* path);
// Stops the writer thread.
void stop();
// Writes the profile now.
void write();

}}

#endif
