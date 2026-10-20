#ifndef LIB_WINDOW_H_
#define LIB_WINDOW_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <string>
#include <vector>

namespace win32
{

// A registered window class (RegisterClassA / RegisterClassExA). Classes live until UnregisterClassA.
struct WindowClass
{
    std::string name;
    ATOM        atom;
    x86::reg32  style;
    x86::reg32  wndProc;
    x86::reg32  windowExtra;    // cbWndExtra bytes
};

// A top-level window. There is no window manager: a window keeps the rectangle it was created with and the client
// area that leaves inside its frame; one window is active and has the focus. All windows share one message queue,
// fed by the input pump and PostMessage; guest code reads it on any thread (the game has one GUI thread).
class Window : public GenericResource
{
public:
    Window(const WindowClass& windowClass, const char* title, x86::reg32 style, x86::reg32 exStyle,
           const RECT& rect);
    ~Window();

    x86::reg32 wndProc() const { return m_wndProc; }
    const RECT& rect() const { return m_rect; }
    LONG clientWidth() const { return m_clientWidth; }
    LONG clientHeight() const { return m_clientHeight; }
    // Moves and sizes the window; the client area follows with the current style's frame.
    void setRect(const RECT& rect);
    // GetWindowLongA / SetWindowLongA: GWL_* indices and the cbWndExtra bytes (aligned dwords). False if the index
    // is invalid.
    bool getLong(int index, x86::reg32* value) const;
    bool setLong(int index, x86::reg32 value, x86::reg32* previous);
    bool visible() const { return (m_style & StyleVisible) != 0; }
    void setVisible(bool visible);

    // Frame sizes of a style, as AdjustWindowRectEx adds them.
    static RECT frameInsets(x86::reg32 style, x86::reg32 exStyle, bool menu);

    // Window classes.
    static ATOM registerClass(const WindowClass& windowClass);
    static bool unregisterClass(x86::reg32 nameOrAtom, const char* name);
    // By atom (below 0x10000) or name; nullptr if not registered.
    static const WindowClass* findClass(x86::reg32 nameOrAtom, const char* name);

    // Message queue. getMessage blocks (polling the pad meanwhile) and returns 0 for WM_QUIT; peekMessage returns
    // false on an empty queue and leaves the message queued unless remove is set. Messages are written as the
    // 28-byte Win32 MSG.
    static x86::reg32 getMessage(const x86::CPU& cpu, MSG* result);
    static bool peekMessage(MSG* result, bool remove);
    static x86::reg32 postMessage(x86::reg32 hWnd, x86::reg32 message, x86::reg32 wParam, x86::reg32 lParam);
    // The window key and character messages go to (SetFocus); setFocus returns the previous one.
    static x86::reg32 focus();
    static x86::reg32 setFocus(x86::reg32 hWnd);

    // Input pump entry points. A key message carries the set-1 scancode and the extended flag in lParam.
    static void pushKey(x86::reg32 virtualKey, x86::reg8 scancode, bool extended, bool down);
    // WM_CHAR without a key message, as for IME text.
    static void pushChar(x86::reg32 charCode);
    // True while a posted key or character message has not been returned by getMessage yet.
    static bool inputPending();
    static void postQuit();
    static void wake();

    static const x86::reg32 StyleVisible = 0x10000000;

private:
    void updateClient();

    x86::reg32              m_wndProc;
    x86::reg32              m_style;
    x86::reg32              m_exStyle;
    x86::reg32              m_userData;
    RECT                    m_rect;
    LONG                    m_clientWidth;
    LONG                    m_clientHeight;
    std::vector<x86::reg8>  m_extra;
};

}

#endif
