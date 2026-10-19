#include <winapi/user32.h>
#include <x86.h>
#include <lib/window.h>
#include <platform/log.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>

namespace win32 { namespace user32
{

// Pseudo handles: the desktop window and the device context of any window (GetDeviceCaps describes the display).
static const HWND DesktopWindow = 0xffff0001;
static const HDC WindowDC = 0xffff0002;


BOOL AdjustWindowRect(WinApplication* app, x86::CPU& cpu,
                      LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpRect);
    RC_USE(dwStyle);
    RC_USE(bMenu);
    return 1;
}

// Painting has no target: the whole window is valid at once.
HDC BeginPaint(WinApplication* app, x86::CPU& cpu,
               HWND hWnd, LPPAINTSTRUCT lpPaint)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    if (lpPaint)
        memset(lpPaint, 0, sizeof(*lpPaint));
    return WindowDC;
}
LRESULT CallNextHookEx(WinApplication* app, x86::CPU& cpu,
                       HHOOK  hhk, int nCode, WPARAM wParam, LPARAM lParam)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hhk);
    RC_USE(nCode);
    RC_USE(wParam);
    RC_USE(lParam);
    RC_ASSERT(false);
    return 0;
}

x86::reg32 CharUpperA(WinApplication* app, x86::CPU& cpu,
                      LPSTR lpsz)
{
    RC_USE(app);
    RC_USE(cpu);
    for (char* c = lpsz; *c; ++c)
        *c = char(toupper(*c));
    return app->getMemory<x86::reg32>(cpu.esp + 4);
}

DWORD CharUpperBuffA(WinApplication* app, x86::CPU& cpu,
                     LPSTR lpsz, DWORD cchLength)
{
    RC_USE(app);
    RC_USE(cpu);
    for (x86::reg32 i = 0; i < cchLength; ++i)
    {
        lpsz[i] = char(toupper(lpsz[i]));
    }
    return cchLength;
}

BOOL ClientToScreen(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, LPPOINT lpPoint)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(lpPoint);
    return 1;
}

HWND CreateWindowExA(WinApplication* app, x86::CPU& cpu,
                     DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                     HINSTANCE hInstance, LPVOID lpParam)
{
    RC_USE(cpu);
    RC_USE(dwExStyle);
    RC_USE(lpClassName);
    RC_USE(dwStyle);
    RC_USE(hWndParent);
    RC_USE(hMenu);
    RC_USE(hInstance);
    RC_USE(lpParam);
    win32::Window* window = new win32::Window(lpWindowName, X, Y, nWidth, nHeight);
    return app->allocateResource(window);
}

HDDEDATA DdeClientTransaction(WinApplication* app, x86::CPU& cpu,
                              LPBYTE pData, DWORD cbData, HCONV hConv, HSZ hszItem,
                              UINT wFmt, UINT wType, DWORD dwTimeout, LPDWORD pdwResult)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pData);
    RC_USE(cbData);
    RC_USE(hConv);
    RC_USE(hszItem);
    RC_USE(wFmt);
    RC_USE(wType);
    RC_USE(dwTimeout);
    RC_USE(pdwResult);
    RC_ASSERT(false);
    return -1;
}

HCONV DdeConnect(WinApplication* app, x86::CPU& cpu, DWORD idInst,
                 HSZ hszService, HSZ hszTopic, PCONVCONTEXT pCC)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idInst);
    RC_USE(hszService);
    RC_USE(hszTopic);
    RC_USE(pCC);
    RC_ASSERT(false);
    return -1;
}

HSZ DdeCreateStringHandleA(WinApplication* app, x86::CPU& cpu,
                           DWORD idInst, LPCSTR psz, int iCodePage)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idInst);
    RC_USE(psz);
    RC_USE(iCodePage);
    RC_ASSERT(false);
    return -1;
}

UINT DdeGetLastError(WinApplication* app, x86::CPU& cpu, DWORD idInst)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idInst);
    RC_ASSERT(false);
    return -1;
}

UINT DdeInitializeA(WinApplication* app, x86::CPU& cpu, LPDWORD pidInst,
                    PFNCALLBACK pfnCallback, DWORD afCmd, DWORD ulRes)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pidInst);
    RC_USE(pfnCallback);
    RC_USE(afCmd);
    RC_USE(ulRes);
    RC_ASSERT(false);
    return -1;
}

DWORD DdeQueryStringA(WinApplication* app, x86::CPU& cpu, DWORD idInst,
                      HSZ hsz, LPSTR psz, DWORD cchMax, int iCodePage)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idInst);
    RC_USE(hsz);
    RC_USE(psz);
    RC_USE(cchMax);
    RC_USE(iCodePage);
    RC_ASSERT(false);
    return -1;
}

BOOL DdeUninitialize(WinApplication* app, x86::CPU& cpu, DWORD idInst)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idInst);
    RC_ASSERT(false);
    return 0;
}

LRESULT DefWindowProcA(WinApplication* app, x86::CPU& cpu,
                       HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    RC_USE(cpu);
    RC_USE(wParam);
    RC_USE(lParam);
    if (Msg == 0x0002)
        app->freeResource(hWnd);
    return 0;
}

BOOL DestroyWindow(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    Window::postMessage(hWnd, 0x0002, 0, 0);
    return 0;
}

LRESULT DispatchMessageA(WinApplication* app, x86::CPU& cpu,
                         const MSG *lpMsg)
{
    x86::reg32 handler = win32::Window::getMessageHandler();
    cpu.esp -= 20;
    app->getMemory<x86::reg32>(cpu.esp + 16) = lpMsg->lParam;
    app->getMemory<x86::reg32>(cpu.esp + 12) = lpMsg->wParam;
    app->getMemory<x86::reg32>(cpu.esp + 8) = lpMsg->message;
    app->getMemory<x86::reg32>(cpu.esp + 4) = lpMsg->hWindow;
    app->getMemory<x86::reg32>(cpu.esp + 0) = handler;
    app->dynamic_call(handler, cpu);
    return x86::sreg32(cpu.eax);
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
HWND FindWindowA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpClassName, LPCSTR lpWindowName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpClassName);
    RC_USE(lpWindowName);
    return 0;
}

SHORT GetAsyncKeyState(WinApplication* app, x86::CPU& cpu,
                       int vKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(vKey);
    return 0;
}

BOOL GetClientRect(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, LPRECT lpRect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    lpRect->top = 0;
    lpRect->left = 0;
    lpRect->right = 800;
    lpRect->bottom = 600;
    return 1;
}

HWND GetDesktopWindow(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return DesktopWindow;
}

SHORT GetKeyState(WinApplication* app, x86::CPU& cpu,
                  int nVirtKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(nVirtKey);
    return 0;
}

int GetKeyboardType(WinApplication* app, x86::CPU& cpu,
                    int nTypeFlag)
{
    RC_USE(app);
    RC_USE(cpu);
    switch(nTypeFlag)
    {
    case 0:
    case 1:
    case 2:
        return 0;
    default:
        RC_ASSERT(false);
        return 0;
    }
}

BOOL GetMessageA(WinApplication* app, x86::CPU& cpu,
                 LPMSG lpMsg, HWND  hWnd, UINT  wMsgFilterMin, UINT  wMsgFilterMax)
{
    win32::Window* window = hWnd ? dynamic_cast<win32::Window*>(app->getResource(hWnd)) : nullptr;
    app->unlockContext(cpu);
    BOOL result = win32::Window::getMessage(cpu, lpMsg, window, wMsgFilterMin, wMsgFilterMax);
    app->lockContext(cpu);
    return result;
}

// The desktop is a 640x480 display; other metrics are 0.
int GetSystemMetrics(WinApplication* app, x86::CPU& cpu,
                     int nIndex)
{
    RC_USE(app);
    RC_USE(cpu);
    switch(nIndex)
    {
    case 0: //SM_CXSCREEN
        return 640;
    case 1: //SM_CYSCREEN
        return 480;
    default:
        pf::logInfo("GetSystemMetrics %d: 0", nIndex);
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
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(nIndex);
    return 0;
}

BOOL IsIconic(WinApplication* app, x86::CPU& cpu,
              HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return 0;
}

HICON LoadIconA(WinApplication* app, x86::CPU& cpu,
                HINSTANCE hInstance, LPCSTR lpIconName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hInstance);
    RC_USE(lpIconName);
    return 0;
}

// No dialog on the Vita: the message goes to the log and the box answers as the user who lets the program go on
// would (OK, Yes, Ignore for an assertion's Abort/Retry/Ignore, Cancel for Retry/Cancel).
int MessageBoxA(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    pf::logError("MessageBox [%s] (type 0x%x): %s", lpCaption ? lpCaption : "", uType, lpText ? lpText : "");
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
    lprc->left += dx;
    lprc->right += dx;
    lprc->top += dy;
    lprc->bottom += dy;
    return 1;
}

BOOL PostMessageA(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    RC_USE(app);
    RC_USE(cpu);
    return win32::Window::postMessage(hWnd, Msg, wParam, lParam);
}

void PostQuitMessage(WinApplication* app, x86::CPU& cpu,
                     int nExitCode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(nExitCode);
    win32::Window::postMessage(0, 0x0012, 0, 0);
}

BOOL PostThreadMessageA(WinApplication* app, x86::CPU& cpu,
                        DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idThread);
    RC_USE(Msg);
    RC_USE(wParam);
    RC_USE(lParam);
    RC_ASSERT(false);
    return 0;
}

ATOM RegisterClassA(WinApplication* app, x86::CPU& cpu,
                    const WNDCLASSA *lpWndClass)
{
    RC_USE(cpu);
    win32::WindowClass* wClass = new win32::WindowClass(lpWndClass->wndProc);
    return app->allocateResource(wClass);
}

BOOL SendNotifyMessageA(WinApplication* app, x86::CPU& cpu,
                        HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(Msg);
    RC_USE(wParam);
    RC_USE(lParam);
    RC_ASSERT(false);
    return 0;
}

HWND SetActiveWindow(WinApplication* app, x86::CPU& cpu,
                     HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return 0;
}

HCURSOR SetCursor(WinApplication* app, x86::CPU& cpu,
                  HCURSOR hCursor)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hCursor);
    return 0;
}

BOOL SetCursorPos(WinApplication* app, x86::CPU& cpu,
                  int X, int Y)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(X);
    RC_USE(Y);
    RC_ASSERT(false);
    return 0;
}

BOOL SetForegroundWindow(WinApplication* app, x86::CPU& cpu,
                         HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    return 1;
}

BOOL SetWindowPos(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
    RC_USE(hWndInsertAfter);
    RC_USE(X);
    RC_USE(Y);
    RC_USE(cx);
    RC_USE(cy);
    RC_USE(uFlags);
    // The window always covers the display.
    return 1;
}

HHOOK SetWindowsHookExA(WinApplication* app, x86::CPU& cpu,
                        int idHook, x86::reg32 lpfn, HINSTANCE hmod, DWORD dwThreadId)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpfn);
    RC_USE(hmod);
    RC_USE(dwThreadId);
    RC_ASSERT(idHook == 2);
    return 0;
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

// The first time the window is shown the application is activated (WM_ACTIVATEAPP), as Windows does; the message
// arrives through the queue.
BOOL ShowWindow(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, int nCmdShow)
{
    RC_USE(app);
    RC_USE(cpu);
    static std::atomic<bool> s_shown{false};
    if (nCmdShow != 0 && !s_shown.exchange(true))
        Window::postMessage(hWnd, 0x001c, 1, 0);    // WM_ACTIVATEAPP
    return 0;
}
BOOL SwapMouseButton(WinApplication* app, x86::CPU& cpu, BOOL fSwap)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(fSwap);
    return 0;
}

BOOL SystemParametersInfoA(WinApplication* app, x86::CPU& cpu,
                           UINT uiAction, UINT uiParam, PVOID pvParam, UINT  fWinIni)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uiParam);
    RC_USE(fWinIni);
    switch(uiAction)
    {
    case 0x10: //SPI_GETSCREENSAVEACTIVE
    case 0x54: //SPI_GETPOWEROFFACTIVE
        *static_cast<x86::reg32*>(pvParam) = 0;
        break;
    default:
        RC_ASSERT(false);
    }
    return 1;
}

BOOL TranslateMessage(WinApplication* app, x86::CPU& cpu,
                      const MSG *lpMsg)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpMsg);
    return 0;
}

BOOL UnhookWindowsHookEx(WinApplication* app, x86::CPU& cpu,
                         HHOOK hhk)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hhk);
    RC_ASSERT(false);
    return 0;
}

void keybd_event(WinApplication* app, x86::CPU& cpu,
                 BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(bVk);
    RC_USE(bScan);
    RC_USE(dwFlags);
    RC_USE(dwExtraInfo);
}

void wsprintfA(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_ASSERT(false);
}

// Calls the window procedure of the window class directly on the calling thread, with the arguments on the guest
// stack (stdcall).
static LRESULT callWindowProc(WinApplication* app, x86::CPU& cpu, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    const x86::reg32 handler = win32::Window::getMessageHandler();
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

// A message sent from another thread runs the window procedure on the sending thread: guest code runs under one
// execution lock, so this matches the sender waiting for the window's thread.
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

HCURSOR LoadCursorA(WinApplication* app, x86::CPU& cpu,
                    HINSTANCE hInstance, LPCSTR lpCursorName)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hInstance);
    RC_USE(lpCursorName);
    return 0;
}

BOOL UpdateWindow(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hWnd);
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

// Scroll bars belong to the game's debug log window; there is nothing to show them on.
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
