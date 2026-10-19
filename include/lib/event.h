#ifndef LIB_EVENT_H_
#define LIB_EVENT_H_
#include <lib/winapp.h>
#include <vector>

namespace win32
{

struct EventWaiter;

// Win32 waitable object: event, semaphore, mutex, or the end of a thread. A signal wakes only the threads waiting on
// this object; an object that a wait takes (auto-reset event, semaphore count, mutex) goes to the first waiter it
// satisfies (FIFO), as on Windows.
class Event : public GenericResource
{
public:
    enum Kind
    {
        AutoReset,      // event
        ManualReset,    // event; also the end of a thread
        Semaphore,
        Mutex
    };

    Event(Kind kind, bool initialState, x86::reg32 count = 0, x86::reg32 maximum = 0);
    ~Event();

    // Events.
    void reset();
    x86::reg32 pulse();
    // Makes the object reachable by its handle from any thread (Create*A); the destructor removes it.
    void registerHandle(x86::reg32 handle);
    // Another handle of the same object (a thread handle for the thread's end); removed with unregisterAlias.
    static void registerAlias(x86::reg32 handle, Event* event);
    static void unregisterAlias(x86::reg32 handle);
    // SetEvent by handle, safe against a concurrent CloseHandle; false if no event has the handle.
    static bool setHandle(x86::reg32 handle);
    // False if no event has the handle.
    static bool isHandleSet(x86::reg32 handle);
    // Sets an object that is not reachable by handle (the end of a thread).
    void set();
    // ReleaseSemaphore: false if the handle is not a semaphore or the count would pass the maximum.
    static bool releaseSemaphore(x86::reg32 handle, x86::reg32 count, x86::reg32* previous);
    // ReleaseMutex by the owning thread: false if the handle is not a mutex the thread owns.
    static bool releaseMutex(x86::reg32 handle, x86::reg32 threadId);
    // WaitForMultipleObjects: index of the signalled object (0 when waitAll), WAIT_TIMEOUT, or WAIT_FAILED (-1) when
    // a handle is not waitable or the thread terminates. timeout in milliseconds, negative for infinite. threadId
    // is the guest thread that waits (mutex ownership).
    static x86::reg32 wait(x86::CPU& cpu, const x86::reg32* handles, size_t count, bool waitAll, x86::sreg32 timeout,
                           x86::reg32 threadId);
    // Wakes every waiting thread so it checks its terminate flag.
    static void broadcastTerminate();

    static void init();
    static void fini();

private:
    bool signaledFor(const EventWaiter& waiter) const;
    void take(const EventWaiter& waiter);
    void release();
    static bool tryComplete(EventWaiter& waiter);

    std::vector<EventWaiter*>   m_waiters;  // FIFO
    std::vector<x86::reg32>     m_handles;  // handle and aliases
    Kind                        m_kind;
    bool                        m_isSet;    // events
    x86::reg32                  m_count;    // semaphore count, mutex recursion
    x86::reg32                  m_maximum;  // semaphore maximum
    x86::reg32                  m_owner;    // mutex owner thread, 0 when free
};

}

#endif /*!EVENT_H_*/
