#ifndef iforce2_H_
#define iforce2_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

// Immersion I-FORCE 2 force-feedback library: there is no force-feedback device, every call fails.
namespace win32 { namespace iforce2
{

x86::reg32 IFLoadProjectFile(WinApplication* app, x86::CPU& cpu, LPCSTR fileName, x86::reg32 device);
BOOL IFReleaseProject(WinApplication* app, x86::CPU& cpu, x86::reg32 project);
x86::reg32 IFCreateEffects(WinApplication* app, x86::CPU& cpu, x86::reg32 project, LPCSTR objectName,
                           x86::reg32 effectCount);
BOOL IFReleaseEffects(WinApplication* app, x86::CPU& cpu, x86::reg32 project, x86::reg32 effects);

}}

#endif
