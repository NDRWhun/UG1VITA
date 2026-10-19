#ifndef LIB_HEAP_H_
#define LIB_HEAP_H_

#include <x86.h>

namespace win32
{

// Guest heap behind HeapAlloc and GlobalAlloc: 16-byte granular blocks in guest memory. Small and medium sizes
// come from size classes carved out of 1 MB chunks; large blocks are whole guest memory blocks. Any thread.
namespace heap
{

// Guest address of a new block (16-byte aligned, zeroed when asked), 0 when guest memory is exhausted.
x86::reg32 allocate(x86::reg32 size, bool zero);
// False if the address is not a live block.
bool release(x86::reg32 address);
// Requested size of a live block, or 0xffffffff.
x86::reg32 size(x86::reg32 address);
// Bytes the guest holds in live blocks (log statistics).
x86::reg32 used();

}

}

#endif
