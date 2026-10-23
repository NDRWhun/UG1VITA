#ifndef PLATFORM_PGO_H_
#define PLATFORM_PGO_H_

// Instrumented build (NFSU_PGO_GENERATE): writes the profile counters to a .profraw file.
namespace pf { namespace pgo
{

// Writes the profile to path every 30 seconds from a low-priority thread.
void start(const char* path);
// Stops the writer thread.
void stop();
// Writes the profile now.
void write();

}}

#endif
