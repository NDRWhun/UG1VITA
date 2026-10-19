#ifndef DINPUT_IDIRECTINPUT8_H_
#define DINPUT_IDIRECTINPUT8_H_

#include "winapi/com.h"

namespace win32 { namespace dinput
{

class IDirectInputDevice;

// IDirectInput8A over the same devices as IDirectInput2: the system keyboard, the system mouse and the Vita pad as
// one game controller. Devices it creates are IDirectInputDevice8A.
class IDirectInput8 : public com::IUnknown
{
public:
    IDirectInput8();

    static x86::reg32 init(WinApplication* app, x86::reg32 memblockIndex, x86::reg8* memBlock);
    static const GUID& interfaceId();

private:
    typedef x86::reg32  LPDIENUMDEVICESCALLBACK;

    HRESULT CreateDevice(WinApplication* app, x86::CPU& cpu,
                         REFGUID rguid, Packed<IDirectInputDevice>* lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
    HRESULT EnumDevices(WinApplication* app, x86::CPU& cpu,
                        DWORD dwDevType, LPDIENUMDEVICESCALLBACK lpCallback, x86::reg32 pvRef, DWORD dwFlags);
    HRESULT GetDeviceStatus(WinApplication* app, x86::CPU& cpu,
                            REFGUID rguidInstance);
    HRESULT RunControlPanel(WinApplication* app, x86::CPU& cpu,
                            HWND hwndOwner, DWORD dwFlags);
    HRESULT Initialize(WinApplication* app, x86::CPU& cpu,
                       HINSTANCE hinst, DWORD dwVersion);
    HRESULT FindDevice(WinApplication* app, x86::CPU& cpu,
                       REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance);
    HRESULT EnumDevicesBySemantics(WinApplication* app, x86::CPU& cpu,
                                   LPCSTR ptszUserName, x86::reg32 lpdiActionFormat, x86::reg32 lpCallback,
                                   x86::reg32 pvRef, DWORD dwFlags);
    HRESULT ConfigureDevices(WinApplication* app, x86::CPU& cpu,
                             x86::reg32 lpdiCallback, x86::reg32 lpdiCDParams, DWORD dwFlags, x86::reg32 pvRefData);

private:
    static x86::reg32 s_vtable;
};

}}

#endif /*!DINPUT_IDIRECTINPUT8_H_*/
