#ifndef NFSU_DISPLAY_H_
#define NFSU_DISPLAY_H_

// Motion Blur display setting (MOTIONBLUR, [0x700E98], saved in the profile with the other display settings): 0 off,
// 1 on, 2 enhanced. The game draws its motion blur for any value other than 0; with 2 the renderer draws it enhanced
// (Device::motionBlurQuad). The Motion Blur row of Customize Display Settings (PC_customize_graphics.fng) cycles
// Off / On / Enhanced (native sub_416a10, sub_4169b0 in display.cpp; the limit of the setting is raised to 2 by
// PATCHES 0x410C3E in generate.py). The default settings (native sub_410a00) have Motion Blur off.
//
// Anti-Aliasing display setting (the BPP setting [0x700E6C], saved in the profile as BPP and read by no game code):
// 2 is msaa 2x, 4 is 4x, any other value off (a profile from before has BPP 32: off). With widescreen=on the
// Resolution row of the Display Settings screen (PC_graphics.fng) is the Anti-Aliasing row and cycles Off / 2x / 4x
// (native sub_415120, sub_4151c0); RES is left as it is. The default settings have it off. The cfg key msaa, when
// the file has it, is used in place of the setting.

namespace nfsu { namespace display
{

// Game thread, before each frame is rendered: the renderer draws the frame's motion blur enhanced when the setting
// is 2, and multisamples with the Anti-Aliasing setting (or cfg msaa) from this frame on.
void frame();

}}

#endif
