// Host-side tests with the real VitaSDK declarations and simulated hardware calls.
#include <lib/controls.h>
#include <platform/config.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static SceCtrlData s_pad;
static int s_samplingCalls = 0, s_touchReads = 0;
static SceTouchSamplingState s_samplingState = SCE_TOUCH_SAMPLING_STATE_STOP;
static unsigned s_reports = 1;

extern "C" int sceCtrlPeekBufferPositive(int, SceCtrlData* pad, int)
{
    *pad = s_pad;
    return 1;
}

extern "C" int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state)
{
    assert(port == SCE_TOUCH_PORT_BACK);
    s_samplingState = state;
    ++s_samplingCalls;
    return 0;
}

extern "C" int sceTouchPeek(SceUInt32 port, SceTouchData* touch, SceUInt32)
{
    assert(port == SCE_TOUCH_PORT_BACK);
    ++s_touchReads;
    memset(touch, 0, sizeof(*touch));
    touch->reportNum = s_reports;
    touch->report[0].x = 100; // an accidental touch on the left half
    return 1;
}

namespace pf {
void logInfo(const char*, ...) {}
void logError(const char*, ...) {}
}

int main(int argc, char** argv)
{
    const bool rear = argc > 2 && strcmp(argv[2], "on") == 0;
    pf::Config::load(argc > 1 ? argv[1] : "");
    s_pad.lx = s_pad.ly = s_pad.rx = s_pad.ry = 128;
    s_pad.buttons = SCE_CTRL_UP;
    using namespace win32::controls;
    Mapper mapper;
    PadSample pad = readPad();
    assert(bool(pad.inputs & bit(RearLeft)) == rear);
    assert(s_samplingCalls == (rear ? 1 : 0));
    assert(s_touchReads == (rear ? 1 : 0));
    State state = mapper.update(pad, 0);
    assert(state.buttons & (1u << 12)); // up: shift up in the race map, navigate up in menus
    assert(!(state.buttons & (1u << 14)));
    assert(state.pov == 0);

    s_pad.buttons = SCE_CTRL_DOWN;
    state = mapper.update(readPad(), 1000);
    assert(state.buttons & (1u << 14)); // down: shift down
    assert(!(state.buttons & (1u << 12)));
    assert(state.pov == 18000);
    s_pad.buttons = 0;
    state = mapper.update(readPad(), 2000);
    assert(!(state.buttons & ((1u << 12) | (1u << 14))));
    assert(state.pov == -1);

    float x, y;
    shapeStick(state.axes[AxisZ], state.axes[AxisRz], 0, 10000, &x, &y);
    assert(x == 0 && y == 0); // neutral hardware must not drift
    s_pad.rx = 255;
    s_pad.ry = 0;
    state = mapper.update(readPad(), 3000);
    assert(state.axes[AxisZ] == 1 && state.axes[AxisRz] == -1);
    assert(axisPresent(AxisZ) && axisPresent(AxisRz) && presentAxisCount() == 4);
    shapeStick(state.axes[AxisZ], state.axes[AxisRz], 0, 10000, &x, &y);
    assert(x > 0 && y < 0 && std::fabs(x * x + y * y - 1.0f) < 0.001f);
    s_pad.rx = 0;
    s_pad.ry = 128;
    state = mapper.update(readPad(), 4000);
    assert(state.axes[AxisZ] == -1); // left must survive the mapper too
    shapeStick(state.axes[AxisZ], state.axes[AxisRz], 0, 10000, &x, &y);
    assert(x < -0.99f);

    // Garage input has to activate the car-orbit actions, not just expose a joystick axis.
    CameraState camera = garageCamera(0.0039f, 0.0039f);
    assert(camera.buttons == 0);
    for (uint8_t amount : camera.strength)
        assert(amount == 0);
    camera = garageCamera(1.0f, 0.0f);
    assert(camera.buttons == (1u << CameraButtons[CameraRight]));
    assert(camera.strength[CameraRight] == 208);
    camera = garageCamera(-1.0f, 0.0f);
    assert(camera.buttons == (1u << CameraButtons[CameraLeft]));
    camera = garageCamera(0.0f, -1.0f);
    assert(camera.buttons == (1u << CameraButtons[CameraUp]));
    camera = garageCamera(0.0f, 1.0f);
    assert(camera.buttons == (1u << CameraButtons[CameraDown]));
    camera = garageCamera(0.4f, 0.0f);
    assert(camera.strength[CameraRight] > 0 && camera.strength[CameraRight] < 208);
    camera = garageCamera(1.0f, -1.0f);
    assert(camera.buttons == ((1u << CameraButtons[CameraRight]) | (1u << CameraButtons[CameraUp])));
    assert(camera.strength[CameraRight] == camera.strength[CameraUp]);
    camera = garageCamera(0.0f, 0.0f);
    assert(camera.buttons == 0 && camera.strength[CameraRight] == 0); // release must stop rotation
    // A menu change takes effect immediately. A hand already resting on the rear must lift first.
    assert(setRearTouchEnabled(false));
    const int reads = s_touchReads;
    assert(!(readPad().inputs & (bit(RearLeft) | bit(RearRight))));
    assert(s_touchReads == reads);
    assert(setRearTouchEnabled(true));
    assert(rearTouchEnabled() && s_samplingState == SCE_TOUCH_SAMPLING_STATE_START);
    assert(!(readPad().inputs & bit(RearLeft)));
    s_reports = 0;
    readPad();
    s_reports = 1;
    assert(readPad().inputs & bit(RearLeft));
    assert(setRearTouchEnabled(false));
    assert(!rearTouchEnabled() && s_samplingState == SCE_TOUCH_SAMPLING_STATE_STOP);
    assert(steeringValue(0, 0) == 128 && steeringValue(0.0039f, 0.0039f) == 128);
    assert(steeringValue(-1, 0) == 0 && steeringValue(1, 0) == 255);
    int previous = 128;
    for (float amount : { 0.15f, 0.3f, 0.5f, 0.75f, 1.0f })
    {
        const int right = steeringValue(amount, 0), left = steeringValue(-amount, 0);
        assert(right > previous && left < 128 && right + left >= 255 && right + left <= 256);
        previous = right;
    }
    // Held R survives Resume immediately; the same menu press cannot also trigger nitrous, pause or a shift.
    LayoutFilter filter;
    uint32_t buttons = 1u << 7;
    int pov = -1;
    filter.update(true, false, &buttons, &pov);
    assert(buttons == (1u << 7));
    filter.update(false, true, &buttons, &pov);
    assert(buttons == 0);
    buttons = (1u << 7) | (1u << 6) | (1u << 2) | (1u << 9) | (1u << 12) | (1u << 15);
    pov = 27000;
    filter.update(true, false, &buttons, &pov);
    assert(buttons == ((1u << 7) | (1u << 6) | (1u << 15)) && pov == 27000);
    buttons = 0;
    pov = -1;
    filter.update(true, false, &buttons, &pov);
    buttons = (1u << 2) | (1u << 9) | (1u << 12);
    filter.update(true, false, &buttons, &pov);
    assert(buttons == ((1u << 2) | (1u << 9) | (1u << 12))); // a fresh press remains available
    std::puts(rear ? "controls: rear-touch opt-in passed" : "controls: rear-touch-off and stick/d-pad checks passed");
}
