#ifndef user32_H_
#define user32_H_

#include <x86.h>
#include <winapi/types.h>


namespace win32 { class WinApplication; }

namespace win32 { namespace user32
{

struct WNDCLASSA
{
    DWORD       style;
    x86::reg32  wndProc;
    LONG        cbClsExtra;
    LONG        cbWndExtra;
    HINSTANCE   hInstance;
    HICON       hIcon;
    HCURSOR     hCursor;
    HBRUSH      hbrBackground;
    x86::reg32  lpczMenuName;
    x86::reg32  lpczClassName;
};

BOOL AdjustWindowRect(WinApplication* app, x86::CPU& cpu,
                      LPRECT lpRect, DWORD  dwStyle, BOOL   bMenu);
HDC BeginPaint(WinApplication* app, x86::CPU& cpu,
               HWND hWnd, LPPAINTSTRUCT lpPaint);
LRESULT CallNextHookEx(WinApplication* app, x86::CPU& cpu,
                       HHOOK  hhk, int nCode, WPARAM wParam, LPARAM lParam);
x86::reg32 CharUpperA(WinApplication* app, x86::CPU& cpu,
                      LPSTR lpsz);
DWORD CharUpperBuffA(WinApplication* app, x86::CPU& cpu,
                     LPSTR lpsz, DWORD cchLength);
BOOL ClientToScreen(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, LPPOINT lpPoint);
HWND CreateWindowExA(WinApplication* app, x86::CPU& cpu,
                     DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                     HINSTANCE hInstance, LPVOID lpParam);
LRESULT DefWindowProcA(WinApplication* app, x86::CPU& cpu,
                       HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL DestroyWindow(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd);
LRESULT DispatchMessageA(WinApplication* app, x86::CPU& cpu,
                         const MSG *lpMsg);
BOOL EndPaint(WinApplication* app, x86::CPU& cpu,
              HWND hWnd, const PAINTSTRUCT *lpPaint);
HWND FindWindowA(WinApplication* app, x86::CPU& cpu,
                 LPCSTR lpClassName, LPCSTR lpWindowName);
SHORT GetAsyncKeyState(WinApplication* app, x86::CPU& cpu,
                       int vKey);
BOOL GetClientRect(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, LPRECT lpRect);
HWND GetDesktopWindow(WinApplication* app, x86::CPU& cpu);
SHORT GetKeyState(WinApplication* app, x86::CPU& cpu,
                  int nVirtKey);
int GetKeyboardType(WinApplication* app, x86::CPU& cpu,
                    int nTypeFlag);
BOOL GetMessageA(WinApplication* app, x86::CPU& cpu,
                 LPMSG lpMsg, HWND  hWnd, UINT  wMsgFilterMin, UINT  wMsgFilterMax);
int GetSystemMetrics(WinApplication* app, x86::CPU& cpu,
                     int nIndex);
HDC GetWindowDC(WinApplication* app, x86::CPU& cpu,
                HWND hWnd);
LONG GetWindowLongA(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, int nIndex);
BOOL IsIconic(WinApplication* app, x86::CPU& cpu,
              HWND hWnd);
HICON LoadIconA(WinApplication* app, x86::CPU& cpu,
                HINSTANCE hInstance, LPCSTR lpIconName);
int MessageBoxA(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
BOOL OffsetRect(WinApplication* app, x86::CPU& cpu,
                LPRECT lprc, int dx, int dy);
BOOL PostMessageA(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
void PostQuitMessage(WinApplication* app, x86::CPU& cpu,
                     int nExitCode);
BOOL PostThreadMessageA(WinApplication* app, x86::CPU& cpu,
                        DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam);
ATOM RegisterClassA(WinApplication* app, x86::CPU& cpu,
                    const WNDCLASSA *lpWndClass);
BOOL SendNotifyMessageA(WinApplication* app, x86::CPU& cpu,
                        HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
HWND SetActiveWindow(WinApplication* app, x86::CPU& cpu,
                     HWND hWnd);
HCURSOR SetCursor(WinApplication* app, x86::CPU& cpu,
                  HCURSOR hCursor);
BOOL SetCursorPos(WinApplication* app, x86::CPU& cpu,
                  int X, int Y);
BOOL SetForegroundWindow(WinApplication* app, x86::CPU& cpu,
                         HWND hWnd);
BOOL SetWindowPos(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
HHOOK SetWindowsHookExA(WinApplication* app, x86::CPU& cpu,
                        int idHook, x86::reg32 lpfn, HINSTANCE hmod, DWORD dwThreadId);
int ShowCursor(WinApplication* app, x86::CPU& cpu,
               BOOL bShow);
BOOL ShowWindow(WinApplication* app, x86::CPU& cpu,
                HWND hWnd, int nCmdShow);
BOOL SwapMouseButton(WinApplication* app, x86::CPU& cpu, BOOL fSwap);
BOOL SystemParametersInfoA(WinApplication* app, x86::CPU& cpu,
                           UINT uiAction, UINT uiParam, PVOID pvParam, UINT  fWinIni);
BOOL TranslateMessage(WinApplication* app, x86::CPU& cpu,
                      const MSG *lpMsg);
BOOL UnhookWindowsHookEx(WinApplication* app, x86::CPU& cpu,
                         HHOOK hhk);
void keybd_event(WinApplication* app, x86::CPU& cpu,
                 BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR dwExtraInfo);
// cdecl with variable arguments: reads them from the guest stack itself.
int wsprintfA(WinApplication* app, x86::CPU& cpu);
int wvsprintfA(WinApplication* app, x86::CPU& cpu,
               LPSTR lpOutput, LPCSTR lpFmt, x86::reg32 arglist);

struct WNDCLASSEXA;
LONG SetWindowLongA(WinApplication* app, x86::CPU& cpu,
                    HWND hWnd, int nIndex, LONG dwNewLong);
HWND GetForegroundWindow(WinApplication* app, x86::CPU& cpu);
BOOL AdjustWindowRectEx(WinApplication* app, x86::CPU& cpu,
                        LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
HWND SetFocus(WinApplication* app, x86::CPU& cpu,
              HWND hWnd);
HWND SetCapture(WinApplication* app, x86::CPU& cpu,
                HWND hWnd);
BOOL ReleaseCapture(WinApplication* app, x86::CPU& cpu);
BOOL PeekMessageA(WinApplication* app, x86::CPU& cpu,
                  LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL GetWindowRect(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, LPRECT lpRect);
ATOM RegisterClassExA(WinApplication* app, x86::CPU& cpu,
                      const WNDCLASSEXA* lpwcx);
BOOL SetRect(WinApplication* app, x86::CPU& cpu,
             LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom);
BOOL UnregisterClassA(WinApplication* app, x86::CPU& cpu,
                      LPCSTR lpClassName, HINSTANCE hInstance);
// Resolved at run time by the CRT's error box (LoadLibraryA("user32.dll")).
HWND GetActiveWindow(WinApplication* app, x86::CPU& cpu);
HWND GetLastActivePopup(WinApplication* app, x86::CPU& cpu,
                        HWND hWnd);
LRESULT SendMessageA(WinApplication* app, x86::CPU& cpu,
                     HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT DefMDIChildProcA(WinApplication* app, x86::CPU& cpu,
                         HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
WORD SetWindowWord(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int nIndex, WORD wNewWord);
WORD GetWindowWord(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int nIndex);
HCURSOR LoadCursorA(WinApplication* app, x86::CPU& cpu,
                    HINSTANCE hInstance, LPCSTR lpCursorName);
BOOL UpdateWindow(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd);
HDC GetDC(WinApplication* app, x86::CPU& cpu,
          HWND hWnd);
int ReleaseDC(WinApplication* app, x86::CPU& cpu,
              HWND hWnd, HDC hDC);
int SetScrollPos(WinApplication* app, x86::CPU& cpu,
                 HWND hWnd, int nBar, int nPos, BOOL bRedraw);
BOOL ShowScrollBar(WinApplication* app, x86::CPU& cpu,
                   HWND hWnd, int wBar, BOOL bShow);
int SetScrollInfo(WinApplication* app, x86::CPU& cpu,
                  HWND hWnd, int nBar, x86::reg32 lpsi, BOOL redraw);
int ToAscii(WinApplication* app, x86::CPU& cpu,
            UINT uVirtKey, UINT uScanCode, const BYTE* lpKeyState, LPWORD lpChar, UINT uFlags);

}}

#endif
