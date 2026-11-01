#include <winapi/dinput/idirectinput8.h>
#include <winapi/dinput/idirectinputdevice.h>
#include <winapi/support.h>
#include <winapi/wrapper.h>
#include <lib/controls.h>
#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <platform/log.h>
#include <cstring>
#include <vector>

namespace win32 { namespace dinput
{

x86::reg32 IDirectInput8::s_vtable = 0;

DEFINE_GUID(IID_IDirectInput8A, 0xBF798030,0x483A,0x4DA2,0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00);
DEFINE_GUID(GUID_SysKeyboard,   0x6F1D2B61,0xD5A0,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(GUID_SysMouse,      0x6F1D2B60,0xD5A0,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
// Joystick instance (Data3 = pad index) and product, as IDirectInput2 reports them.
static const GUID s_joystickGuid = { 0x4251809a, 0x67be, 0, "VITAPAD" };
static const GUID s_driverGuid = { 0x4251809a, 0x67bf, 0xffff, "VITAPAD" };

static const HRESULT DI_OK = 0;
static const HRESULT DIERR_DEVICENOTREG = 0x80040154;
static const HRESULT DIERR_INVALIDPARAM = 0x80070057;

enum
{
    DI8DEVCLASS_ALL         = 0,
    DI8DEVCLASS_DEVICE      = 1,
    DI8DEVCLASS_POINTER     = 2,
    DI8DEVCLASS_KEYBOARD    = 3,
    DI8DEVCLASS_GAMECTRL    = 4,
    DI8DEVTYPE_MOUSE        = 0x12,
    DI8DEVTYPE_KEYBOARD     = 0x13,
    DIEDFL_FORCEFEEDBACK    = 0x100
};

// DIDEVICEINSTANCEA of DirectInput 8 (0x244 bytes).
struct DIDEVICEINSTANCE
{
    DWORD   dwSize;
    GUID    guidInstance;
    GUID    guidProduct;
    DWORD   dwDevType;
    char    tszInstanceName[MAX_PATH];
    char    tszProductName[MAX_PATH];
    GUID    guidFFDriver;
    WORD    wUsagePage;
    WORD    wUsage;
};
static_assert(sizeof(DIDEVICEINSTANCE) == 0x244, "DIDEVICEINSTANCEA layout");

const GUID& IDirectInput8::interfaceId()
{
    return IID_IDirectInput8A;
}

IDirectInput8::IDirectInput8()
    :   IUnknown(IID_IDirectInput8A)
{
    m_vtable = s_vtable;
}

HRESULT IDirectInput8::CreateDevice(WinApplication* app, x86::CPU& cpu,
                                    REFGUID rguid, Packed<IDirectInputDevice>* lplpDirectInputDevice,
                                    LPUNKNOWN pUnkOuter)
{
    if (!rguid || !lplpDirectInputDevice)
        return DIERR_INVALIDPARAM;
    if (pUnkOuter)
        unsupported(app, cpu, "IDirectInput8::CreateDevice", "aggregation");
    Input* input;
    if (*rguid == GUID_SysKeyboard)
    {
        input = new Keyboard;
    }
    else if (*rguid == GUID_SysMouse)
    {
        input = new Mouse;
    }
    else
    {
        GUID pad = s_joystickGuid;
        pad.Data3 = rguid->Data3;
        if (!(*rguid == pad) || rguid->Data3 >= Gamepad::getCount())
        {
            *lplpDirectInputDevice = Packed<IDirectInputDevice>(0);
            return DIERR_DEVICENOTREG;
        }
        input = new Gamepad(rguid->Data3);
    }
    app->allocateResource(input);
    *lplpDirectInputDevice = Packed<IDirectInputDevice>(com::ComAlloc(app));
    new (&app->getMemory<IDirectInputDevice>(*lplpDirectInputDevice)) IDirectInputDevice(input, true);
    return DI_OK;
}

HRESULT IDirectInput8::EnumDevices(WinApplication* app, x86::CPU& cpu,
                                   DWORD dwDevType, LPDIENUMDEVICESCALLBACK lpCallback, x86::reg32 pvRef,
                                   DWORD dwFlags)
{
    if (!lpCallback)
        return DIERR_INVALIDPARAM;
    // No device has force feedback.
    if (dwFlags & DIEDFL_FORCEFEEDBACK)
        return DI_OK;
    struct Device
    {
        GUID        instance;
        GUID        product;
        DWORD       type;
        const char* name;
        DWORD       devClass;
    };
    std::vector<Device> devices;
    devices.push_back(Device{ GUID_SysMouse, GUID_SysMouse, Mouse().getDeviceType8(), "Mouse", DI8DEVCLASS_POINTER });
    devices.push_back(Device{ GUID_SysKeyboard, GUID_SysKeyboard, Keyboard().getDeviceType8(), "Keyboard",
                              DI8DEVCLASS_KEYBOARD });
    for (x86::reg32 index = 0; index < Gamepad::getCount(); ++index)
    {
        Device pad{ s_joystickGuid, controls::JoystickProduct, Gamepad(index).getDeviceType8(),
                    controls::JoystickName, DI8DEVCLASS_GAMECTRL };
        pad.instance.Data3 = x86::reg16(index);
        devices.push_back(pad);
    }
    MemMap memory(sizeof(DIDEVICEINSTANCE));
    const x86::reg32 address = memory.getBlockStart();
    DIDEVICEINSTANCE* info = &app->getMemory<DIDEVICEINSTANCE>(address);
    for (const Device& device : devices)
    {
        // A class, or a DI8DEVTYPE_* value (0x11 and up) matched against the type's low byte.
        const bool match = dwDevType == DI8DEVCLASS_ALL || dwDevType == device.devClass
                        || (dwDevType >= 0x11 && dwDevType == (device.type & 0xff));
        if (!match)
            continue;
        memset(info, 0, sizeof(*info));
        info->dwSize = sizeof(*info);
        info->guidInstance = device.instance;
        info->guidProduct = device.product;
        info->dwDevType = device.type;
        strcpy(info->tszInstanceName, device.name);
        strcpy(info->tszProductName, device.name);
        if (device.devClass == DI8DEVCLASS_GAMECTRL)
            info->guidFFDriver = s_driverGuid;
        // BOOL CALLBACK (LPCDIDEVICEINSTANCE, LPVOID), stdcall.
        cpu.esp -= 12;
        app->getMemory<x86::reg32>(cpu.esp + 0) = lpCallback;
        app->getMemory<x86::reg32>(cpu.esp + 4) = address;
        app->getMemory<x86::reg32>(cpu.esp + 8) = pvRef;
        app->dynamic_call(lpCallback, cpu);
        if (!cpu.eax)
            break;
    }
    return DI_OK;
}

// Every device the interface enumerates is attached.
HRESULT IDirectInput8::GetDeviceStatus(WinApplication* app, x86::CPU& cpu,
                                       REFGUID rguidInstance)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!rguidInstance)
        return DIERR_INVALIDPARAM;
    if (*rguidInstance == GUID_SysKeyboard || *rguidInstance == GUID_SysMouse)
        return DI_OK;
    GUID pad = s_joystickGuid;
    pad.Data3 = rguidInstance->Data3;
    return *rguidInstance == pad && rguidInstance->Data3 < Gamepad::getCount() ? DI_OK : DIERR_DEVICENOTREG;
}

HRESULT IDirectInput8::RunControlPanel(WinApplication* app, x86::CPU& cpu,
                                       HWND hwndOwner, DWORD dwFlags)
{
    RC_USE(hwndOwner);
    unsupported(app, cpu, "IDirectInput8::RunControlPanel", "flags 0x%x", unsigned(dwFlags));
}

// DirectInput8Create initialised the object already.
HRESULT IDirectInput8::Initialize(WinApplication* app, x86::CPU& cpu,
                                  HINSTANCE hinst, DWORD dwVersion)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hinst);
    RC_USE(dwVersion);
    return DI_OK;
}

// Finds HID devices by name; the pad is not a HID device.
HRESULT IDirectInput8::FindDevice(WinApplication* app, x86::CPU& cpu,
                                  REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rguidClass);
    RC_USE(ptszName);
    RC_USE(pguidInstance);
    return DIERR_DEVICENOTREG;
}

// The game's two action formats map keyboard semantics only. The Vita has no keyboard
// and the pad is read through its own bindings, so no device suits the formats: none is enumerated, and the game
// builds no action map.
HRESULT IDirectInput8::EnumDevicesBySemantics(WinApplication* app, x86::CPU& cpu,
                                              LPCSTR ptszUserName, x86::reg32 lpdiActionFormat,
                                              x86::reg32 lpCallback, x86::reg32 pvRef, DWORD dwFlags)
{
    RC_USE(ptszUserName);
    RC_USE(pvRef);
    RC_USE(dwFlags);
    if (!lpdiActionFormat || !lpCallback)
        return DIERR_INVALIDPARAM;
    pf::logInfo("EnumDevicesBySemantics: no keyboard for action format 0x%08x (from 0x%08x)", lpdiActionFormat,
                callerAddress(app, cpu));
    return DI_OK;
}

HRESULT IDirectInput8::ConfigureDevices(WinApplication* app, x86::CPU& cpu,
                                        x86::reg32 lpdiCallback, x86::reg32 lpdiCDParams, DWORD dwFlags,
                                        x86::reg32 pvRefData)
{
    RC_USE(lpdiCallback);
    RC_USE(lpdiCDParams);
    RC_USE(pvRefData);
    unsupported(app, cpu, "IDirectInput8::ConfigureDevices", "flags 0x%x", unsigned(dwFlags));
}

x86::reg32 IDirectInput8::init(WinApplication* app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400100, {"IDirectInput8::CreateDevice", &Wrapper<decltype(&IDirectInput8::CreateDevice), &IDirectInput8::CreateDevice>::stdcall});
    app->registerMethod(0x400101, {"IDirectInput8::EnumDevices", &Wrapper<decltype(&IDirectInput8::EnumDevices), &IDirectInput8::EnumDevices>::stdcall});
    app->registerMethod(0x400102, {"IDirectInput8::GetDeviceStatus", &Wrapper<decltype(&IDirectInput8::GetDeviceStatus), &IDirectInput8::GetDeviceStatus>::stdcall});
    app->registerMethod(0x400103, {"IDirectInput8::RunControlPanel", &Wrapper<decltype(&IDirectInput8::RunControlPanel), &IDirectInput8::RunControlPanel>::stdcall});
    app->registerMethod(0x400104, {"IDirectInput8::Initialize", &Wrapper<decltype(&IDirectInput8::Initialize), &IDirectInput8::Initialize>::stdcall});
    app->registerMethod(0x400105, {"IDirectInput8::FindDevice", &Wrapper<decltype(&IDirectInput8::FindDevice), &IDirectInput8::FindDevice>::stdcall});
    app->registerMethod(0x400106, {"IDirectInput8::EnumDevicesBySemantics", &Wrapper<decltype(&IDirectInput8::EnumDevicesBySemantics), &IDirectInput8::EnumDevicesBySemantics>::stdcall});
    app->registerMethod(0x400107, {"IDirectInput8::ConfigureDevices", &Wrapper<decltype(&IDirectInput8::ConfigureDevices), &IDirectInput8::ConfigureDevices>::stdcall});
    s_vtable = memblockIndex;
    vtable[0] = 0x400000;    // IUnknown::QueryInterface
    vtable[1] = 0x400001;    // IUnknown::AddRef
    vtable[2] = 0x400002;    // IUnknown::Release
    for (x86::reg32 i = 0; i < 8; ++i)
        vtable[3 + i] = 0x400100 + i;
    return 11 * sizeof(x86::reg32);
}

}}
