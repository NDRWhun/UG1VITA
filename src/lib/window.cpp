#include <lib/window.h>
#include <lib/gamepad.h>
#include <lib/timer.h>
#include <platform/log.h>
#include <platform/msgqueue.h>
#include <platform/sync.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <strings.h>

namespace win32
{

static const x86::reg32 WM_QUIT     = 0x0012;
static const x86::reg32 WM_KEYDOWN  = 0x0100;
static const x86::reg32 WM_KEYUP    = 0x0101;
static const x86::reg32 WM_CHAR     = 0x0102;
static const x86::reg32 WM_USER     = 0x0400;
static const x86::reg32 WM_USER_END = 0xC000;

// Window styles that add a frame.
static const x86::reg32 WS_BORDER       = 0x00800000;
static const x86::reg32 WS_DLGFRAME     = 0x00400000;
static const x86::reg32 WS_CAPTION      = 0x00C00000;
static const x86::reg32 WS_THICKFRAME   = 0x00040000;
static const x86::reg32 WS_EX_CLIENTEDGE = 0x00000200;
static const x86::reg32 WS_EX_DLGMODALFRAME = 0x00000001;

// Frame metrics of the classic Windows XP look (GetSystemMetrics reports the same).
static const LONG CaptionHeight = 18;
static const LONG MenuHeight = 19;
static const LONG ThinBorder = 1;
static const LONG DialogFrame = 3;
static const LONG SizingFrame = 4;
static const LONG ClientEdge = 2;

enum MessageType
{
    MSG_USER,       // WM_USER range, code = message
    MSG_USER1,      // other posted messages, code = message
    MSG_KEY,        // code = down, data1 = virtual key, data2 = lParam
    MSG_CHAR,       // data1 = character, data2 = lParam
    MSG_QUIT,
    MSG_WAKE        // no payload; waiters re-check termination
};

static const x86::reg32 s_inputPollMs = 16;
static const x86::reg32 s_idleWaitMs = 100;     // threads not polling still check their terminate flag

static pf::MessageQueue s_queue;
static std::atomic<x86::reg32> s_inputPosted{0};  // MSG_KEY / MSG_CHAR pushed
static std::atomic<x86::reg32> s_inputTaken{0};   // MSG_KEY / MSG_CHAR returned by getMessage
static std::atomic<bool>       s_polling{false};  // a thread in getMessage polls the pad

static pf::LwMutex s_classMutex("window_classes");
static std::vector<WindowClass> s_classes;
static ATOM s_nextAtom = 0xC000;    // registered class atoms start here on Windows
static std::atomic<x86::reg32> s_focus{0};  // window that receives key and character messages

// Holds the pad polling role while a thread waits in getMessage; one poller is enough.
class PollRole
{
public:
    PollRole() : m_owned(false) {}
    ~PollRole()
    {
        if (m_owned)
            s_polling.store(false);
    }
    bool acquire()
    {
        if (!m_owned)
            m_owned = !s_polling.exchange(true);
        return m_owned;
    }
private:
    bool m_owned;
};

static void pushInput(const pf::Message& message)
{
    s_inputPosted.fetch_add(1);
    s_queue.push(message);
}

Window::Window(const WindowClass& windowClass, const char* title, x86::reg32 style, x86::reg32 exStyle,
               const RECT& rect)
    :   m_wndProc(windowClass.wndProc)
    ,   m_style(style)
    ,   m_exStyle(exStyle)
    ,   m_userData(0)
    ,   m_rect(rect)
    ,   m_clientWidth(0)
    ,   m_clientHeight(0)
    ,   m_extra((windowClass.windowExtra + 3) & ~3u, 0)
{
    updateClient();
    pf::logInfo("window \"%s\" of class \"%s\": %ldx%ld at %ld,%ld, client %ldx%ld, style 0x%08x", title ? title : "",
                windowClass.name.c_str(), long(rect.right - rect.left), long(rect.bottom - rect.top), long(rect.left),
                long(rect.top), long(m_clientWidth), long(m_clientHeight), style);
}

Window::~Window()
{
}

RECT Window::frameInsets(x86::reg32 style, x86::reg32 exStyle, bool menu)
{
    LONG border = 0;
    if (style & WS_THICKFRAME)
        border = SizingFrame;
    else if ((style & WS_DLGFRAME) || (exStyle & WS_EX_DLGMODALFRAME))
        border = DialogFrame;
    else if (style & WS_BORDER)
        border = ThinBorder;
    if (exStyle & WS_EX_CLIENTEDGE)
        border += ClientEdge;
    RECT insets = { border, border, border, border };
    if ((style & WS_CAPTION) == WS_CAPTION)
        insets.top += CaptionHeight;
    if (menu)
        insets.top += MenuHeight;
    return insets;
}

void Window::updateClient()
{
    const RECT insets = frameInsets(m_style, m_exStyle, false);
    m_clientWidth = std::max<LONG>(0, m_rect.right - m_rect.left - insets.left - insets.right);
    m_clientHeight = std::max<LONG>(0, m_rect.bottom - m_rect.top - insets.top - insets.bottom);
}

void Window::setRect(const RECT& rect)
{
    m_rect = rect;
    updateClient();
}

void Window::setVisible(bool visible)
{
    m_style = visible ? (m_style | StyleVisible) : (m_style & ~StyleVisible);
}

// GWL_WNDPROC (-4), GWL_HINSTANCE (-6), GWL_ID (-12), GWL_STYLE (-16), GWL_EXSTYLE (-20), GWL_USERDATA (-21).
bool Window::getLong(int index, x86::reg32* value) const
{
    switch (index)
    {
    case -4:  *value = m_wndProc; return true;
    case -6:  *value = 0x400000; return true;
    case -12: *value = 0; return true;
    case -16: *value = m_style; return true;
    case -20: *value = m_exStyle; return true;
    case -21: *value = m_userData; return true;
    default:
        if (index < 0 || size_t(index) + 4 > m_extra.size())
            return false;
        memcpy(value, &m_extra[index], 4);
        return true;
    }
}

// A new style keeps the window rectangle; the client area follows the new frame. The instance and id of a
// top-level window cannot change.
bool Window::setLong(int index, x86::reg32 value, x86::reg32* previous)
{
    if (!getLong(index, previous) || index == -6 || index == -12)
        return false;
    switch (index)
    {
    case -4:  m_wndProc = value; break;
    case -16: m_style = value; updateClient(); break;
    case -20: m_exStyle = value; updateClient(); break;
    case -21: m_userData = value; break;
    default:  memcpy(&m_extra[index], &value, 4); break;
    }
    return true;
}

x86::reg32 Window::focus()
{
    return s_focus.load();
}

x86::reg32 Window::setFocus(x86::reg32 hWnd)
{
    return s_focus.exchange(hWnd);
}

ATOM Window::registerClass(const WindowClass& windowClass)
{
    pf::LwLockGuard guard(s_classMutex);
    for (const WindowClass& registered : s_classes)
    {
        if (strcasecmp(registered.name.c_str(), windowClass.name.c_str()) == 0)
            return 0;   // ERROR_CLASS_ALREADY_EXISTS
    }
    s_classes.push_back(windowClass);
    s_classes.back().atom = s_nextAtom++;
    return s_classes.back().atom;
}

bool Window::unregisterClass(x86::reg32 nameOrAtom, const char* name)
{
    pf::LwLockGuard guard(s_classMutex);
    for (auto it = s_classes.begin(); it != s_classes.end(); ++it)
    {
        if (nameOrAtom < 0x10000 ? it->atom == nameOrAtom : strcasecmp(it->name.c_str(), name) == 0)
        {
            s_classes.erase(it);
            return true;
        }
    }
    return false;
}

const WindowClass* Window::findClass(x86::reg32 nameOrAtom, const char* name)
{
    pf::LwLockGuard guard(s_classMutex);
    for (const WindowClass& registered : s_classes)
    {
        if (nameOrAtom < 0x10000 ? registered.atom == nameOrAtom : strcasecmp(registered.name.c_str(), name) == 0)
            return &registered;
    }
    return nullptr;
}

// Writes the 28 bytes of a Win32 MSG (the host MSG has an extra field the guest's does not).
static void storeMessage(MSG* result, x86::reg32 hWnd, x86::reg32 message, x86::reg32 wParam, x86::reg32 lParam)
{
    result->hWindow = hWnd;
    result->message = message;
    result->wParam = wParam;
    result->lParam = lParam;
    result->time = timeGetTickCount();
    result->pt.x = 0;
    result->pt.y = 0;
}

// Fills the MSG of a queued message; false for a wake-up, which carries none.
static bool toMessage(const pf::Message& message, MSG* result, bool removed)
{
    switch (message.type)
    {
    case MSG_USER:
    case MSG_USER1:
        storeMessage(result, message.window, message.code, message.data1, message.data2);
        return true;
    case MSG_KEY:
        if (removed)
            s_inputTaken.fetch_add(1);
        storeMessage(result, message.window, message.code ? WM_KEYDOWN : WM_KEYUP, message.data1, message.data2);
        return true;
    case MSG_CHAR:
        if (removed)
            s_inputTaken.fetch_add(1);
        storeMessage(result, message.window, WM_CHAR, message.data1, message.data2);
        return true;
    case MSG_QUIT:
        storeMessage(result, 0, WM_QUIT, 0, 0);
        return true;
    case MSG_WAKE:
    default:
        return false;
    }
}

x86::reg32 Window::getMessage(const x86::CPU& cpu, MSG* result)
{
    pf::Message message;
    PollRole role;
    for (;;)
    {
        const bool poller = role.acquire();
        if (!s_queue.wait(message, poller ? s_inputPollMs : s_idleWaitMs))
        {
            if (cpu.terminate)
                return 0;
            if (poller)
                Gamepad::updateKeys();
            continue;
        }
        if (cpu.terminate)
            return 0;
        if (toMessage(message, result, true))
            return result->message != WM_QUIT;
    }
}

// Wake-ups at the head of the queue are dropped; they only matter to waiting threads.
bool Window::peekMessage(MSG* result, bool remove)
{
    pf::Message message;
    for (;;)
    {
        if (!s_queue.peek(message, false))
            return false;
        if (message.type != MSG_WAKE)
            break;
        s_queue.peek(message, true);
    }
    if (remove)
        s_queue.peek(message, true);
    return toMessage(message, result, remove);
}

x86::reg32 Window::postMessage(x86::reg32 hWnd, x86::reg32 message, x86::reg32 wParam, x86::reg32 lParam)
{
    pf::Message m;
    m.type = (message >= WM_USER && message < WM_USER_END) ? MSG_USER : MSG_USER1;
    m.code = message;
    m.data1 = wParam;
    m.data2 = lParam;
    m.window = hWnd;
    s_queue.push(m);
    return 1;
}

// lParam: repeat count 1, scancode in bits 16-23, extended key bit 24; a key-up also has the previous-state and
// transition bits (30, 31).
void Window::pushKey(x86::reg32 virtualKey, x86::reg8 scancode, bool extended, bool down)
{
    pf::Message m;
    m.type = MSG_KEY;
    m.code = down ? 1 : 0;
    m.data1 = virtualKey;
    m.data2 = 1 | (x86::reg32(scancode) << 16) | (extended ? 0x01000000u : 0) | (down ? 0 : 0xC0000000u);
    m.window = s_focus.load();
    pushInput(m);
}

void Window::pushChar(x86::reg32 charCode)
{
    // lParam: repeat count 1, no scancode.
    pf::Message m = { MSG_CHAR, 0, charCode, 1, s_focus.load() };
    pushInput(m);
}

bool Window::inputPending()
{
    return s_inputTaken.load() != s_inputPosted.load();
}

void Window::postQuit()
{
    pf::Message m = { MSG_QUIT, 0, 0, 0, 0 };
    s_queue.push(m);
}

void Window::wake()
{
    pf::Message m = { MSG_WAKE, 0, 0, 0, 0 };
    s_queue.push(m);
}

}
