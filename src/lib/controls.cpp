#include <lib/controls.h>
#include <platform/config.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <cmath>
#include <cstring>
#include <iterator>

namespace win32 { namespace controls
{

// The Vita pad is presented as the EMS USB2 PlayStation 2 adapter (VID 0B43, PID 0003): the game has a built-in
// binding table for that product (docs/re/input.md sections 5 and 7), so no binding file is needed.
const char* const JoystickName       = "Vita Gamepad";
const x86::reg32  JoystickDeviceType = 0x0204;      // DIDEVTYPE_JOYSTICK, DIDEVTYPEJOYSTICK_GAMEPAD
const GUID        JoystickProduct    = { 0x00030B43, 0x0000, 0x0000, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };

// ---- Layout (docs/re/input.md section 7) ----

// Joystick buttons by DirectInput index, in the adapter's PlayStation order. src/nfsu/main.cpp sets the race and menu
// actions of these indices (per input map, at each poll). The game's original EMS table: 0 camera / back, 1 look back,
// 2 throttle / select, 3 brake / customise, 4 / 5 shift down / up, 6 nitrous, 7 handbrake, 9 pause,
// 12-15 steer and menu up, right, down, left. Select is a tap (button 8, respot): see the Mapper.
struct ButtonMapping
{
    Input input;
    int   button;
};

static const ButtonMapping s_buttons[] =
{
    { Triangle,  0 },
    { Circle,    1 },
    { Cross,     2 },
    { Square,    3 },
    { RearLeft,  4 },
    { RearRight, 5 },
    { L,         6 },
    { R,         7 },
    { Start,     9 },
    { DpadUp,    12 },
    { DpadRight, 13 },
    { DpadDown,  14 },
    { DpadLeft,  15 },
};

// Button that a short press of Select (no chord) reports: respot.
static const int s_selectTapButton = 8;
static const int s_buttonCount = 16;

// Keys held while an input is held: the d-pad moves the cursor of a text field (WM_KEYDOWN Left / Right).
struct KeyMapping
{
    Input    input;
    uint32_t keys;
};

static constexpr uint32_t keys(Key key) { return uint32_t(1) << key; }

static const KeyMapping s_keys[] =
{
    { DpadLeft,  keys(KeyLeft)   },
    { DpadRight, keys(KeyRight)  },
};

// Select + input. An input pressed while Select is held uses this table until it is released.
struct Chord
{
    Input    input;
    uint32_t keys;
    int      button;    // -1 for none
    bool     ime;       // opens the on-screen keyboard
};

static const Chord s_chords[] =
{
    { Triangle,  0, -1, true  },    // text entry
};

// How long a Select tap holds its button, enough for one game frame at 10 fps.
static const uint64_t s_tapUs = 100000;

// Left and right stick: dead zone and saturation as fractions of full deflection, and the response curve
// y = linear * x + (1 - linear) * x^3 between them (finer aim near the centre).
static const float s_stickDeadZone   = 0.08f;
static const float s_stickSaturation = 0.94f;
static const float s_stickLinear     = 0.6f;

// Horizontal centre of the rear panel (1920x890 starting at y 108).
static const int s_panelCentreX = 960;

// ---- Keys ----

// Order of enum Key.
static const KeyCode s_keyCodes[] =
{
    { 0x26, 0x48, true  },  // up
    { 0x28, 0x50, true  },  // down
    { 0x25, 0x4B, true  },  // left
    { 0x27, 0x4D, true  },  // right
    { 0x1B, 0x01, false },  // escape
    { 0x0D, 0x1C, false },  // enter
    { 0x10, 0x2A, false },  // left shift
    { 0x2E, 0x53, true  },  // delete
    { 0x70, 0x3B, false },  // F1
    { 0x71, 0x3C, false },  // F2
    { 0x72, 0x3D, false },  // F3
};
static_assert(std::size(s_keyCodes) == KeyCount, "s_keyCodes must list every Key");
static_assert(KeyCount <= 32, "keys are a 32-bit set");
static_assert(InputCount <= 32, "inputs are a 32-bit set");

// Set-1 scancodes of A..Z and 0..9.
static const x86::reg8 s_letterScancodes[26] =
{
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
};
static const x86::reg8 s_digitScancodes[10] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };

const KeyCode& keyCode(Key key)
{
    return s_keyCodes[key];
}

const KeyCode& shiftKey()
{
    return s_keyCodes[KeyLeftShift];
}

bool characterKey(char c, KeyCode* key, bool* shift)
{
    *shift = c >= 'A' && c <= 'Z';
    if (c >= 'a' && c <= 'z')
        *key = KeyCode{ x86::reg32('A' + (c - 'a')), s_letterScancodes[c - 'a'], false };
    else if (*shift)
        *key = KeyCode{ x86::reg32(c), s_letterScancodes[c - 'A'], false };
    else if (c >= '0' && c <= '9')
        *key = KeyCode{ x86::reg32(c), s_digitScancodes[c - '0'], false };
    else if (c == ' ')
        *key = KeyCode{ 0x20, 0x39, false };
    else
        return false;
    return true;
}

// ---- Joystick ----

uint32_t buttonMask(Input input)
{
    uint32_t mask = 0;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (mapping.input == input)
            mask |= uint32_t(1) << mapping.button;
    }
    return mask;
}

// The adapter's four axes: X, Y (left stick), Z (combined pedal, held centred) and Rz (right stick Y).
bool axisPresent(Axis axis)
{
    return axis == AxisX || axis == AxisY || axis == AxisZ || axis == AxisRz;
}

x86::reg32 presentAxisCount()
{
    return 4;
}

x86::reg32 joystickButtonCount()
{
    return s_buttonCount;
}

static const char* s_buttonNames[s_buttonCount] = {};
static const char* s_axisNames[AxisCount] = {};

void setObjectNames(const char* const* buttons, x86::reg32 buttonCount, const char* const* axes)
{
    for (x86::reg32 i = 0; i < x86::reg32(s_buttonCount); ++i)
        s_buttonNames[i] = buttons && i < buttonCount ? buttons[i] : nullptr;
    for (int i = 0; i < AxisCount; ++i)
        s_axisNames[i] = axes ? axes[i] : nullptr;
}

const char* buttonName(x86::reg32 button)
{
    return button < x86::reg32(s_buttonCount) ? s_buttonNames[button] : nullptr;
}

const char* axisName(Axis axis)
{
    return axis >= 0 && axis < AxisCount ? s_axisNames[axis] : nullptr;
}

// ---- Pad ----

struct PadButton
{
    uint32_t mask;
    Input    input;
};

static const PadButton s_padButtons[] =
{
    { SCE_CTRL_CROSS,    Cross     },
    { SCE_CTRL_CIRCLE,   Circle    },
    { SCE_CTRL_SQUARE,   Square    },
    { SCE_CTRL_TRIANGLE, Triangle  },
    { SCE_CTRL_LTRIGGER, L         },
    { SCE_CTRL_RTRIGGER, R         },
    { SCE_CTRL_SELECT,   Select    },
    { SCE_CTRL_START,    Start     },
    { SCE_CTRL_UP,       DpadUp    },
    { SCE_CTRL_DOWN,     DpadDown  },
    { SCE_CTRL_LEFT,     DpadLeft  },
    { SCE_CTRL_RIGHT,    DpadRight },
};

static bool s_touchReady = false;
static bool s_rearTouch = true;

// Setting rear_touch (on by default) turns the rear touch inputs off.
static void initTouch()
{
    s_touchReady = true;
    s_rearTouch = pf::Config::getBool("rear_touch", true);
    if (s_rearTouch)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

static float stickValue(unsigned char raw)
{
    return (float(raw) - 127.5f) / 127.5f;
}

PadSample readPad()
{
    if (!s_touchReady)
        initTouch();
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    PadSample sample;
    sample.inputs = 0;
    for (const PadButton& button : s_padButtons)
    {
        if (pad.buttons & button.mask)
            sample.inputs |= bit(button.input);
    }
    sample.sticks[0] = stickValue(pad.lx);
    sample.sticks[1] = stickValue(pad.ly);
    sample.sticks[2] = stickValue(pad.rx);
    sample.sticks[3] = stickValue(pad.ry);

    SceTouchData touch;
    if (s_rearTouch && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 0)
    {
        for (SceUInt32 i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
            sample.inputs |= bit(touch.report[i].x < s_panelCentreX ? RearLeft : RearRight);
    }
    return sample;
}

// ---- Mapper ----

static const Chord* findChord(Input input)
{
    for (const Chord& chord : s_chords)
    {
        if (chord.input == input)
            return &chord;
    }
    return nullptr;
}

Mapper::Mapper()
    :   m_previous(0)
    ,   m_chorded(0)
    ,   m_selectUsed(false)
    ,   m_selectPulseEndUs(0)
{
}

State Mapper::update(const PadSample& pad, uint64_t nowUs)
{
    State state;
    memset(&state, 0, sizeof(state));
    const uint32_t inputs = pad.inputs;
    const uint32_t pressed = inputs & ~m_previous;
    const uint32_t released = m_previous & ~inputs;
    const bool select = (inputs & bit(Select)) != 0;

    if (pressed & bit(Select))
        m_selectUsed = false;
    m_chorded &= inputs;
    for (int input = 0; input < InputCount; ++input)
    {
        if (!(pressed & bit(Input(input))) || input == Select || !select)
            continue;
        const Chord* chord = findChord(Input(input));
        if (!chord)
            continue;
        m_chorded |= bit(Input(input));
        m_selectUsed = true;
        if (chord->ime)
            state.openIme = true;
    }
    if ((released & bit(Select)) && !m_selectUsed)
        m_selectPulseEndUs = nowUs + s_tapUs;
    m_previous = inputs;

    const uint32_t normal = inputs & ~m_chorded;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (normal & bit(mapping.input))
            state.buttons |= uint32_t(1) << mapping.button;
    }
    for (const KeyMapping& mapping : s_keys)
    {
        if (normal & bit(mapping.input))
            state.keys |= mapping.keys;
    }
    for (const Chord& chord : s_chords)
    {
        if (!(m_chorded & bit(chord.input)))
            continue;
        state.keys |= chord.keys;
        if (chord.button >= 0)
            state.buttons |= uint32_t(1) << chord.button;
    }
    if (nowUs < m_selectPulseEndUs)
        state.buttons |= uint32_t(1) << s_selectTapButton;

    state.axes[AxisX] = pad.sticks[0];
    state.axes[AxisY] = pad.sticks[1];
    state.axes[AxisZ] = 0.0f;
    state.axes[AxisRz] = pad.sticks[3];

    // Hat 0 follows the d-pad: hundredths of a degree clockwise from up, -1 when centred.
    const bool up = normal & bit(DpadUp), down = normal & bit(DpadDown);
    const bool left = normal & bit(DpadLeft), right = normal & bit(DpadRight);
    const int dy = (down ? 1 : 0) - (up ? 1 : 0);
    const int dx = (right ? 1 : 0) - (left ? 1 : 0);
    static const int s_angles[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
    state.pov = s_angles[dy + 1][dx + 1];
    return state;
}

// ---- Stick shaping ----

void shapeStick(float x, float y, uint32_t deadZone, uint32_t saturation, float* outX, float* outY)
{
    const float dead = std::fmax(s_stickDeadZone, float(deadZone) / 10000.0f);
    const float full = std::fmin(s_stickSaturation, float(saturation) / 10000.0f);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dead)
    {
        *outX = 0.0f;
        *outY = 0.0f;
        return;
    }
    const float t = full > dead ? std::fmin(1.0f, (magnitude - dead) / (full - dead)) : 1.0f;
    const float scale = (s_stickLinear * t + (1.0f - s_stickLinear) * t * t * t) / magnitude;
    *outX = std::fmax(-1.0f, std::fmin(1.0f, x * scale));
    *outY = std::fmax(-1.0f, std::fmin(1.0f, y * scale));
}

}}
