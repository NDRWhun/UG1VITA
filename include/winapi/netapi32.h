#ifndef netapi32_H_
#define netapi32_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

// NetBIOS. The port is offline: an adapter enumeration finds no adapters, any other command reports an invalid
// adapter number.
namespace win32 { namespace netapi32
{

struct NCB;

BYTE Netbios(WinApplication* app, x86::CPU& cpu, NCB* pncb);

}}

#endif
