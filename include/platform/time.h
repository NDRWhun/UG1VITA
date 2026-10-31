#ifndef PLATFORM_TIME_H_
#define PLATFORM_TIME_H_

#include <cstdint>

namespace pf
{

// Process-relative clocks.
uint64_t ticksUs();
// The low word of ticksUs, read from the clock's low word only (for differences modulo 2^32).
uint32_t ticksUsLow();
uint32_t ticksMs();

void sleepMs(uint32_t milliseconds);
void sleepUs(uint32_t microseconds);

}

#endif
