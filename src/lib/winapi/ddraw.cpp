#include <winapi/ddraw.h>
#include <winapi/com.h>
#include <winapi/ddraw/idirectdraw2.h>
#include <x86.h>
#include <lib/memmap.h>
#include <lib/library.h>
#include <winapi/wrapper.h>


namespace win32 { namespace ddraw
{


HRESULT DirectDrawCreate(WinApplication* app, x86::CPU& cpu,
                         GUID* lpGUID, x86::reg32* lplpDD, void* pUnkOuter)
{
    RC_USE(cpu);
    RC_USE(lpGUID);
    RC_ASSERT(pUnkOuter == nullptr);
    *lplpDD = com::ComAlloc(app);
    new (&app->getMemory<IDirectDraw2>(*lplpDD)) IDirectDraw2();
    return 0;
}

// No DirectDraw devices are reported.
HRESULT DirectDrawEnumerateExA(WinApplication* app, x86::CPU& cpu,
                               x86::reg32 lpCallback, x86::reg32 lpContext, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpCallback);
    RC_USE(lpContext);
    RC_USE(dwFlags);
    return 0;
}

HRESULT DirectDrawEnumerateA(WinApplication* app, x86::CPU& cpu,
                             x86::reg32 lpCallback, x86::reg32 lpCallbackParam)
{
    cpu.esp -= 20;
    app->getMemory<x86::reg32>(cpu.esp + 0) = lpCallback;
    app->getMemory<x86::reg32>(cpu.esp + 4) = 0;
    app->getMemory<x86::reg32>(cpu.esp + 8) = 0;
    app->getMemory<x86::reg32>(cpu.esp + 12) = 0;
    app->getMemory<x86::reg32>(cpu.esp + 16) = lpCallbackParam;
    app->dynamic_call(lpCallback, cpu);
    return 0;
}

Library s_dDrawLibrary = win32::Library("ddraw.dll");
Library* s_dDrawRegistry = &(s_dDrawLibrary
    .registerSymbol("DirectDrawCreate", &Wrapper<decltype(&DirectDrawCreate), &DirectDrawCreate>::stdcall));

}}
