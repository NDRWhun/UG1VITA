#include <winapi/dinput.h>
#include <winapi/com.h>
#include <winapi/dinput/idirectinput2.h>
#include <x86.h>

namespace win32 { namespace dinput
{

HRESULT DirectInputCreateA(WinApplication* app, x86::CPU& cpu,
                           HINSTANCE hinst, DWORD dwVersion, Packed<IDirectInput2>* lplpDirectInput,
                           LPVOID punkOuter)
{
    RC_USE(cpu);
    RC_USE(hinst);
    RC_USE(dwVersion);
    RC_ASSERT(punkOuter == nullptr);
    *lplpDirectInput = Packed<IDirectInput2>(com::ComAlloc(app));
    IDirectInput2* dinput = &app->getMemory<IDirectInput2>(*lplpDirectInput);
    new (dinput) IDirectInput2();
    return 0;
}

}}
