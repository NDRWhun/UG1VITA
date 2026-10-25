#include <winapi/dinput/idirectinputdevice.h>
#include <winapi/support.h>
#include <winapi/wrapper.h>
#include <lib/controls.h>
#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <cstddef>
#include <cstdio>
#include <vector>


namespace win32 { namespace dinput
{


x86::reg32 IDirectInputDevice::s_iDirectInputDeviceVtable = 0;
x86::reg32 IDirectInputDevice::s_iDirectInputDevice8Vtable = 0;

static const HRESULT DI_OK                = 0;
static const HRESULT DIERR_UNSUPPORTED    = 0x80004001;
static const HRESULT DIERR_OBJECTNOTFOUND = 0x80070002;
static const HRESULT DIERR_INVALIDPARAM   = 0x80070057;

// MAKEDIPROP(n): the property "GUID" argument is the number itself.
enum DirectInputProperty
{
    DIPROP_RANGE      = 4,
    DIPROP_DEADZONE   = 5,
    DIPROP_SATURATION = 6
};

enum DirectInputObjectType
{
    DIDFT_ABSAXIS   = 0x00000002,
    DIDFT_AXIS      = 0x00000003,
    DIDFT_PSHBUTTON = 0x00000004,
    DIDFT_BUTTON    = 0x0000000C
};

static const x86::reg32 s_propertyUnits = 10000;   // dead zone and saturation: 10000 = full deflection

enum DirectInutDeviceForceFeedBackCommand
{
    DISFFC_RESET            = 0x00000001,
    DISFFC_STOPALL          = 0x00000002,
    DISFFC_PAUSE            = 0x00000004,
    DISFFC_CONTINUE         = 0x00000008,
    DISFFC_SETACTUATORSON   = 0x00000010,
    DISFFC_SETACTUATORSOFF  = 0x00000020
};

enum DirectInutDeviceFlags
{
    DIDC_ATTACHED           = 0x00000001,
    DIDC_POLLEDDEVICE       = 0x00000002,
    DIDC_EMULATED           = 0x00000004,
    DIDC_POLLEDDATAFORMAT   = 0x00000008,
    DIDC_FORCEFEEDBACK      = 0x00000100,
    DIDC_FFATTACK           = 0x00000200,
    DIDC_FFFADE             = 0x00000400,
    DIDC_SATURATION         = 0x00000800,
    DIDC_POSNEGCOEFFICIENTS = 0x00001000,
    DIDC_POSNEGSATURATION   = 0x00002000,
    DIDC_DEADBAND           = 0x00004000,
    DIDC_STARTDELAY         = 0x00008000,
    DIDC_ALIAS              = 0x00010000,
    DIDC_PHANTOM            = 0x00020000,
    DIDC_HIDDEN             = 0x00040000
};

enum DirectInputDeviceHow
{
    DIPH_DEVICE     = 0,
    DIPH_BYOFFSET   = 1,
    DIPH_BYID       = 2,
    DIPH_BYUSAGE    = 3
};

enum DirectInputDeviceForceFeedbackState
{
    DIGFFS_EMPTY           = 0x00000001,
    DIGFFS_STOPPED         = 0x00000002,
    DIGFFS_PAUSED          = 0x00000004,
    DIGFFS_ACTUATORSON     = 0x00000010,
    DIGFFS_ACTUATORSOFF    = 0x00000020,
    DIGFFS_POWERON         = 0x00000040,
    DIGFFS_POWEROFF        = 0x00000080,
    DIGFFS_SAFETYSWITCHON  = 0x00000100,
    DIGFFS_SAFETYSWITCHOFF = 0x00000200,
    DIGFFS_USERFFSWITCHON  = 0x00000400,
    DIGFFS_USERFFSWITCHOFF = 0x00000800,
    DIGFFS_DEVICELOST      = 0x80000000
};

struct DIJOYSTATE
{
    LONG lX;
    LONG lY;
    LONG lZ;
    LONG lRx;
    LONG lRy;
    LONG lRz;
    LONG rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[32];
};

struct IDirectInputDevice::DIPROPHEADER
{
    DWORD dwSize;
    DWORD dwHeaderSize;
    DWORD dwObj;
    DWORD dwHow;
};

struct IDirectInputDevice::DIPROPRANGE
{
    IDirectInputDevice::DIPROPHEADER diph;
    LONG lMin;
    LONG lMax;
};

struct IDirectInputDevice::DIPROPPOINTER
{
    IDirectInputDevice::DIPROPHEADER    diph;
    Packed<UINT>    uData;
};

struct IDirectInputDevice::DIPROPDWORD
{
    IDirectInputDevice::DIPROPHEADER diph;
    DWORD dwData;
};

// Joystick axis slot (controls::Axis) addressed by dwObj/dwHow, or -1 if the joystick lacks it.
static int findAxis(DWORD obj, DWORD how)
{
    x86::reg32 axis;
    if (how == DIPH_BYOFFSET && obj % sizeof(LONG) == 0)
        axis = obj / sizeof(LONG);
    else if (how == DIPH_BYID && (obj & DIDFT_AXIS))
        axis = (obj >> 8) & 0xffff;
    else
        return -1;
    return axis < controls::AxisCount && controls::axisPresent(controls::Axis(axis)) ? int(axis) : -1;
}

// Joystick button addressed by dwObj/dwHow, or -1.
static int findButton(DWORD obj, DWORD how)
{
    const DWORD firstOffset = offsetof(DIJOYSTATE, rgbButtons);
    x86::reg32 button;
    if (how == DIPH_BYOFFSET && obj >= firstOffset)
        button = obj - firstOffset;
    else if (how == DIPH_BYID && (obj & DIDFT_BUTTON))
        button = (obj >> 8) & 0xffff;
    else
        return -1;
    return button < controls::joystickButtonCount() ? int(button) : -1;
}

// Axes a property header addresses (bit per controls::Axis); 0 if the joystick has no such object. The game's
// presence test for an axis is whether setting its range succeeds.
static uint32_t propertyAxes(DWORD obj, DWORD how)
{
    if (how == DIPH_DEVICE)
    {
        if (obj != 0)
            return 0;
        uint32_t axes = 0;
        for (int axis = 0; axis < controls::AxisCount; ++axis)
        {
            if (controls::axisPresent(controls::Axis(axis)))
                axes |= uint32_t(1) << axis;
        }
        return axes;
    }
    const int axis = findAxis(obj, how);
    return axis >= 0 ? uint32_t(1) << axis : 0;
}

static int firstAxis(uint32_t axes)
{
    return __builtin_ctz(axes);
}

static bool isAxisProperty(x86::reg32 property)
{
    return property == DIPROP_RANGE || property == DIPROP_DEADZONE || property == DIPROP_SATURATION;
}

DEFINE_GUID(IID_IDirectInputDevice2A,	0x5944E682,0xC92E,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(IID_IDirectInputDevice8A,	0x54D41080,0xDC15,0x4833,0xA4,0x1B,0x74,0x8F,0x73,0xA3,0x81,0x79);

static const GUID s_sysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
static const GUID s_sysMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
// Joystick instance and product, as IDirectInput2::EnumDevices reports them.
static const GUID s_joystick    = { 0x4251809a, 0x67be, 0, "VITAPAD" };

// Object type GUIDs: GUID_XAxis .. GUID_Slider and GUID_Button are A36D02xx-C9F3-11CF, GUID_Key 55728220-D33C-11CF.
static const x86::reg32 s_axisGuids[controls::AxisCount] =
    { 0xA36D02E0, 0xA36D02E1, 0xA36D02E2, 0xA36D02F4, 0xA36D02F5, 0xA36D02E3, 0xA36D02E4, 0xA36D02E4 };
static const x86::reg32 s_buttonGuid = 0xA36D02F0;
static const x86::reg32 s_keyGuid = 0x55728220;

static GUID objectGuid(x86::reg32 data1)
{
    return GUID{ data1, x86::reg16(data1 == s_keyGuid ? 0xD33C : 0xC9F3), 0x11CF,
                 { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
}

IDirectInputDevice::IDirectInputDevice(Input* input, bool version8)
    :   IUnknown(version8 ? IID_IDirectInputDevice8A : IID_IDirectInputDevice2A)
{
    m_vtable = version8 ? s_iDirectInputDevice8Vtable : s_iDirectInputDeviceVtable;
    m_resource = input;
}

bool IDirectInputDevice::isVersion8() const
{
    return m_interface == IID_IDirectInputDevice8A;
}

x86::reg32 IDirectInputDevice::deviceType() const
{
    const Input* input = static_cast<const Input*>(m_resource);
    return isVersion8() ? input->getDeviceType8() : input->getDeviceType();
}

// The game passes the DirectX 5 size (0x2C); a smaller structure gets its fields only.
HRESULT IDirectInputDevice::GetCapabilities(WinApplication* app, x86::CPU& cpu,
                                            LPDIDEVCAPS lpDIDevCaps)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpDIDevCaps || lpDIDevCaps->dwSize < 6 * sizeof(DWORD) || lpDIDevCaps->dwSize > sizeof(DIDEVCAPS))
        return DIERR_INVALIDPARAM;
    const Input* input = static_cast<Input*>(m_resource);
    DIDEVCAPS caps;
    memset(&caps, 0, sizeof(caps));
    caps.dwSize = lpDIDevCaps->dwSize;
    // Not DIDC_FORCEFEEDBACK: the game then leaves force feedback off.
    caps.dwFlags = DIDC_ATTACHED | (dynamic_cast<const Gamepad*>(input) ? DIDC_POLLEDDATAFORMAT : 0);
    caps.dwDevType = deviceType();
    caps.dwAxes = input->getAxesCount();
    caps.dwButtons = input->getButtonCount();
    caps.dwPOVs = dynamic_cast<const Gamepad*>(input) ? 1 : 0;
    memcpy(lpDIDevCaps, &caps, caps.dwSize);
    return DI_OK;
}

// Objects in data format order: joystick axes then buttons (DIJOYSTATE offsets), mouse axes then buttons
// (DIMOUSESTATE), keyboard keys 1-255 (offset = key code). The instance number in dwType is the axis slot, button or
// key, as GetObjectInfo and the properties address them. No object is a force feedback actuator.
HRESULT IDirectInputDevice::EnumObjects(WinApplication* app, x86::CPU& cpu,
                                        LPDIENUMDEVICEOBJECTSCALLBACK lpCallback, x86::reg32 pvRef, DWORD dwFlags)
{
    static const DWORD DIDFT_RELAXIS = 0x01;
    static const DWORD DIDFT_TYPEMASK = 0x1f;
    static const DWORD DIDFT_FFACTUATOR = 0x01000000;
    static const DWORD DIDOI_ASPECTPOSITION = 0x100;
    if (!lpCallback)
        return DIERR_INVALIDPARAM;
    if (dwFlags & DIDFT_FFACTUATOR)
        return DI_OK;
    struct Object
    {
        x86::reg32  guid;
        DWORD       offset;
        DWORD       type;
        DWORD       flags;
        char        name[32];
    };
    std::vector<Object> objects;
    const Input* input = static_cast<const Input*>(m_resource);
    if (dynamic_cast<const Gamepad*>(input))
    {
        static const char* const names[controls::AxisCount] =
            { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider" };
        for (int axis = 0; axis < controls::AxisCount; ++axis)
        {
            if (!controls::axisPresent(controls::Axis(axis)))
                continue;
            Object object = { s_axisGuids[axis], DWORD(axis * sizeof(LONG)), DIDFT_ABSAXIS | (DWORD(axis) << 8),
                              DIDOI_ASPECTPOSITION, {} };
            const char* name = controls::axisName(controls::Axis(axis));
            snprintf(object.name, sizeof(object.name), "%s", name ? name : names[axis]);
            objects.push_back(object);
        }
        Object hat = { 0xA36D02F2, DWORD(offsetof(DIJOYSTATE, rgdwPOV)), 0x10, 0, "Hat Switch" };   // DIDFT_POV
        objects.push_back(hat);
        for (x86::reg32 button = 0; button < controls::joystickButtonCount(); ++button)
        {
            Object object = { s_buttonGuid, DWORD(offsetof(DIJOYSTATE, rgbButtons) + button),
                              DIDFT_PSHBUTTON | (button << 8), 0, {} };
            if (const char* name = controls::buttonName(button))
                snprintf(object.name, sizeof(object.name), "%s", name);
            else
                snprintf(object.name, sizeof(object.name), "Button %u", unsigned(button + 1));
            objects.push_back(object);
        }
    }
    else if (dynamic_cast<const Mouse*>(input))
    {
        static const char* const names[3] = { "X Axis", "Y Axis", "Wheel" };
        for (DWORD axis = 0; axis < 3; ++axis)
        {
            Object object = { s_axisGuids[axis], DWORD(axis * sizeof(LONG)), DIDFT_RELAXIS | (axis << 8), 0, {} };
            snprintf(object.name, sizeof(object.name), "%s", names[axis]);
            objects.push_back(object);
        }
        for (DWORD button = 0; button < input->getButtonCount(); ++button)
        {
            Object object = { s_buttonGuid, 12 + button, DIDFT_PSHBUTTON | (button << 8), 0, {} };
            snprintf(object.name, sizeof(object.name), "Button %u", unsigned(button));
            objects.push_back(object);
        }
    }
    else
    {
        for (DWORD key = 1; key < 256; ++key)
        {
            Object object = { s_keyGuid, key, DIDFT_PSHBUTTON | (key << 8), 0, {} };
            snprintf(object.name, sizeof(object.name), "Key 0x%02x", unsigned(key));
            objects.push_back(object);
        }
    }
    MemMap memory(sizeof(DIDEVICEOBJECTINSTANCE));
    const x86::reg32 instance = memory.getBlockStart();
    DIDEVICEOBJECTINSTANCE* info = &app->getMemory<DIDEVICEOBJECTINSTANCE>(instance);
    const DWORD wanted = dwFlags & DIDFT_TYPEMASK;
    for (const Object& object : objects)
    {
        if (wanted && !(object.type & wanted))
            continue;
        memset(info, 0, sizeof(*info));
        info->dwSize = sizeof(*info);
        info->guidType = objectGuid(object.guid);
        info->dwOfs = object.offset;
        info->dwType = object.type;
        info->dwFlags = object.flags;
        snprintf(info->tszName, sizeof(info->tszName), "%s", object.name);
        // BOOL CALLBACK (LPCDIDEVICEOBJECTINSTANCE, LPVOID), stdcall.
        cpu.esp -= 12;
        app->getMemory<x86::reg32>(cpu.esp + 0) = lpCallback;
        app->getMemory<x86::reg32>(cpu.esp + 4) = instance;
        app->getMemory<x86::reg32>(cpu.esp + 8) = pvRef;
        app->dynamic_call(lpCallback, cpu);
        if (!cpu.eax)
            break;
    }
    return DI_OK;
}

// Joystick axis range, dead zone and saturation; the device-wide form reports the first axis.
HRESULT IDirectInputDevice::GetProperty(WinApplication* app, x86::CPU& cpu,
                                        Packed<const GUID> rguidProp, LPDIPROPHEADER pdiph)
{
    RC_USE(app);
    RC_USE(cpu);
    Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource);
    const x86::reg32 property = rguidProp;
    if (!gamepad || !isAxisProperty(property))
        return DIERR_UNSUPPORTED;
    const DWORD size = property == DIPROP_RANGE ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD);
    if (!pdiph || pdiph->dwHeaderSize != sizeof(DIPROPHEADER) || pdiph->dwSize != size)
        return DIERR_INVALIDPARAM;
    const uint32_t axes = propertyAxes(pdiph->dwObj, pdiph->dwHow);
    if (!axes)
        return DIERR_OBJECTNOTFOUND;
    const Gamepad::AxisProperties& axis = gamepad->axisProperties(controls::Axis(firstAxis(axes)));
    if (property == DIPROP_RANGE)
    {
        DIPROPRANGE* range = reinterpret_cast<DIPROPRANGE*>(pdiph);
        range->lMin = axis.min;
        range->lMax = axis.max;
    }
    else
    {
        reinterpret_cast<DIPROPDWORD*>(pdiph)->dwData = property == DIPROP_DEADZONE ? axis.deadZone : axis.saturation;
    }
    return DI_OK;
}

// Joystick axis range, dead zone and saturation; other properties (DIPROP_AUTOCENTER) are accepted and ignored.
HRESULT IDirectInputDevice::SetProperty(WinApplication* app, x86::CPU& cpu,
                                        Packed<const GUID> rguidProp, LPCDIPROPHEADER pdiph)
{
    RC_USE(app);
    RC_USE(cpu);
    Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource);
    const x86::reg32 property = rguidProp;
    if (!gamepad || !isAxisProperty(property))
        return DI_OK;
    const DWORD size = property == DIPROP_RANGE ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD);
    if (!pdiph || pdiph->dwHeaderSize != sizeof(DIPROPHEADER) || pdiph->dwSize != size)
        return DIERR_INVALIDPARAM;
    const uint32_t axes = propertyAxes(pdiph->dwObj, pdiph->dwHow);
    if (!axes)
        return DIERR_OBJECTNOTFOUND;
    const LONG min = property == DIPROP_RANGE ? reinterpret_cast<const DIPROPRANGE*>(pdiph)->lMin : 0;
    const LONG max = property == DIPROP_RANGE ? reinterpret_cast<const DIPROPRANGE*>(pdiph)->lMax : 0;
    const DWORD value = property == DIPROP_RANGE ? 0 : reinterpret_cast<const DIPROPDWORD*>(pdiph)->dwData;
    if (property == DIPROP_RANGE ? min >= max : value > s_propertyUnits)
        return DIERR_INVALIDPARAM;
    for (int axis = 0; axis < controls::AxisCount; ++axis)
    {
        if (!(axes & (uint32_t(1) << axis)))
            continue;
        Gamepad::AxisProperties& properties = gamepad->axisProperties(controls::Axis(axis));
        if (property == DIPROP_RANGE)
        {
            properties.min = min;
            properties.max = max;
        }
        else
        {
            (property == DIPROP_DEADZONE ? properties.deadZone : properties.saturation) = value;
        }
    }
    return DI_OK;
}

HRESULT IDirectInputDevice::Acquire(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

HRESULT IDirectInputDevice::Unacquire(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

// Joystick: DIJOYSTATE (the DIJOYSTATE2 extension reads zero), hat 0 from the d-pad. Keyboard: 256 key bytes.
// Mouse: no motion or buttons.
HRESULT IDirectInputDevice::GetDeviceState(WinApplication* app, x86::CPU& cpu,
                                           DWORD cbData, LPVOID lpvData)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpvData)
        return DIERR_INVALIDPARAM;
    if (Gamepad* gamepad = dynamic_cast<Gamepad*>(m_resource))
    {
        if (cbData < sizeof(DIJOYSTATE))
            return DIERR_INVALIDPARAM;
        x86::reg32 buttons;
        x86::sreg32 axes[controls::AxisCount];
        int hat;
        gamepad->getState(&buttons, axes, &hat);
        memset(lpvData, 0, cbData);
        DIJOYSTATE* state = reinterpret_cast<DIJOYSTATE*>(lpvData);
        state->lX = axes[controls::AxisX];
        state->lY = axes[controls::AxisY];
        state->lZ = axes[controls::AxisZ];
        state->lRx = axes[controls::AxisRx];
        state->lRy = axes[controls::AxisRy];
        state->lRz = axes[controls::AxisRz];
        state->rglSlider[0] = axes[controls::AxisSlider0];
        state->rglSlider[1] = axes[controls::AxisSlider1];
        for (DWORD& pov : state->rgdwPOV)
            pov = 0xffffffff;
        state->rgdwPOV[0] = hat < 0 ? 0xffffffff : DWORD(hat);
        for (x86::reg32 button = 0; button < controls::joystickButtonCount(); ++button)
            state->rgbButtons[button] = (buttons & (x86::reg32(1) << button)) ? 0x80 : 0x00;
        return DI_OK;
    }
    if (dynamic_cast<Keyboard*>(m_resource))
    {
        if (cbData < 256)
            return DIERR_INVALIDPARAM;
        x86::reg8 keys[256];
        Keyboard::getState(keys);
        memset(lpvData, 0, cbData);
        memcpy(lpvData, keys, sizeof(keys));
        return DI_OK;
    }
    memset(lpvData, 0, cbData);
    return DI_OK;
}

HRESULT IDirectInputDevice::GetDeviceData(WinApplication* app, x86::CPU& cpu,
                                          DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(cbObjectData);
    RC_USE(rgdod);
    RC_USE(dwFlags);
    if (pdwInOut)
        *pdwInOut = 0;
    return DI_OK;
}

// GetDeviceState writes the standard layouts: c_dfDIJoystick / c_dfDIJoystick2 (80 / 272 bytes), c_dfDIMouse /
// c_dfDIMouse2 (16 / 20) and c_dfDIKeyboard (256), which are the game's formats. Another layout would need the
// object list mapped and traps.
HRESULT IDirectInputDevice::SetDataFormat(WinApplication* app, x86::CPU& cpu,
                                          LPCDIDATAFORMAT lpdf)
{
    if (!lpdf || lpdf->dwSize != sizeof(DIDATAFORMAT) || lpdf->dwObjSize != sizeof(DIOBJECTDATAFORMAT))
        return DIERR_INVALIDPARAM;
    const Input* input = static_cast<const Input*>(m_resource);
    const DWORD size = lpdf->dwDataSize;
    const bool standard = dynamic_cast<const Gamepad*>(input) ? size == sizeof(DIJOYSTATE) || size == 272
                        : dynamic_cast<const Mouse*>(input) ? size == 16 || size == 20
                        : size == 256;
    if (!standard)
        unsupported(app, cpu, "IDirectInputDevice::SetDataFormat", "custom data format of %u bytes, %u objects",
                    unsigned(size), unsigned(lpdf->dwNumObjs));
    return DI_OK;
}

HRESULT IDirectInputDevice::SetEventNotification(WinApplication* app, x86::CPU& cpu,
                                                 HANDLE hEvent)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hEvent);
    return 0;
}

HRESULT IDirectInputDevice::SetCooperativeLevel(WinApplication* app, x86::CPU& cpu,
                                                HWND hwnd, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hwnd);
    RC_USE(dwFlags);
    return 0;
}

HRESULT IDirectInputDevice::GetObjectInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIDEVICEOBJECTINSTANCE pdidoi, DWORD dwObj, DWORD dwHow)
{
    RC_USE(app);
    RC_USE(cpu);
    // The DirectX 3 size ends after tszName.
    static const DWORD s_dx3Size = offsetof(DIDEVICEOBJECTINSTANCE, dwFFMaxForce);
    if (!pdidoi || (pdidoi->dwSize != sizeof(DIDEVICEOBJECTINSTANCE) && pdidoi->dwSize != s_dx3Size))
        return DIERR_INVALIDPARAM;
    const DWORD size = pdidoi->dwSize;
    memset(pdidoi, 0, size);
    pdidoi->dwSize = size;
    if (!dynamic_cast<Gamepad*>(m_resource))
        return DI_OK;
    const int axis = findAxis(dwObj, dwHow);
    if (axis >= 0)
    {
        static const char* const names[controls::AxisCount] =
            { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider" };
        pdidoi->dwOfs = DWORD(axis) * sizeof(LONG);
        pdidoi->dwType = DIDFT_ABSAXIS | (DWORD(axis) << 8);
        const char* name = controls::axisName(controls::Axis(axis));
        snprintf(pdidoi->tszName, sizeof(pdidoi->tszName), "%s", name ? name : names[axis]);
        return DI_OK;
    }
    const int button = findButton(dwObj, dwHow);
    if (button >= 0)
    {
        pdidoi->dwOfs = offsetof(DIJOYSTATE, rgbButtons) + DWORD(button);
        pdidoi->dwType = DIDFT_PSHBUTTON | (DWORD(button) << 8);
        if (const char* name = controls::buttonName(x86::reg32(button)))
            snprintf(pdidoi->tszName, sizeof(pdidoi->tszName), "%s", name);
        else
            snprintf(pdidoi->tszName, sizeof(pdidoi->tszName), "Button %d", button + 1);
        return DI_OK;
    }
    return DIERR_OBJECTNOTFOUND;
}

// The game passes the DirectX 3 size (0x230, without guidFFDriver and the usage fields); nothing beyond dwSize is
// written.
HRESULT IDirectInputDevice::GetDeviceInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIDEVICEINSTANCE pdidi)
{
    RC_USE(app);
    RC_USE(cpu);
    static const DWORD s_dx3Size = offsetof(DIDEVICEINSTANCE, guidFFDriver);
    if (!pdidi || (pdidi->dwSize != s_dx3Size && pdidi->dwSize != sizeof(DIDEVICEINSTANCE)))
        return DIERR_INVALIDPARAM;
    const Input* input = static_cast<Input*>(m_resource);
    DIDEVICEINSTANCE info;
    memset(&info, 0, sizeof(info));
    info.dwSize = pdidi->dwSize;
    info.dwDevType = deviceType();
    const char* name;
    if (const Gamepad* gamepad = dynamic_cast<const Gamepad*>(input))
    {
        info.guidInstance = s_joystick;
        info.guidInstance.Data3 = x86::reg16(gamepad->index());
        info.guidProduct = controls::JoystickProduct;
        name = controls::JoystickName;
    }
    else if (dynamic_cast<const Keyboard*>(input))
    {
        info.guidInstance = s_sysKeyboard;
        info.guidProduct = s_sysKeyboard;
        name = "Keyboard";
    }
    else
    {
        info.guidInstance = s_sysMouse;
        info.guidProduct = s_sysMouse;
        name = "Mouse";
    }
    snprintf(info.tszInstanceName, sizeof(info.tszInstanceName), "%s", name);
    snprintf(info.tszProductName, sizeof(info.tszProductName), "%s", name);
    memcpy(pdidi, &info, info.dwSize);
    return DI_OK;
}

HRESULT IDirectInputDevice::RunControlPanel(WinApplication* app, x86::CPU& cpu,
                                            HWND hwndOwner, DWORD dwFlags)
{
    RC_USE(hwndOwner);
    unsupported(app, cpu, "IDirectInputDevice::RunControlPanel", "flags 0x%x", unsigned(dwFlags));
}

HRESULT IDirectInputDevice::Initialize(WinApplication* app, x86::CPU& cpu,
                                       HINSTANCE hinst, DWORD dwVersion, REFGUID rguid)
{
    RC_USE(hinst);
    RC_USE(rguid);
    unsupported(app, cpu, "IDirectInputDevice::Initialize", "version 0x%x", unsigned(dwVersion));
}

// The devices have no force feedback actuators (EnumObjects, GetCapabilities), so no effect can be made.
HRESULT IDirectInputDevice::CreateEffect(WinApplication* app, x86::CPU& cpu,
                                         REFGUID rguid, LPCDIEFFECT lpeff, Packed<IUnknown>* ppdeff, LPUNKNOWN punkOuter)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rguid);
    RC_USE(lpeff);
    RC_USE(punkOuter);
    if (ppdeff)
        *ppdeff = Packed<IUnknown>(0);
    return DIERR_UNSUPPORTED;
}

HRESULT IDirectInputDevice::EnumEffects(WinApplication* app, x86::CPU& cpu,
                                        LPDIENUMEFFECTSCALLBACK lpCallback, LPVOID pvRef, DWORD dwEffType)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpCallback);
    RC_USE(pvRef);
    RC_USE(dwEffType);
    // No effects: the game leaves force feedback off.
    return DI_OK;
}

HRESULT IDirectInputDevice::GetEffectInfo(WinApplication* app, x86::CPU& cpu,
                                          LPDIEffectInfo pdei, REFGUID rguid)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pdei);
    RC_USE(rguid);
    return DIERR_UNSUPPORTED;
}

HRESULT IDirectInputDevice::GetForceFeedbackState(WinApplication* app, x86::CPU& cpu,
                                                  LPDWORD pdwOut)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pdwOut);
    return DIERR_UNSUPPORTED;
}

HRESULT IDirectInputDevice::SendForceFeedbackCommand(WinApplication* app, x86::CPU& cpu,
                                                     DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    return 0;
}

HRESULT IDirectInputDevice::EnumCreatedEffectObjects(WinApplication* app, x86::CPU& cpu,
                                                     LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl)
{
    // No effects were created: nothing to enumerate.
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpCallback);
    RC_USE(pvRef);
    RC_USE(fl);
    return DI_OK;
}

HRESULT IDirectInputDevice::Escape(WinApplication* app, x86::CPU& cpu,
                                   LPDIEFFESCAPE pesc)
{
    unsupported(app, cpu, "IDirectInputDevice::Escape", "command 0x%x", pesc ? unsigned(pesc->dwCommand) : 0u);
}

HRESULT IDirectInputDevice::Poll(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    // Short host lock that is never held while waiting for the execution lock.
    Gamepad::updateKeys();
    return 0;
}

HRESULT IDirectInputDevice::SendDeviceData(WinApplication* app, x86::CPU& cpu,
                                           DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl)
{
    RC_USE(rgdod);
    RC_USE(pdwInOut);
    unsupported(app, cpu, "IDirectInputDevice::SendDeviceData", "%u-byte objects, flags 0x%x",
                unsigned(cbObjectData), unsigned(fl));
}

HRESULT IDirectInputDevice::EnumEffectsInFile(WinApplication* app, x86::CPU& cpu,
                                              LPCSTR lpszFileName, x86::reg32 pec, LPVOID pvRef, DWORD dwFlags)
{
    RC_USE(pec);
    RC_USE(pvRef);
    RC_USE(dwFlags);
    unsupported(app, cpu, "IDirectInputDevice8::EnumEffectsInFile", "%s", lpszFileName ? lpszFileName : "");
}

HRESULT IDirectInputDevice::WriteEffectToFile(WinApplication* app, x86::CPU& cpu,
                                              LPCSTR lpszFileName, DWORD dwEntries, x86::reg32 rgDiFileEft,
                                              DWORD dwFlags)
{
    RC_USE(dwEntries);
    RC_USE(rgDiFileEft);
    RC_USE(dwFlags);
    unsupported(app, cpu, "IDirectInputDevice8::WriteEffectToFile", "%s", lpszFileName ? lpszFileName : "");
}

HRESULT IDirectInputDevice::BuildActionMap(WinApplication* app, x86::CPU& cpu,
                                           x86::reg32 lpdiaf, LPCSTR lpszUserName, DWORD dwFlags)
{
    RC_USE(lpdiaf);
    RC_USE(lpszUserName);
    unsupported(app, cpu, "IDirectInputDevice8::BuildActionMap", "flags 0x%x", unsigned(dwFlags));
}

HRESULT IDirectInputDevice::SetActionMap(WinApplication* app, x86::CPU& cpu,
                                         x86::reg32 lpdiaf, LPCSTR lpszUserName, DWORD dwFlags)
{
    RC_USE(lpdiaf);
    RC_USE(lpszUserName);
    unsupported(app, cpu, "IDirectInputDevice8::SetActionMap", "flags 0x%x", unsigned(dwFlags));
}

HRESULT IDirectInputDevice::GetImageInfo(WinApplication* app, x86::CPU& cpu,
                                         x86::reg32 lpdiDevImageInfoHeader)
{
    unsupported(app, cpu, "IDirectInputDevice8::GetImageInfo", "header 0x%08x", lpdiDevImageInfoHeader);
}

x86::reg32 IDirectInputDevice::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400048, {"GetCapabilities", &Wrapper<decltype(&IDirectInputDevice::GetCapabilities), &IDirectInputDevice::GetCapabilities>::stdcall});
    app->registerMethod(0x400049, {"EnumObjects", &Wrapper<decltype(&IDirectInputDevice::EnumObjects), &IDirectInputDevice::EnumObjects>::stdcall});
    app->registerMethod(0x40004a, {"GetProperty", &Wrapper<decltype(&IDirectInputDevice::GetProperty), &IDirectInputDevice::GetProperty>::stdcall});
    app->registerMethod(0x40004b, {"SetProperty", &Wrapper<decltype(&IDirectInputDevice::SetProperty), &IDirectInputDevice::SetProperty>::stdcall});
    app->registerMethod(0x40004c, {"Acquire", &Wrapper<decltype(&IDirectInputDevice::Acquire), &IDirectInputDevice::Acquire>::stdcall});
    app->registerMethod(0x40004d, {"Unacquire", &Wrapper<decltype(&IDirectInputDevice::Unacquire), &IDirectInputDevice::Unacquire>::stdcall});
    app->registerMethod(0x40004e, {"GetDeviceState", &Wrapper<decltype(&IDirectInputDevice::GetDeviceState), &IDirectInputDevice::GetDeviceState>::stdcall});
    app->registerMethod(0x40004f, {"GetDeviceData", &Wrapper<decltype(&IDirectInputDevice::GetDeviceData), &IDirectInputDevice::GetDeviceData>::stdcall});
    app->registerMethod(0x400050, {"SetDataFormat", &Wrapper<decltype(&IDirectInputDevice::SetDataFormat), &IDirectInputDevice::SetDataFormat>::stdcall});
    app->registerMethod(0x400051, {"SetEventNotification", &Wrapper<decltype(&IDirectInputDevice::SetEventNotification), &IDirectInputDevice::SetEventNotification>::stdcall});
    app->registerMethod(0x400052, {"SetCooperativeLevel", &Wrapper<decltype(&IDirectInputDevice::SetCooperativeLevel), &IDirectInputDevice::SetCooperativeLevel>::stdcall});
    app->registerMethod(0x400053, {"GetObjectInfo", &Wrapper<decltype(&IDirectInputDevice::GetObjectInfo), &IDirectInputDevice::GetObjectInfo>::stdcall});
    app->registerMethod(0x400054, {"GetDeviceInfo", &Wrapper<decltype(&IDirectInputDevice::GetDeviceInfo), &IDirectInputDevice::GetDeviceInfo>::stdcall});
    app->registerMethod(0x400055, {"RunControlPanel", &Wrapper<decltype(&IDirectInputDevice::RunControlPanel), &IDirectInputDevice::RunControlPanel>::stdcall});
    app->registerMethod(0x400056, {"Initialize", &Wrapper<decltype(&IDirectInputDevice::Initialize), &IDirectInputDevice::Initialize>::stdcall});
    app->registerMethod(0x400057, {"CreateEffect", &Wrapper<decltype(&IDirectInputDevice::CreateEffect), &IDirectInputDevice::CreateEffect>::stdcall});
    app->registerMethod(0x400058, {"EnumEffects", &Wrapper<decltype(&IDirectInputDevice::EnumEffects), &IDirectInputDevice::EnumEffects>::stdcall});
    app->registerMethod(0x400059, {"GetEffectInfo", &Wrapper<decltype(&IDirectInputDevice::GetEffectInfo), &IDirectInputDevice::GetEffectInfo>::stdcall});
    app->registerMethod(0x40005a, {"GetForceFeedbackState", &Wrapper<decltype(&IDirectInputDevice::GetForceFeedbackState), &IDirectInputDevice::GetForceFeedbackState>::stdcall});
    app->registerMethod(0x40005b, {"SendForceFeedbackCommand", &Wrapper<decltype(&IDirectInputDevice::SendForceFeedbackCommand), &IDirectInputDevice::SendForceFeedbackCommand>::stdcall});
    app->registerMethod(0x40005c, {"EnumCreatedEffectObjects", &Wrapper<decltype(&IDirectInputDevice::EnumCreatedEffectObjects), &IDirectInputDevice::EnumCreatedEffectObjects>::stdcall});
    app->registerMethod(0x40005d, {"Escape", &Wrapper<decltype(&IDirectInputDevice::Escape), &IDirectInputDevice::Escape>::stdcall});
    app->registerMethod(0x40005e, {"Poll", &Wrapper<decltype(&IDirectInputDevice::Poll), &IDirectInputDevice::Poll>::stdcall});
    app->registerMethod(0x40005f, {"SendDeviceData", &Wrapper<decltype(&IDirectInputDevice::SendDeviceData), &IDirectInputDevice::SendDeviceData>::stdcall});

    s_iDirectInputDeviceVtable = memblockIndex;
    vtable[0] = 0x400000;    // IUnknown_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknwon_Release
    vtable[3] = 0x400048;    // IDirectInputDevice_GetCapabilities
    vtable[4] = 0x400049;    // IDirectInputDevice_EnumObjects
    vtable[5] = 0x40004a;    // IDirectInputDevice_GetProperty
    vtable[6] = 0x40004b;    // IDirectInputDevice_SetProperty
    vtable[7] = 0x40004c;    // IDirectInputDevice_Acquire
    vtable[8] = 0x40004d;    // IDirectInputDevice_Unacquire
    vtable[9] = 0x40004e;    // IDirectInputDevice_GetDeviceState
    vtable[10] = 0x40004f;    // IDirectInputDevice_GetDeviceData
    vtable[11] = 0x400050;    // IDirectInputDevice_SetDataFormat
    vtable[12] = 0x400051;    // IDirectInputDevice_SetEventNotification
    vtable[13] = 0x400052;    // IDirectInputDevice_SetCooperativeLevel
    vtable[14] = 0x400053;    // IDirectInputDevice_GetObjectInfo
    vtable[15] = 0x400054;    // IDirectInputDevice_GetDeviceInfo
    vtable[16] = 0x400055;    // IDirectInputDevice_RunControlPanel
    vtable[17] = 0x400056;    // IDirectInputDevice_Initialize
    vtable[18] = 0x400057;    // IDirectInputDevice_CreateEffect
    vtable[19] = 0x400058;    // IDirectInputDevice_EnumEffects
    vtable[20] = 0x400059;    // IDirectInputDevice_GetEffectInfo
    vtable[21] = 0x40005a;    // IDirectInputDevice_GetForceFeedbackState
    vtable[22] = 0x40005b;    // IDirectInputDevice_SendForceFeedbackCommand
    vtable[23] = 0x40005c;    // IDirectInputDevice_EnumCreatedEffectObjects
    vtable[24] = 0x40005d;    // IDirectInputDevice_Escape
    vtable[25] = 0x40005e;    // IDirectInputDevice_Poll
    vtable[26] = 0x40005f;    // IDirectInputDevice_SendDeviceData

    // IDirectInputDevice8A: the same 27 methods, then EnumEffectsInFile .. GetImageInfo.
    app->registerMethod(0x400110, {"IDirectInputDevice8::EnumEffectsInFile", &Wrapper<decltype(&IDirectInputDevice::EnumEffectsInFile), &IDirectInputDevice::EnumEffectsInFile>::stdcall});
    app->registerMethod(0x400111, {"IDirectInputDevice8::WriteEffectToFile", &Wrapper<decltype(&IDirectInputDevice::WriteEffectToFile), &IDirectInputDevice::WriteEffectToFile>::stdcall});
    app->registerMethod(0x400112, {"IDirectInputDevice8::BuildActionMap", &Wrapper<decltype(&IDirectInputDevice::BuildActionMap), &IDirectInputDevice::BuildActionMap>::stdcall});
    app->registerMethod(0x400113, {"IDirectInputDevice8::SetActionMap", &Wrapper<decltype(&IDirectInputDevice::SetActionMap), &IDirectInputDevice::SetActionMap>::stdcall});
    app->registerMethod(0x400114, {"IDirectInputDevice8::GetImageInfo", &Wrapper<decltype(&IDirectInputDevice::GetImageInfo), &IDirectInputDevice::GetImageInfo>::stdcall});
    x86::reg32* vtable8 = vtable + 27;
    s_iDirectInputDevice8Vtable = memblockIndex + 27 * sizeof(x86::reg32);
    for (int i = 0; i < 27; ++i)
        vtable8[i] = vtable[i];
    for (int i = 0; i < 5; ++i)
        vtable8[27 + i] = 0x400110 + x86::reg32(i);
    return (27 + 32) * sizeof(x86::reg32);
}

}}
