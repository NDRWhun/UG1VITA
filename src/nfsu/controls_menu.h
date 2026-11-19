#ifndef NFSU_CONTROLS_MENU_H_
#define NFSU_CONTROLS_MENU_H_

#include <lib/winapp.h>

namespace nfsu { namespace controls_menu {

// Registers the new row's callback before the guest starts. No glyph pack or PC/Vita prompt mode dependency.
void configure(win32::WinApplication& app);
// After the game's Controller-page builder: activate its existing third option row and wire its callback.
void appendRearTouch(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 controller);

}}
#endif
