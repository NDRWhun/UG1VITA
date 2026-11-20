#ifndef NFSU_DISPLAY_H_
#define NFSU_DISPLAY_H_

// Motion Blur display setting (MOTIONBLUR, [0x700E98], saved in the profile with the other display settings): 0 off,
// 1 on, 2 enhanced. The game draws its motion blur for any value other than 0; with 2 the renderer draws it enhanced
// (Device::motionBlurQuad). The Motion Blur row of Customize Display Settings (PC_customize_graphics.fng) cycles
// Off / On / Enhanced (native sub_416a10, sub_4169b0 in display.cpp; the limit of the setting is raised to 2 by
// PATCHES 0x410C3E in generate.py).

namespace nfsu { namespace display
{

// Game thread, before each frame is rendered: the renderer draws the frame's motion blur enhanced when the setting
// is 2.
void frame();

}}

#endif
