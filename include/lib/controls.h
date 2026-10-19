#ifndef LIB_CONTROLS_H_
#define LIB_CONTROLS_H_

#include <x86.h>
#include <cstdint>

namespace win32
{

// Vita controls as the game sees them (docs/re/input.md section 7): one DirectInput joystick, "Vita Gamepad", whose
// buttons follow the game's own joystick bindings, plus a synthesized keyboard for the keys those bindings lack.
// The mapping tables are in controls.cpp.
namespace controls
{

// Vita inputs.
enum Input
{
    Cross, Circle, Square, Triangle, L, R, Select, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    RearLeft, RearRight,    // rear touch, left / right half
    FrontTop,               // front touch, top strip
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

// DIJOYSTATE axis slots (lX, lY, lZ, lRx, lRy, lRz, two sliders): the joystick has X, Y (left stick) and Rx, Ry
// (right stick). The game's default bindings use only X and Y; with no Z axis its throttle binding stays idle.
enum Axis
{
    AxisX, AxisY, AxisZ, AxisRx, AxisRy, AxisRz, AxisSlider0, AxisSlider1,
    AxisCount
};

bool axisPresent(Axis axis);
x86::reg32 presentAxisCount();
x86::reg32 joystickButtonCount();

// Product name and DIDEVTYPE_JOYSTICK | DIDEVTYPEJOYSTICK_GAMEPAD << 8.
extern const char* const JoystickName;
extern const x86::reg32  JoystickDeviceType;

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
