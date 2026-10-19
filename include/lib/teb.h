#ifndef LIB_TEB_H_
#define LIB_TEB_H_

#include <x86.h>

// Guest thread block (fs): the Win32 TEB fields guest code and the shims use, plus two host words at the end.
namespace win32 { namespace teb
{

static const x86::reg32 Size            = 0x1000;
static const x86::reg32 ExceptionList   = 0x000;    // SEH chain head, 0xffffffff when empty
static const x86::reg32 StackBase       = 0x004;
static const x86::reg32 StackLimit      = 0x008;
static const x86::reg32 Self            = 0x018;
static const x86::reg32 ProcessId       = 0x020;
static const x86::reg32 ThreadId        = 0x024;
static const x86::reg32 LastError       = 0x034;
static const x86::reg32 TlsSlots        = 0xe10;    // TlsSlotCount dwords
static const x86::reg32 TlsSlotCount    = 64;
// Host pointers get 8-byte slots, so they fit on 64-bit hosts too.
static const x86::reg32 HostLock        = 0xff0;    // Mutex* of the execution context, or null
static const x86::reg32 HostControl     = 0xff8;    // ThreadControl* of the host thread (lib/scheduler.h)

static const x86::reg32 GuestProcessId  = 1;

}}

#endif
