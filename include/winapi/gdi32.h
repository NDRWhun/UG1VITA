#ifndef gdi32_H_
#define gdi32_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace gdi32
{

int GetDeviceCaps(WinApplication* app, x86::CPU& cpu, HDC hdc, int index);
HGDIOBJ GetStockObject(WinApplication* app, x86::CPU& cpu, int i);
struct TEXTMETRICA;
BOOL GetTextMetricsA(WinApplication* app, x86::CPU& cpu, HDC hdc, TEXTMETRICA* lptm);
HGDIOBJ SelectObject(WinApplication* app, x86::CPU& cpu, HDC hdc, HGDIOBJ h);
BOOL TextOutA(WinApplication* app, x86::CPU& cpu, HDC hdc, int x, int y, LPCSTR lpString, int c);

}}

#endif
