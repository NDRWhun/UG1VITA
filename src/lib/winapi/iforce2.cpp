#include <winapi/iforce2.h>
#include <platform/log.h>
#include <x86.h>

namespace win32 { namespace iforce2
{

x86::reg32 IFLoadProjectFile(WinApplication* app, x86::CPU& cpu, LPCSTR fileName, x86::reg32 device)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(device);
    pf::logInfo("IFLoadProjectFile %s: no force feedback", fileName ? fileName : "");
    return 0;
}

BOOL IFReleaseProject(WinApplication* app, x86::CPU& cpu, x86::reg32 project)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(project);
    return 0;
}

x86::reg32 IFCreateEffects(WinApplication* app, x86::CPU& cpu, x86::reg32 project, LPCSTR objectName,
                           x86::reg32 effectCount)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(project);
    RC_USE(objectName);
    RC_USE(effectCount);
    return 0;
}

BOOL IFReleaseEffects(WinApplication* app, x86::CPU& cpu, x86::reg32 project, x86::reg32 effects)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(project);
    RC_USE(effects);
    return 0;
}

}}
