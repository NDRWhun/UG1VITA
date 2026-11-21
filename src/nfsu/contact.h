#ifndef NFSU_CONTACT_H_
#define NFSU_CONTACT_H_

// Native contact test helpers (contact.cpp). Settings: contact_native=off runs the guest code, shadow_contact=on runs
// the guest code and then the native code against the memory before it, and logs the first differences.

namespace nfsu { namespace contact
{

// With shadow_contact=on: logs the calls compared and differing so far as a "shadow_contact:" line.
void logShadowTotal();

}}

#endif
