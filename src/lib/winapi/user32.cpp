#include <winapi/user32.h>
#include <winapi/support.h>
#include <x86.h>
#include <lib/teb.h>
#include <lib/window.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <string>

namespace win32 { namespace user32
{

// Pseudo handles: the desktop window, the device context of any window (GetDeviceCaps describes the display),
// system cursors and icons (base + resource id).
static const HWND DesktopWindow = 0xffff0001;
static const HDC WindowDC = 0xffff0002;
static const HCURSOR SystemCursorBase = 0xfffe0000;
static const HICON SystemIconBase = 0xfffd0000;

// The display is the Vita screen.
static const int ScreenWidth = 960;
static const int ScreenHeight = 544;

enum
{
    WM_CREATE       = 0x0001,
    WM_DESTROY      = 0x0002,
    WM_ACTIVATE     = 0x0006,
    WM_SETFOCUS     = 0x0007,
    WM_KILLFOCUS    = 0x0008,
    WM_CLOSE        = 0x0010,
    WM_QUIT         = 0x0012,
    WM_ERASEBKGND   = 0x0014,
    WM_ACTIVATEAPP  = 0x001C,
    WM_NCCREATE     = 0x0081,
    WM_NCDESTROY    = 0x0082,
    WM_NCACTIVATE   = 0x0086,
};

static const DWORD WS_VISIBLE = 0x10000000;
static const DWORD ERROR_INVALID_WINDOW_HANDLE = 1400;
static const DWORD ERROR_CLASS_ALREADY_EXISTS = 1410;
static const DWORD ERROR_CLASS_DOES_NOT_EXIST = 1411;
static const DWORD ERROR_INVALID_THREAD_ID = 1444;
static const DWORD ERROR_CANNOT_FIND_WND_CLASS = 1407;

// Window state shared by the calls: active window, mouse capture, cursor. The guest calls under the execution
// lock, so plain atomics suffice.
static std::atomic<HWND>    s_active{0};
static std::atomic<HWND>    s_capture{0};
static std::atomic<HCURSOR> s_cursor{0};
static std::atomic<bool>    s_activated{false};
static std::atomic<DWORD>   s_guiThread{0};     // thread that created the first window
static POINT                s_cursorPos = { 0, 0 };
static BOOL                 s_screenSaverActive = 0;

static Window* findWindow(WinApplication* app, HWND hWnd)
{
    return hWnd ? dynamic_cast<Window*>(app->getResource(hWnd)) : nullptr;
}

static DWORD currentThreadId(WinApplication* app, const x86::CPU& cpu)
{
    return app->getMemory<x86::reg32>(cpu.efs + teb::ThreadId);
}

// Runs the window procedure on the calling thread with the arguments on the guest stack (stdcall), as SendMessage
// does for a window of the calling thread. 0 for a window that does not exist.
static LRESULT callWindowProc(WinApplication* app, x86::CPU& cpu, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    const Window* window = findWindow(app, hWnd);
    if (!window || !window->wndProc())
        return 0;
    const x86::reg32 handler = window->wndProc();
    cpu.esp -= 20;
    app->getMemory<x86::reg32>(cpu.esp + 16) = lParam;
    app->getMemory<x86::reg32>(cpu.esp + 12) = wParam;
    app->getMemory<x86::reg32>(cpu.esp + 8) = Msg;
    app->getMemory<x86::reg32>(cpu.esp + 4) = hWnd;
    app->getMemory<x86::reg32>(cpu.esp + 0) = 0;
    cpu.ip = handler;
    app->dynamic_call(handler, cpu);
    return x86::sreg32(cpu.eax);
}

// The application becomes active once, when its first window is shown, and stays active: the Vita has no other
// application to switch to (suspend and resume happen outside the guest). The messages go to the window procedure
// at once, so the game's pump sees the active flag (docs/re/boot.md section 4) before its first frame.
static void activate(WinApplication* app, x86::CPU& cpu, HWND hWnd)
{
    if (s_activated.exchange(true))
        return;
    s_active.store(hWnd);
    Window::setFocus(hWnd);
    callWindowProc(app, cpu, hWnd, WM_ACTIVATEAPP, 1, currentThreadId(app, cpu));
    callWindowProc(app, cpu, hWnd, WM_NCACTIVATE, 1, 0);
    callWindowProc(app, cpu, hWnd, WM_ACTIVATE, 1, 0);      // WA_ACTIVE
    callWindowProc(app, cpu, hWnd, WM_SETFOCUS, 0, 0);
}

BOOL AdjustWindowRectEx(WinApplication* app, x86::CPU& cpu,
                        LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle)
{
    if (!lpRect)
    {
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
    const RECT insets = Window::frameInsets(dwStyle, dwExStyle, bMenu != 0);
    lpRect->left -= insets.left;
    lpRect->top -= insets.top;
    lpRect->right += insets.right;
    lpRect->bottom += insets.bottom;
    return 1;
}

BOOL AdjustWindowRect(WinApplication* app, x86::CPU& cpu,
                      LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
{
    return AdjustWindowRectEx(app, cpu, lpRect, dwStyle, bMenu, 0);
}

// Painting has no target: the whole window is valid at once.
HDC BeginPaint(WinApplication* app, x86::CPU& cpu,
               HWND hWnd, LPPAINTSTRUCT lpPaint)
{
    const Window* window = findWindow(app, hWnd);
    if (!window || !lpPaint)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    memset(lpPaint, 0, sizeof(*lpPaint));
    lpPaint->hdc = WindowDC;
    lpPaint->rcPaint = RECT{ 0, 0, window->clientWidth(), window->clientHeight() };
    return WindowDC;
}

BOOL EndPaint(WinApplication* app, x86::CPU& cpu,
              HWND hWnd, const PAINTSTRUCT *lpPaint)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(lpPaint);
    return 1;
}

// The hooks are the only ones in their chains.
LRESULT CallNextHookEx(WinApplication* app, x86::CPU& cpu,
                       HHOOK  hhk, int nCode, WPARAM wParam, LPARAM lParam)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hhk);
    RC_USE(nCode);
    RC_USE(wParam);
    RC_USE(lParam);
    return 0;
}

// Latin-1 upper case; a pointer below 0x10000 is a single character in its low word.
x86::reg32 CharUpperA(WinApplication* app, x86::CPU& cpu,
                      LPSTR lpsz)
{
    const x86::reg32 address = app->getMemory<x86::reg32>(cpu.esp + 4);
    if (address < 0x10000)
        return x86::reg32(toupper(int(address & 0xff)));
    for (char* c = lpsz; *c; ++c)
        *c = char(toupper(x86::reg8(*c)));
    return address;
}

DWORD CharUpperBuffA(WinApplication* app, x86::CPU& cpu,
                     LPSTR lpsz, DWORD cchLength)
{
    RC_USE(app);
    RC_USE(cpu);
    for (x86::reg32 i = 0; i < cchLength; ++i)
        lpsz[i] = char(toupper(x86::reg8(lpsz[i])));
    return cchLength;
}

// The client area starts inside the frame of the window's rectangle.
BOOL ClientToScreen(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, LPPOINT lpPoint)
{
    const Window* window = findWindow(app, hWnd);
    if (!window || !lpPoint)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    x86::reg32 style = 0, exStyle = 0;
    window->getLong(-16, &style);
    window->getLong(-20, &exStyle);
    const RECT insets = Window::frameInsets(style, exStyle, false);
    lpPoint->x += window->rect().left + insets.left;
    lpPoint->y += window->rect().top + insets.top;
    return 1;
}

// Top-level windows only. CW_USEDEFAULT places the window at 0,0 with the screen's size.
HWND CreateWindowExA(WinApplication* app, x86::CPU& cpu,
                     DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                     HINSTANCE hInstance, LPVOID lpParam)
{
    RC_USE(hInstance);
    RC_USE(lpParam);
    static const int CW_USEDEFAULT = int(0x80000000);
    const x86::reg32 classAddress = app->getMemory<x86::reg32>(cpu.esp + 8);
    const WindowClass* windowClass = Window::findClass(classAddress, classAddress >= 0x10000 ? lpClassName : nullptr);
    if (!windowClass)
    {
        pf::logError("CreateWindowExA: class %s is not registered (from 0x%08x)",
                     classAddress >= 0x10000 ? lpClassName : "(atom)", callerAddress(app, cpu));
        setLastError(app, cpu, ERROR_CANNOT_FIND_WND_CLASS);
        return 0;
    }
    if (hMenu || (hWndParent && hWndParent != DesktopWindow))
        unsupported(app, cpu, "CreateWindowExA", "child window or menu (parent 0x%x, menu 0x%x)", hWndParent, hMenu);
    if (X == CW_USEDEFAULT)
        X = Y = 0;
    if (nWidth == CW_USEDEFAULT)
    {
        nWidth = ScreenWidth;
        nHeight = ScreenHeight;
    }
    const RECT rect = { X, Y, X + nWidth, Y + nHeight };
    Window* window = new Window(*windowClass, lpWindowName, dwStyle, dwExStyle, rect);
    const HWND hWnd = app->allocateResource(window);
    DWORD expected = 0;
    s_guiThread.compare_exchange_strong(expected, currentThreadId(app, cpu));
    if (dwStyle & WS_VISIBLE)
        activate(app, cpu, hWnd);
    return hWnd;
}

// The game's window procedures pass every message they do not handle here.
LRESULT DefWindowProcA(WinApplication* app, x86::CPU& cpu,
                       HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    RC_USE(wParam);
    RC_USE(lParam);
    switch (Msg)
    {
    case WM_CLOSE:
        DestroyWindow(app, cpu, hWnd);
        return 0;
    case WM_NCCREATE:
    case WM_NCACTIVATE:
    case WM_ERASEBKGND:
        return 1;
    default:
        return 0;
    }
}

BOOL DestroyWindow(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd)
{
    if (!findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    callWindowProc(app, cpu, hWnd, WM_DESTROY, 0, 0);
    callWindowProc(app, cpu, hWnd, WM_NCDESTROY, 0, 0);
    HWND expected = hWnd;
    s_active.compare_exchange_strong(expected, 0);
    if (Window::focus() == hWnd)
        Window::setFocus(0);
    app->freeResource(hWnd);
    return 1;
}

// Thread messages (no window) are not dispatched.
LRESULT DispatchMessageA(WinApplication* app, x86::CPU& cpu,
                         const MSG *lpMsg)
{
    if (!lpMsg || !lpMsg->hWindow)
        return 0;
    return callWindowProc(app, cpu, lpMsg->hWindow, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

HWND FindWindowA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpClassName, LPCSTR lpWindowName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpClassName);
    RC_USE(lpWindowName);
    return 0;
}

// There is no keyboard: no key is down and no lock key is toggled on (the game's lock-key reset then has nothing to
// do, docs/re/input.md section 6).
SHORT GetAsyncKeyState(WinApplication* app, x86::CPU& cpu,
                       int vKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(vKey);
    return 0;
}

SHORT GetKeyState(WinApplication* app, x86::CPU& cpu,
                  int nVirtKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(nVirtKey);
    return 0;
}

BOOL GetClientRect(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, LPRECT lpRect)
{
    if (hWnd == DesktopWindow && lpRect)
    {
        *lpRect = RECT{ 0, 0, ScreenWidth, ScreenHeight };
        return 1;
    }
    const Window* window = findWindow(app, hWnd);
    if (!window || !lpRect)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    *lpRect = RECT{ 0, 0, window->clientWidth(), window->clientHeight() };
    return 1;
}

BOOL GetWindowRect(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, LPRECT lpRect)
{
    if (hWnd == DesktopWindow && lpRect)
    {
        *lpRect = RECT{ 0, 0, ScreenWidth, ScreenHeight };
        return 1;
    }
    const Window* window = findWindow(app, hWnd);
    if (!window || !lpRect)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    *lpRect = window->rect();
    return 1;
}

HWND GetDesktopWindow(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return DesktopWindow;
}

HWND GetForegroundWindow(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return s_active.load();
}

HWND GetActiveWindow(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return s_active.load();
}

// No window owns popups: the last active one is the window itself.
HWND GetLastActivePopup(WinApplication* app, x86::CPU& cpu,
                        HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    return hWnd;
}

int GetKeyboardType(WinApplication* app, x86::CPU& cpu,
                    int nTypeFlag)
{
    // An enhanced 101/102-key keyboard with 12 function keys.
    switch (nTypeFlag)
    {
    case 0: return 4;
    case 1: return 0;
    case 2: return 12;
    default:
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
}

BOOL GetMessageA(WinApplication* app, x86::CPU& cpu,
                 LPMSG lpMsg, HWND  hWnd, UINT  wMsgFilterMin, UINT  wMsgFilterMax)
{
    if (hWnd || wMsgFilterMin || wMsgFilterMax)
        unsupported(app, cpu, "GetMessageA", "filter window 0x%x, messages 0x%x-0x%x", hWnd, wMsgFilterMin,
                    wMsgFilterMax);
    app->unlockContext(cpu);
    const BOOL result = Window::getMessage(cpu, lpMsg);
    app->lockContext(cpu);
    return result;
}

// PM_REMOVE (1); PM_NOYIELD (2) changes nothing. Filters are not supported.
BOOL PeekMessageA(WinApplication* app, x86::CPU& cpu,
                  LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
    if (hWnd || wMsgFilterMin || wMsgFilterMax || (wRemoveMsg & ~3u))
        unsupported(app, cpu, "PeekMessageA", "filter window 0x%x, messages 0x%x-0x%x, flags 0x%x", hWnd,
                    wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (!lpMsg)
        return 0;
    return Window::peekMessage(lpMsg, (wRemoveMsg & 1) != 0) ? 1 : 0;
}

// The desktop is the Vita screen; frame metrics match AdjustWindowRect. No mouse is present.
int GetSystemMetrics(WinApplication* app, x86::CPU& cpu,
                     int nIndex)
{
    switch (nIndex)
    {
    case 0:  return ScreenWidth;        // SM_CXSCREEN
    case 1:  return ScreenHeight;       // SM_CYSCREEN
    case 4:  return 18;                 // SM_CYCAPTION
    case 5:  case 6: return 1;          // SM_CXBORDER, SM_CYBORDER
    case 7:  case 8: return 3;          // SM_CXDLGFRAME, SM_CYDLGFRAME
    case 15: return 19;                 // SM_CYMENU
    case 16: return ScreenWidth;        // SM_CXFULLSCREEN
    case 17: return ScreenHeight - 18;  // SM_CYFULLSCREEN
    case 19: return 0;                  // SM_MOUSEPRESENT
    case 32: case 33: return 4;         // SM_CXFRAME, SM_CYFRAME
    case 43: return 0;                  // SM_CMOUSEBUTTONS
    case 80: return 1;                  // SM_CMONITORS
    default:
        pf::logError("GetSystemMetrics %d: not provided, 0 (from 0x%08x)", nIndex, callerAddress(app, cpu));
        return 0;
    }
}

HDC GetWindowDC(WinApplication* app, x86::CPU& cpu,
                HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return WindowDC;
}

LONG GetWindowLongA(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, int nIndex)
{
    const Window* window = findWindow(app, hWnd);
    x86::reg32 value = 0;
    if (!window || !window->getLong(nIndex, &value))
    {
        setLastError(app, cpu, window ? 1413 : ERROR_INVALID_WINDOW_HANDLE);   // ERROR_INVALID_INDEX
        return 0;
    }
    return LONG(value);
}

LONG SetWindowLongA(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, int nIndex, LONG dwNewLong)
{
    Window* window = findWindow(app, hWnd);
    x86::reg32 previous = 0;
    if (!window || !window->setLong(nIndex, x86::reg32(dwNewLong), &previous))
    {
        setLastError(app, cpu, window ? 1413 : ERROR_INVALID_WINDOW_HANDLE);   // ERROR_INVALID_INDEX
        return 0;
    }
    setLastError(app, cpu, error::Success);
    return LONG(previous);
}

BOOL IsIconic(WinApplication* app, x86::CPU& cpu,
              HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return 0;
}

// System icons and cursors (no module) are pseudo handles; the executable has no resources.
HICON LoadIconA(WinApplication* app, x86::CPU& cpu,
                HINSTANCE hInstance, LPCSTR lpIconName)
{
    const x86::reg32 id = app->getMemory<x86::reg32>(cpu.esp + 8);
    if (hInstance || id >= 0x10000)
    {
        setLastError(app, cpu, 1813);   // ERROR_RESOURCE_TYPE_NOT_FOUND
        return 0;
    }
    RC_USE(lpIconName);
    return SystemIconBase + id;
}

HCURSOR LoadCursorA(WinApplication* app, x86::CPU& cpu,
                    HINSTANCE hInstance, LPCSTR lpCursorName)
{
    const x86::reg32 id = app->getMemory<x86::reg32>(cpu.esp + 8);
    if (hInstance || id >= 0x10000)
    {
        setLastError(app, cpu, 1813);   // ERROR_RESOURCE_TYPE_NOT_FOUND
        return 0;
    }
    RC_USE(lpCursorName);
    return SystemCursorBase + id;
}

// No dialog on the Vita: the message goes to the log and the box answers as the user who lets the program go on
// would (OK, Yes, Ignore for an assertion's Abort/Retry/Ignore, Cancel for Retry/Cancel).
int MessageBoxA(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    RC_USE(hWnd);
    pf::logError("MessageBox [%s] (type 0x%x, from 0x%08x): %s", lpCaption ? lpCaption : "", uType,
                 callerAddress(app, cpu), lpText ? lpText : "");
    enum { IDOK = 1, IDCANCEL = 2, IDIGNORE = 5, IDYES = 6 };
    switch (uType & 0xf)
    {
    case 2:  return IDIGNORE;   // MB_ABORTRETRYIGNORE
    case 3:                     // MB_YESNOCANCEL
    case 4:  return IDYES;      // MB_YESNO
    case 5:  return IDCANCEL;   // MB_RETRYCANCEL
    default: return IDOK;
    }
}

BOOL OffsetRect(WinApplication* app, x86::CPU& cpu,
                LPRECT lprc, int dx, int dy)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lprc)
        return 0;
    lprc->left += dx;
    lprc->right += dx;
    lprc->top += dy;
    lprc->bottom += dy;
    return 1;
}

BOOL SetRect(WinApplication* app, x86::CPU& cpu,
             LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lprc)
        return 0;
    *lprc = RECT{ xLeft, yTop, xRight, yBottom };
    return 1;
}

BOOL PostMessageA(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (hWnd && !findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return Window::postMessage(hWnd, Msg, wParam, lParam);
}

void PostQuitMessage(WinApplication* app, x86::CPU& cpu,
                     int nExitCode)
{
    RC_USE(app);
    RC_USE(cpu);
    Window::postMessage(0, WM_QUIT, WPARAM(nExitCode), 0);
}

// One message queue: the GUI thread's. A thread that has ended has none (ERROR_INVALID_THREAD_ID); the TAPI and
// network threads that could own one never run a message loop on the port.
BOOL PostThreadMessageA(WinApplication* app, x86::CPU& cpu,
                        DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (idThread && idThread == s_guiThread.load())
        return Window::postMessage(0, Msg, wParam, lParam);
    pf::logInfo("PostThreadMessageA: thread 0x%x has no message queue, message 0x%x dropped (from 0x%08x)",
                idThread, Msg, callerAddress(app, cpu));
    setLastError(app, cpu, ERROR_INVALID_THREAD_ID);
    return 0;
}

static ATOM registerClass(WinApplication* app, x86::CPU& cpu, const char* name, x86::reg32 nameAddress,
                          x86::reg32 style, x86::reg32 wndProc, LONG windowExtra)
{
    if (nameAddress < 0x10000)
        unsupported(app, cpu, "RegisterClass", "class named by atom 0x%x", nameAddress);
    const ATOM atom = Window::registerClass(WindowClass{ name, 0, style, wndProc, x86::reg32(windowExtra) });
    if (!atom)
        setLastError(app, cpu, ERROR_CLASS_ALREADY_EXISTS);
    return atom;
}

ATOM RegisterClassA(WinApplication* app, x86::CPU& cpu,
                    const WNDCLASSA *lpWndClass)
{
    if (!lpWndClass)
        return 0;
    const char* name = lpWndClass->lpczClassName >= 0x10000 ? &app->getMemory<char>(lpWndClass->lpczClassName) : "";
    return registerClass(app, cpu, name, lpWndClass->lpczClassName, lpWndClass->style, lpWndClass->wndProc,
                         lpWndClass->cbWndExtra);
}

struct WNDCLASSEXA
{
    UINT        cbSize;
    UINT        style;
    x86::reg32  lpfnWndProc;
    LONG        cbClsExtra;
    LONG        cbWndExtra;
    HINSTANCE   hInstance;
    HICON       hIcon;
    HCURSOR     hCursor;
    HBRUSH      hbrBackground;
    x86::reg32  lpszMenuName;
    x86::reg32  lpszClassName;
    HICON       hIconSm;
};

ATOM RegisterClassExA(WinApplication* app, x86::CPU& cpu,
                      const WNDCLASSEXA* lpwcx)
{
    if (!lpwcx || lpwcx->cbSize != sizeof(WNDCLASSEXA))
    {
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
    const char* name = lpwcx->lpszClassName >= 0x10000 ? &app->getMemory<char>(lpwcx->lpszClassName) : "";
    return registerClass(app, cpu, name, lpwcx->lpszClassName, lpwcx->style, lpwcx->lpfnWndProc, lpwcx->cbWndExtra);
}

BOOL UnregisterClassA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpClassName, HINSTANCE hInstance)
{
    RC_USE(hInstance);
    const x86::reg32 nameAddress = app->getMemory<x86::reg32>(cpu.esp + 4);
    if (!Window::unregisterClass(nameAddress, nameAddress >= 0x10000 ? lpClassName : nullptr))
    {
        setLastError(app, cpu, ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    return 1;
}

// A window of the port is always one of the calling (GUI) thread's: the message is sent at once.
BOOL SendNotifyMessageA(WinApplication* app, x86::CPU& cpu,
                        HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    static const HWND HWND_BROADCAST = 0xffff;
    if (hWnd == HWND_BROADCAST)
        return 1;   // no other top-level windows to notify
    if (!findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    callWindowProc(app, cpu, hWnd, Msg, wParam, lParam);
    return 1;
}

HWND SetActiveWindow(WinApplication* app, x86::CPU& cpu,
                     HWND hWnd)
{
    if (!findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    activate(app, cpu, hWnd);
    return s_active.exchange(hWnd);
}

HWND SetFocus(WinApplication* app, x86::CPU& cpu,
              HWND hWnd)
{
    if (hWnd && !findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return Window::setFocus(hWnd);
}

HWND SetCapture(WinApplication* app, x86::CPU& cpu,
                HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    return s_capture.exchange(hWnd);
}

BOOL ReleaseCapture(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    s_capture.store(0);
    return 1;
}

// The cursor is never drawn.
HCURSOR SetCursor(WinApplication* app, x86::CPU& cpu,
                  HCURSOR hCursor)
{
    RC_USE(app);
    RC_USE(cpu);
    return s_cursor.exchange(hCursor);
}

// The position is kept for the cursor that is never drawn, clamped to the screen.
BOOL SetCursorPos(WinApplication* app, x86::CPU& cpu,
                  int X, int Y)
{
    RC_USE(app);
    RC_USE(cpu);
    s_cursorPos.x = std::max(0, std::min(X, ScreenWidth - 1));
    s_cursorPos.y = std::max(0, std::min(Y, ScreenHeight - 1));
    return 1;
}

BOOL SetForegroundWindow(WinApplication* app, x86::CPU& cpu,
                         HWND hWnd)
{
    if (!findWindow(app, hWnd))
        return 0;
    activate(app, cpu, hWnd);
    s_active.store(hWnd);
    return 1;
}

// SWP_NOSIZE (1), SWP_NOMOVE (2); z-order and the other flags change nothing without other windows.
BOOL SetWindowPos(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
    RC_USE(hWndInsertAfter);
    Window* window = findWindow(app, hWnd);
    if (!window)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    RECT rect = window->rect();
    if (!(uFlags & 2))
    {
        rect.right += X - rect.left;
        rect.bottom += Y - rect.top;
        rect.left = X;
        rect.top = Y;
    }
    if (!(uFlags & 1))
    {
        rect.right = rect.left + cx;
        rect.bottom = rect.top + cy;
    }
    window->setRect(rect);
    if (uFlags & 0x40)          // SWP_SHOWWINDOW
    {
        window->setVisible(true);
        activate(app, cpu, hWnd);
    }
    else if (uFlags & 0x80)     // SWP_HIDEWINDOW
    {
        window->setVisible(false);
    }
    return 1;
}

// WH_KEYBOARD (2) and WH_KEYBOARD_LL (13), the game's task-switch filter. The port produces none of the keys the
// filter swallows (Alt+Tab, Win, ...), and passing the others on is what the filter does, so the hook procedure is
// kept but not called.
class Hook : public GenericResource
{
public:
    Hook(int id, x86::reg32 procedure) : m_id(id), m_procedure(procedure) {}
    int id() const { return m_id; }
    x86::reg32 procedure() const { return m_procedure; }

private:
    int        m_id;
    x86::reg32 m_procedure;
};

HHOOK SetWindowsHookExA(WinApplication* app, x86::CPU& cpu,
                        int idHook, x86::reg32 lpfn, HINSTANCE hmod, DWORD dwThreadId)
{
    RC_USE(hmod);
    RC_USE(dwThreadId);
    if (idHook != 2 && idHook != 13)
        unsupported(app, cpu, "SetWindowsHookExA", "hook type %d", idHook);
    if (!lpfn)
    {
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
    return app->allocateResource(new Hook(idHook, lpfn));
}

BOOL UnhookWindowsHookEx(WinApplication* app, x86::CPU& cpu,
                         HHOOK hhk)
{
    if (!dynamic_cast<Hook*>(app->getResource(hhk)) || !app->freeResource(hhk))
    {
        setLastError(app, cpu, 1404);   // ERROR_INVALID_HOOK_HANDLE
        return 0;
    }
    return 1;
}

// The display counter of the cursor, which is never drawn; it starts at 0 as with a mouse installed.
int ShowCursor(WinApplication* app, x86::CPU& cpu,
               BOOL bShow)
{
    RC_USE(app);
    RC_USE(cpu);
    static std::atomic<int> s_displayCount{0};
    return bShow ? ++s_displayCount : --s_displayCount;
}

// Returns whether the window was visible. Showing it activates the application (once, see activate).
BOOL ShowWindow(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, int nCmdShow)
{
    Window* window = findWindow(app, hWnd);
    if (!window)
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    const BOOL wasVisible = window->visible() ? 1 : 0;
    window->setVisible(nCmdShow != 0);  // SW_HIDE
    if (nCmdShow != 0)
        activate(app, cpu, hWnd);
    return wasVisible;
}

BOOL SwapMouseButton(WinApplication* app, x86::CPU& cpu, BOOL fSwap)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(fSwap);
    return 0;
}

// The screen saver flag (SPI_GETSCREENSAVEACTIVE / SPI_SETSCREENSAVEACTIVE): the game clears it while it runs and
// restores it on exit. The Vita's own screen dimming is handled outside the guest.
BOOL SystemParametersInfoA(WinApplication* app, x86::CPU& cpu,
                           UINT uiAction, UINT uiParam, PVOID pvParam, UINT  fWinIni)
{
    RC_USE(fWinIni);
    switch (uiAction)
    {
    case 0x10:  // SPI_GETSCREENSAVEACTIVE
    case 0x54:  // SPI_GETPOWEROFFACTIVE
        if (!pvParam)
            return 0;
        *static_cast<x86::reg32*>(pvParam) = uiAction == 0x10 ? s_screenSaverActive : 0;
        return 1;
    case 0x11:  // SPI_SETSCREENSAVEACTIVE
        s_screenSaverActive = uiParam ? 1 : 0;
        return 1;
    default:
        unsupported(app, cpu, "SystemParametersInfoA", "action 0x%x", uiAction);
    }
}

// Key messages carry no characters on the port: text arrives as WM_CHAR directly (docs/re/input.md section 6).
BOOL TranslateMessage(WinApplication* app, x86::CPU& cpu,
                      const MSG *lpMsg)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpMsg);
    return 0;
}

// Synthesised key presses: the game only uses them to switch a lock key off, which is never on.
void keybd_event(WinApplication* app, x86::CPU& cpu,
                 BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo)
{
    RC_USE(bScan);
    RC_USE(dwExtraInfo);
    pf::logDebug("keybd_event vk 0x%x flags 0x%x from 0x%08x: no keyboard", bVk, dwFlags, callerAddress(app, cpu));
}

// ---- wsprintf ----------------------------------------------------------------------------------------------------

// The wsprintf formats: %[-][#][0][width][.precision][l|h]{c,C,d,i,u,x,X,s,S}, %%; at most 1024 characters
// including the terminator. Arguments are dwords read from the guest at arguments.
static int formatGuest(WinApplication* app, char* output, const char* format, x86::reg32 arguments)
{
    static const size_t Limit = 1024;
    std::string out;
    auto next = [&]() { const x86::reg32 v = app->getMemory<x86::reg32>(arguments); arguments += 4; return v; };
    for (const char* p = format; *p && out.size() < Limit - 1; ++p)
    {
        if (*p != '%')
        {
            out += *p;
            continue;
        }
        ++p;
        if (*p == '%')
        {
            out += '%';
            continue;
        }
        bool left = false, alternate = false, zero = false;
        for (;; ++p)
        {
            if (*p == '-') left = true;
            else if (*p == '#') alternate = true;
            else if (*p == '0') zero = true;
            else break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        int precision = -1;
        if (*p == '.')
        {
            precision = 0;
            ++p;
            while (*p >= '0' && *p <= '9')
                precision = precision * 10 + (*p++ - '0');
        }
        bool wide = false;
        if (*p == 'l' || *p == 'h' || *p == 'w')
        {
            wide = *p == 'l' || *p == 'w';
            ++p;
        }
        std::string field;
        switch (*p)
        {
        case 'c':
        case 'C':
            field = char(next() & 0xff);
            break;
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        {
            char number[32];
            const x86::reg32 value = next();
            const char* spec = *p == 'd' || *p == 'i' ? "%ld" : *p == 'u' ? "%lu" : *p == 'x' ? "%lx" : "%lX";
            if (*p == 'd' || *p == 'i')
                snprintf(number, sizeof(number), spec, long(x86::sreg32(value)));
            else
                snprintf(number, sizeof(number), spec, static_cast<unsigned long>(value));
            field = number;
            if (precision >= 0 && int(field.size()) < precision)
                field.insert(field[0] == '-' ? 1 : 0, size_t(precision) - field.size(), '0');
            if (alternate && (*p == 'x' || *p == 'X') && value)
                field.insert(0, *p == 'x' ? "0x" : "0X");
            break;
        }
        case 's':
        case 'S':
        {
            const x86::reg32 address = next();
            if (!address)
            {
                field = "(null)";
            }
            else if (wide || *p == 'S')
            {
                for (x86::reg32 a = address; app->getMemory<WCHAR>(a); a += 2)
                    field += char(app->getMemory<WCHAR>(a) & 0xff);
            }
            else
            {
                field = &app->getMemory<char>(address);
            }
            if (precision >= 0 && int(field.size()) > precision)
                field.resize(size_t(precision));
            break;
        }
        case 0:
            --p;
            continue;
        default:
            field = *p;     // unknown types print themselves, as wsprintf does
            break;
        }
        if (int(field.size()) < width)
        {
            const size_t pad = size_t(width) - field.size();
            if (left)
                field.append(pad, ' ');
            else if (zero && *p != 's' && *p != 'S' && *p != 'c' && *p != 'C')
                field.insert(field[0] == '-' ? 1 : 0, pad, '0');
            else
                field.insert(0, pad, ' ');
        }
        out += field;
    }
    if (out.size() > Limit - 1)
        out.resize(Limit - 1);
    memcpy(output, out.c_str(), out.size() + 1);
    return int(out.size());
}

// cdecl: output and format, then the arguments; the caller pops them.
int wsprintfA(WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 output = app->getMemory<x86::reg32>(cpu.esp + 4);
    const x86::reg32 format = app->getMemory<x86::reg32>(cpu.esp + 8);
    if (!output || !format)
        return 0;
    return formatGuest(app, &app->getMemory<char>(output), &app->getMemory<char>(format), cpu.esp + 12);
}

int wvsprintfA(WinApplication* app, x86::CPU& cpu,
               LPSTR lpOutput, LPCSTR lpFmt, x86::reg32 arglist)
{
    RC_USE(cpu);
    if (!lpOutput || !lpFmt)
        return 0;
    return formatGuest(app, lpOutput, lpFmt, arglist);
}

// ---- Calls of other games' code kept for the shared runtime ----------------------------------------------------

LRESULT SendMessageA(WinApplication* app, x86::CPU& cpu,
                     HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    return callWindowProc(app, cpu, hWnd, Msg, wParam, lParam);
}

LRESULT DefMDIChildProcA(WinApplication* app, x86::CPU& cpu,
                         HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcA(app, cpu, hWnd, uMsg, wParam, lParam);
}

// The extra window words (SetWindowWord/GetWindowWord) by window and offset.
static std::map<std::pair<HWND, int>, WORD> s_windowWords;

WORD SetWindowWord(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int nIndex, WORD wNewWord)
{
    RC_USE(app);
    RC_USE(cpu);
    WORD& word = s_windowWords[std::make_pair(hWnd, nIndex)];
    const WORD previous = word;
    word = wNewWord;
    return previous;
}

WORD GetWindowWord(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int nIndex)
{
    RC_USE(app);
    RC_USE(cpu);
    const auto it = s_windowWords.find(std::make_pair(hWnd, nIndex));
    return it == s_windowWords.end() ? 0 : it->second;
}

// UpdateWindow would send WM_PAINT for an invalid region; a window of the port never has one.
BOOL UpdateWindow(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd)
{
    if (!findWindow(app, hWnd))
    {
        setLastError(app, cpu, ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return 1;
}

HDC GetDC(WinApplication* app, x86::CPU& cpu,
          HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return WindowDC;
}

int ReleaseDC(WinApplication* app, x86::CPU& cpu,
              HWND hWnd, HDC hDC)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(hDC);
    return 1;
}

int SetScrollPos(WinApplication* app, x86::CPU& cpu,
                 HWND hWnd, int nBar, int nPos, BOOL bRedraw)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(nBar);
    RC_USE(nPos);
    RC_USE(bRedraw);
    return 0;
}

BOOL ShowScrollBar(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int wBar, BOOL bShow)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(wBar);
    RC_USE(bShow);
    return 1;
}

int SetScrollInfo(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, int nBar, x86::reg32 lpsi, BOOL redraw)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(nBar);
    RC_USE(lpsi);
    RC_USE(redraw);
    return 0;
}

// Translates a virtual key to the character a US keyboard types without modifiers: 1 character, or 0.
int ToAscii(WinApplication* app, x86::CPU& cpu,
            UINT uVirtKey, UINT uScanCode, const BYTE* lpKeyState, LPWORD lpChar, UINT uFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uScanCode);
    RC_USE(uFlags);
    const bool shift = lpKeyState && ((lpKeyState[0x10] & 0x80) != 0);
    int c = 0;
    if (uVirtKey >= 'A' && uVirtKey <= 'Z')
        c = shift ? int(uVirtKey) : int(uVirtKey) + 32;
    else if ((uVirtKey >= '0' && uVirtKey <= '9') || uVirtKey == 0x20 || uVirtKey == 0x0d || uVirtKey == 0x08
             || uVirtKey == 0x1b || uVirtKey == 0x09)
        c = int(uVirtKey);
    if (!c || !lpChar)
        return 0;
    *lpChar = WORD(c);
    return 1;
}

}}
