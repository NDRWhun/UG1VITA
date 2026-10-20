#include <winapi/dinput8.h>
#include <winapi/com.h>
#include <winapi/dinput/idirectinput8.h>
#include <winapi/support.h>
#include <platform/log.h>

namespace win32 { namespace dinput8
{

static const HRESULT DIERR_NOINTERFACE = 0x80004002;
static const HRESULT DIERR_INVALIDPARAM = 0x80070057;
static const HRESULT DIERR_OLDDIRECTINPUTVERSION = 0x8007047E;
static const HRESULT DIERR_BETADIRECTINPUTVERSION = 0x80070481;

HRESULT DirectInput8Create(WinApplication* app, x86::CPU& cpu,
                           HINSTANCE hinst, DWORD dwVersion, const GUID* riidltf, x86::reg32* ppvOut,
                           LPVOID punkOuter)
{
    RC_USE(hinst);
    if (!ppvOut || !riidltf)
        return DIERR_INVALIDPARAM;
    *ppvOut = 0;
    if (punkOuter)
        unsupported(app, cpu, "DirectInput8Create", "aggregation");
    if (dwVersion < 0x800)
        return DIERR_OLDDIRECTINPUTVERSION;
    if (dwVersion > 0x800)
        return DIERR_BETADIRECTINPUTVERSION;
    if (!(*riidltf == dinput::IDirectInput8::interfaceId()))
    {
        pf::logError("DirectInput8Create: interface %08x not supported (from 0x%08x)", riidltf->Data1,
                     callerAddress(app, cpu));
        return DIERR_NOINTERFACE;
    }
    *ppvOut = com::ComAlloc(app);
    new (&app->getMemory<dinput::IDirectInput8>(*ppvOut)) dinput::IDirectInput8();
    return 0;
}

}}
