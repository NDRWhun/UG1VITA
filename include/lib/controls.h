#ifndef LIB_CONTROLS_H_
#define LIB_CONTROLS_H_

#include <x86.h>
#include <winapi/types.h>
#include <cstdint>

namespace win32
{

// Vita controls as the game sees them (docs/re/input.md section 7): one DirectInput 8 game controller presented as
// the EMS USB2 PlayStation 2 adapter, whose buttons follow the game's built-in table for that product, plus key
// messages for text fields. The mapping tables are in controls.cpp.
namespace controls
{

// Vita inputs.
enum Input
{
    Cross, Circle, Square, Triangle, L, R, Select, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    RearLeft, RearRight,    // rear touch, left / right half
    InputCount
};

inline uint32_t bit(Input input) { return uint32_t(1) << input; }

// Keyboard keys the pad sends. Presses are posted in this order and releases in the reverse one (Shift before
// Delete goes down, after it comes up).
enum Key
{
    KeyUp, KeyDown, KeyLeft, KeyRight,
    KeyEscape, KeyEnter, KeyLeftShift, KeyDelete,
    KeyF1, KeyF2, KeyF3,
    KeyCount
};

// A key as WM_KEYDOWN/WM_KEYUP carry it: virtual key, PC/AT set-1 scancode and the extended flag. The DirectInput
// key code (DIK) is the scancode with bit 7 set for an extended key.
struct KeyCode
{
    x86::reg32 vk;
    x86::reg8  scancode;
    bool       extended;

    x86::reg8 dik() const { return x86::reg8(scancode | (extended ? 0x80 : 0)); }
};

const KeyCode& keyCode(Key key);
// Key of a character typed from the on-screen keyboard: letters, digits and space; shift is set for capitals.
// False for other characters.
bool characterKey(char c, KeyCode* key, bool* shift);
const KeyCode& shiftKey();

// DIJOYSTATE axis slots (lX, lY, lZ, lRx, lRy, lRz, two sliders): the joystick has X, Y (left stick), Z (the
// adapter's combined pedal, held centred: the buttons drive throttle and brake) and Rz (right stick Y).
enum Axis
{
    AxisX, AxisY, AxisZ, AxisRx, AxisRy, AxisRz, AxisSlider0, AxisSlider1,
    AxisCount
};

// DirectInput button bits an input reports (0 for none).
uint32_t buttonMask(Input input);

bool axisPresent(Axis axis);
x86::reg32 presentAxisCount();
x86::reg32 joystickButtonCount();

// Object names DirectInput reports for the joystick's buttons and axes (GetObjectInfo, EnumObjects). Null entries,
// and every object before setObjectNames, get the default names. The strings must stay valid.
void setObjectNames(const char* const* buttons, x86::reg32 buttonCount, const char* const* axes);
const char* buttonName(x86::reg32 button);  // null for the default
const char* axisName(Axis axis);            // null for the default

// Product name, DirectInput 7 device type and the product GUID (the EMS adapter's VID/PID GUID).
extern const char* const JoystickName;
extern const x86::reg32  JoystickDeviceType;
extern const GUID        JoystickProduct;

// One reading of the pad and touch panels.
struct PadSample
{
    uint32_t    inputs;             // bit per Input
    float       sticks[4];          // left x, left y, right x, right y: -1..1, y down
};

PadSample readPad();

// What the game gets from one PadSample.
struct State
{
    uint32_t    buttons;            // DirectInput button bits
    float       axes[AxisCount];    // -1..1, absent axes 0; not shaped yet (shapeStick)
    uint32_t    keys;               // bit per Key held
    int         pov;                // hat 0 in hundredths of a degree, -1 centred
    bool        openIme;            // the on-screen keyboard chord was pressed
};

// Turns pad samples into joystick and key state: chords with Select, the Select tap (camera cycle) and buttons held
// across chords. One instance; calls must not overlap.
class Mapper
{
public:
    Mapper();
    State update(const PadSample& pad, uint64_t nowUs);

private:
    uint32_t    m_previous;         // inputs of the last sample
    uint32_t    m_chorded;          // inputs pressed while Select was held, mapped by the chord table until released
    bool        m_selectUsed;       // a chord was used during this Select press
    uint64_t    m_selectPulseEndUs; // the Select tap's button is reported until then
};

// Radial dead zone and response curve of one stick (x, y in -1..1). deadZone and saturation are DirectInput
// property values (0..10000 of full deflection); the larger dead zone and the smaller saturation of these and the
// stick's own apply.
void shapeStick(float x, float y, uint32_t deadZone, uint32_t saturation, float* outX, float* outY);

}

}

#endif
