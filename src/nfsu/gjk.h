#ifndef NFSU_GJK_H_
#define NFSU_GJK_H_

// Native GJK distance parts (gjk.cpp). Settings: gjk_native=off runs the guest code, shadow_gjk=on runs the guest
// code and then the native code against the memory before it, and logs the first differences.

namespace nfsu { namespace gjk
{

// With shadow_gjk=on: logs the calls compared and differing so far as a "shadow_gjk:" line.
void logShadowTotal();

}}

#endif
