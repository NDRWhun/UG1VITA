#ifndef LIB_FOREIGNCODE_H_
#define LIB_FOREIGNCODE_H_

// Guest code that is not part of the executable, such as machine code the game loads from its data files. A call
// through a pointer to an address with no translated function asks the resolver before it traps; the method it
// returns starts at cpu.ip.

#include <lib/winapp.h>

namespace win32
{

// The method that runs the code at address, or nullptr when the address holds no known foreign code.
typedef MethodPtr (*ForeignCodeResolver)(x86::reg32 address);

void setForeignCodeResolver(ForeignCodeResolver resolver);

}

#endif /* !LIB_FOREIGNCODE_H_ */
