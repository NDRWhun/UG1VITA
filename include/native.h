#ifndef NATIVE_H_
#define NATIVE_H_

// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). The generated entry of
// an overridden function calls RC_NATIVE(address, native::sub_x, sub_x_guest), which runs the native function, or
// hands it to the call hook while one is set (shadow compares that record the calls of a guest body); the
// translated body sub_x_guest is unused.

#include <lib/winapp.h>

namespace rc
{

typedef void (*NativeFunction)(win32::WinApplication* app, x86::CPU& cpu);
typedef void (*NativeHook)(x86::reg32 address, NativeFunction run, win32::WinApplication* app, x86::CPU& cpu);
extern NativeHook g_nativeHook;

}

#define RC_NATIVE(address, native, guest) \
    (__builtin_expect(rc::g_nativeHook != nullptr, 0) ? rc::g_nativeHook(address, &native, app, cpu) \
                                                       : native(app, cpu))

#endif /* !NATIVE_H_ */
