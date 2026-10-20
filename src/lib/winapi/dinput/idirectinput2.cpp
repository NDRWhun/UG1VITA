#include <winapi/dinput/idirectinput2.h>
#include <winapi/dinput/idirectinputdevice.h>
#include <winapi/support.h>
#include <winapi/wrapper.h>
#include <lib/controls.h>
#include <lib/gamepad.h>


namespace win32 { namespace dinput
{


x86::reg32 IDirectInput2::s_iDirectInput2Vtable = 0;
const GUID IDirectInput2::s_GUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
const GUID IDirectInput2::s_GUID_SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };


struct DIDEVICEINSTANCE
{
    x86::reg32 dwSize;
    GUID guidInstance;
    GUID guidProduct;
    x86::reg32 dwDevType;
    char tszInstanceName[MAX_PATH];
    char tszProductName[MAX_PATH];
    GUID guidFFDriver;
    x86::reg16 wUsagePage;
    x86::reg16 wUsage;
};

DEFINE_GUID(IID_IDirectInput2A,		0x5944E662,0xAA8A,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);

IDirectInput2::IDirectInput2()
    :   IUnknown(IID_IDirectInput2A)
{
    m_vtable = s_iDirectInput2Vtable;
}

HRESULT IDirectInput2::CreateDevice(WinApplication* app, x86::CPU& cpu,
                                    REFGUID rguid, Packed<IDirectInputDevice>* lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_ASSERT(pUnkOuter == nullptr);
    Input* input;
    if (*rguid == s_GUID_SysKeyboard)
    {
        input = new Keyboard;
    }
    else if (*rguid == s_GUID_SysMouse)
    {
        input = new Mouse;
    }
    else
    {
        input = new Gamepad(rguid->Data3);
    }
    app->allocateResource(input);
    *lplpDirectInputDevice = Packed<IDirectInputDevice>(com::ComAlloc(app));
    new (&app->getMemory<IDirectInputDevice>(*lplpDirectInputDevice)) IDirectInputDevice(input, false);
    return 0;
}

HRESULT IDirectInput2::EnumDevices(WinApplication* app, x86::CPU& cpu,
                                   DWORD dwDevType, LPDIENUMDEVICESCALLBACK lpCallback, Packed<void> pvRef, DWORD dwFlags)
{
    RC_USE(dwFlags);

    static const GUID s_joystickGUID = { 0x4251809a, 0x67be, 0, "VITAPAD" };
    static const GUID s_driverGUID = { 0x4251809a, 0x67bf, 0xffff, "VITAPAD" };
    if (dwDevType == 4)
    {
        MemMap mem(1024);
        DIDEVICEINSTANCE* device = &app->getMemory<DIDEVICEINSTANCE>(mem.getBlockStart());
        device->dwSize = sizeof(DIDEVICEINSTANCE);
        for (x86::reg16 deviceIndex = 0; deviceIndex < Gamepad::getCount(); ++deviceIndex)
        {
            device->dwDevType = controls::JoystickDeviceType;
            device->guidInstance = s_joystickGUID;
            device->guidInstance.Data3 = deviceIndex;
            device->guidProduct = controls::JoystickProduct;
            strcpy(device->tszInstanceName, controls::JoystickName);
            strcpy(device->tszProductName, controls::JoystickName);
            device->guidFFDriver = s_driverGUID;
            device->wUsagePage = 0;
            device->wUsage = 0;
            cpu.esp -= 12;
            app->getMemory<x86::reg32>(cpu.esp + 0) = lpCallback;
            app->getMemory<x86::reg32>(cpu.esp + 4) = mem.getBlockStart();
            app->getMemory<x86::reg32>(cpu.esp + 8) = pvRef;
            app->dynamic_call(lpCallback, cpu);
            if (!cpu.eax)
                break;
        }
    }
    return 0;
}

// The system keyboard and mouse and the pads are attached; other devices are not registered.
HRESULT IDirectInput2::GetDeviceStatus(WinApplication* app, x86::CPU& cpu,
                                       REFGUID rguidInstance)
{
    RC_USE(app);
    RC_USE(cpu);
    static const HRESULT DIERR_DEVICENOTREG = 0x80040154;
    if (!rguidInstance)
        return 0x80070057;
    if (*rguidInstance == s_GUID_SysKeyboard || *rguidInstance == s_GUID_SysMouse)
        return 0;
    static const GUID s_joystickGUID = { 0x4251809a, 0x67be, 0, "VITAPAD" };
    GUID pad = s_joystickGUID;
    pad.Data3 = rguidInstance->Data3;
    return *rguidInstance == pad && rguidInstance->Data3 < Gamepad::getCount() ? 0 : DIERR_DEVICENOTREG;
}

HRESULT IDirectInput2::RunControlPanel(WinApplication* app, x86::CPU& cpu,
                                       HWND hwndOwner, DWORD dwFlags)
{
    RC_USE(hwndOwner);
    unsupported(app, cpu, "IDirectInput2::RunControlPanel", "flags 0x%x", unsigned(dwFlags));
}

// DirectInputCreateA initialised the object already.
HRESULT IDirectInput2::Initialize(WinApplication* app, x86::CPU& cpu,
                                  HINSTANCE hinst, DWORD dwVersion)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hinst);
    RC_USE(dwVersion);
    return 0;
}

// Finds HID devices by name; the pad is not a HID device (DIERR_DEVICENOTREG).
HRESULT IDirectInput2::FindDevice(WinApplication* app, x86::CPU& cpu,
                                  REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rguidClass);
    RC_USE(ptszName);
    RC_USE(pguidInstance);
    return 0x80040154;
}

x86::reg32 IDirectInput2::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400042, {"IDirectInput2::CreateDevice", &Wrapper<decltype(&IDirectInput2::CreateDevice), &IDirectInput2::CreateDevice>::stdcall});
    app->registerMethod(0x400043, {"IDirectInput2::EnumDevices", &Wrapper<decltype(&IDirectInput2::EnumDevices), &IDirectInput2::EnumDevices>::stdcall});
    app->registerMethod(0x400044, {"IDirectInput2::GetDeviceStatus", &Wrapper<decltype(&IDirectInput2::GetDeviceStatus), &IDirectInput2::GetDeviceStatus>::stdcall});
    app->registerMethod(0x400045, {"IDirectInput2::RunControlPanel", &Wrapper<decltype(&IDirectInput2::RunControlPanel), &IDirectInput2::RunControlPanel>::stdcall});
    app->registerMethod(0x400046, {"IDirectInput2::Initialize", &Wrapper<decltype(&IDirectInput2::Initialize), &IDirectInput2::Initialize>::stdcall});
    app->registerMethod(0x400047, {"IDirectInput2::FindDevice", &Wrapper<decltype(&IDirectInput2::FindDevice), &IDirectInput2::FindDevice>::stdcall});

    s_iDirectInput2Vtable = memblockIndex;
    vtable[0] = 0x400000;    // IUnknown_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknwon_Release
    vtable[3] = 0x400042;    // IDirectInput2_CreateDevice
    vtable[4] = 0x400043;    // IDirectInput2_EnumDevices
    vtable[5] = 0x400044;    // IDirectInput2_GetDeviceStatus
    vtable[6] = 0x400045;    // IDirectInput2_RunControlPanel
    vtable[7] = 0x400046;    // IDirectInput2_Initialize
    vtable[8] = 0x400047;    // IDirectInput2_FindDevice
    return 9 * sizeof(x86::reg32);
}

}}
