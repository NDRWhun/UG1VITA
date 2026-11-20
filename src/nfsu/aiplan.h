#ifndef NFSU_AIPLAN_H_
#define NFSU_AIPLAN_H_

// Native AI planning functions (aiplan.cpp). Settings: ai_native=off runs the guest code, shadow_ai=on runs the
// guest code and then the native code against the memory before it, and logs the first differences.

namespace nfsu { namespace aiplan
{

// With shadow_ai=on: logs the calls compared and differing so far as a "shadow_ai:" line.
void logShadowTotal();

}}

#endif
