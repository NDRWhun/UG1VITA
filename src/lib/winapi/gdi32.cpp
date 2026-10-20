#include <winapi/gdi32.h>
#include <x86.h>
#include <platform/log.h>
#include <cstring>

namespace win32 { namespace gdi32
{

// The desktop is the 16-bit Vita display.
int GetDeviceCaps(WinApplication* app, x86::CPU& cpu, HDC hdc, int index)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hdc);
    enum { HORZRES = 8, VERTRES = 10, BITSPIXEL = 12, PLANES = 14 };
    switch (index)
    {
    case HORZRES:
        return 960;
    case VERTRES:
        return 544;
    case BITSPIXEL:
        return 16;
    case PLANES:
        return 1;
    default:
        pf::logError("GetDeviceCaps: index %d not implemented", index);
        return 0;
    }
}

// GDI drawing has no target on the Vita; only the game's debug log window uses it.
struct TEXTMETRICA
{
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
};

// The metrics of an 8x16 fixed font.
BOOL GetTextMetricsA(WinApplication* app, x86::CPU& cpu, HDC hdc, TEXTMETRICA* lptm)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hdc);
    if (!lptm)
        return 0;
    memset(lptm, 0, sizeof(*lptm));
    lptm->tmHeight = 16;
    lptm->tmAscent = 12;
    lptm->tmDescent = 4;
    lptm->tmAveCharWidth = 8;
    lptm->tmMaxCharWidth = 8;
    lptm->tmWeight = 400;
    lptm->tmLastChar = 0xff;
    lptm->tmDefaultChar = '?';
    lptm->tmBreakChar = ' ';
    return 1;
}

HGDIOBJ SelectObject(WinApplication* app, x86::CPU& cpu, HDC hdc, HGDIOBJ h)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hdc);
    RC_USE(h);
    return 0xffffffff;
}

BOOL TextOutA(WinApplication* app, x86::CPU& cpu, HDC hdc, int x, int y, LPCSTR lpString, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hdc);
    RC_USE(x);
    RC_USE(y);
    RC_USE(lpString);
    RC_USE(c);
    return 1;
}

// Stock objects WHITE_BRUSH (0) .. DC_PEN (19) are pseudo handles; nothing draws with them.
static const HGDIOBJ StockObjectBase = 0xfffc0000;

HGDIOBJ GetStockObject(WinApplication* app, x86::CPU& cpu, int i)
{
    RC_USE(app);
    RC_USE(cpu);
    return i >= 0 && i <= 19 ? StockObjectBase + HGDIOBJ(i) : 0;
}

// The port creates no GDI objects; deleting a stock object succeeds and does nothing, as on Windows.
BOOL DeleteObject(WinApplication* app, x86::CPU& cpu, HGDIOBJ ho)
{
    RC_USE(app);
    RC_USE(cpu);
    return ho >= StockObjectBase && ho <= StockObjectBase + 19 ? 1 : 0;
}

}}
