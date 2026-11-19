#ifndef NFSU_DRIVING_INPUT_H_
#define NFSU_DRIVING_INPUT_H_
#include <lib/winapp.h>
namespace nfsu { namespace driving_input {
void configure(win32::WinApplication& app);
// Guest thread, at pad poll, before DirectInput applies the legacy PC device properties.
void poll(float x, float y, bool driving);
}}
#endif
