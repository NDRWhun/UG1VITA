#include <lib/event.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <algorithm>
#include <unordered_map>

namespace win32
{

static const x86::reg32 WAIT_TIMEOUT = 0x102;
static const x86::reg32 WAIT_FAILED = 0xffffffff;
static const size_t MAXIMUM_WAIT_OBJECTS = 64;

// One blocked WaitFor*Object call. Lives on the waiting thread's stack while it is registered.
struct EventWaiter
{
    Event**         events;     // null once the object is closed
    size_t          count;
    bool            waitAll;
    bool            done;       // satisfied; result is valid
    bool            posted;     // semaphore posted and not taken yet
    x86::reg32      result;
    x86::reg32      threadId;   // guest thread (mutex ownership)
    pf::Semaphore*  semaphore;
};

// Guards all object state, the waiter lists and the semaphore pool. The lock and unlock calls are full barriers, so
// guest data published before a signal is visible to the thread the signal wakes.
static pf::LwMutex*                 s_lock = nullptr;
static std::vector<EventWaiter*>    s_waiters;      // every registered waiter, for broadcastTerminate
static std::vector<pf::Semaphore*>  s_semaphores;   // idle wake-up semaphores, count 0
static std::unordered_map<x86::reg32, Event*> s_handles;

static void wake(EventWaiter& waiter)
{
    waiter.posted = true;
    waiter.semaphore->post();
}

static void unregister(std::vector<EventWaiter*>& list, EventWaiter* waiter)
{
    list.erase(std::find(list.begin(), list.end(), waiter));
}

Event::Event(Kind kind, bool initialState, x86::reg32 count, x86::reg32 maximum)
    :   m_kind(kind)
    ,   m_isSet(initialState)
    ,   m_count(count)
    ,   m_maximum(maximum)
    ,   m_owner(0)
{
}

// A wait still registered goes on without the object, as a wait on a closed handle does on Windows.
Event::~Event()
{
    pf::LwLockGuard guard(*s_lock);
    for (x86::reg32 handle : m_handles)
        s_handles.erase(handle);
    for (EventWaiter* waiter : m_waiters)
    {
        for (size_t i = 0; i < waiter->count; ++i)
        {
            if (waiter->events[i] == this)
                waiter->events[i] = nullptr;
        }
    }
}

void Event::registerHandle(x86::reg32 handle)
{
    registerAlias(handle, this);
}

void Event::registerAlias(x86::reg32 handle, Event* event)
{
    pf::LwLockGuard guard(*s_lock);
    event->m_handles.push_back(handle);
    s_handles[handle] = event;
}

void Event::unregisterAlias(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::iterator it = s_handles.find(handle);
    if (it == s_handles.end())
        return;
    std::vector<x86::reg32>& handles = it->second->m_handles;
    handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
    s_handles.erase(it);
}

bool Event::setHandle(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::iterator it = s_handles.find(handle);
    if (it == s_handles.end() || it->second->m_kind > ManualReset)
        return false;
    it->second->m_isSet = true;
    it->second->release();
    return true;
}

bool Event::isHandleSet(x86::reg32 handle)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::const_iterator it = s_handles.find(handle);
    return it != s_handles.end() && it->second->m_isSet;
}

void Event::set()
{
    pf::LwLockGuard guard(*s_lock);
    m_isSet = true;
    release();
}

bool Event::releaseSemaphore(x86::reg32 handle, x86::reg32 count, x86::reg32* previous)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::iterator it = s_handles.find(handle);
    if (it == s_handles.end() || it->second->m_kind != Semaphore)
        return false;
    Event* semaphore = it->second;
    if (count == 0 || count > semaphore->m_maximum - semaphore->m_count)
        return false;
    if (previous)
        *previous = semaphore->m_count;
    semaphore->m_count += count;
    semaphore->release();
    return true;
}

bool Event::releaseMutex(x86::reg32 handle, x86::reg32 threadId)
{
    pf::LwLockGuard guard(*s_lock);
    std::unordered_map<x86::reg32, Event*>::iterator it = s_handles.find(handle);
    if (it == s_handles.end() || it->second->m_kind != Mutex || it->second->m_owner != threadId)
        return false;
    Event* mutex = it->second;
    if (--mutex->m_count == 0)
    {
        mutex->m_owner = 0;
        mutex->release();
    }
    return true;
}

void Event::init()
{
    s_lock = new pf::LwMutex("events");
}

void Event::fini()
{
    for (pf::Semaphore* semaphore : s_semaphores)
        delete semaphore;
    s_semaphores.clear();
    delete s_lock;
    s_lock = nullptr;
}

void Event::broadcastTerminate()
{
    pf::LwLockGuard guard(*s_lock);
    for (EventWaiter* waiter : s_waiters)
    {
        if (!waiter->posted)
            wake(*waiter);
    }
}

// Under s_lock.
bool Event::signaledFor(const EventWaiter& waiter) const
{
    switch (m_kind)
    {
    case Semaphore: return m_count > 0;
    case Mutex:     return m_owner == 0 || m_owner == waiter.threadId;
    default:        return m_isSet;
    }
}

// Under s_lock: what a satisfied wait takes from the object.
void Event::take(const EventWaiter& waiter)
{
    switch (m_kind)
    {
    case AutoReset: m_isSet = false; break;
    case Semaphore: --m_count; break;
    case Mutex:     m_owner = waiter.threadId; ++m_count; break;
    default:        break;
    }
}

// Under s_lock: completes the wait if its objects allow it now, taking what it acquires.
bool Event::tryComplete(EventWaiter& waiter)
{
    if (waiter.waitAll)
    {
        for (size_t i = 0; i < waiter.count; ++i)
        {
            if (!waiter.events[i] || !waiter.events[i]->signaledFor(waiter))
                return false;
        }
        for (size_t i = 0; i < waiter.count; ++i)
            waiter.events[i]->take(waiter);
        waiter.result = 0;
    }
    else
    {
        size_t i = 0;
        while (i < waiter.count && (!waiter.events[i] || !waiter.events[i]->signaledFor(waiter)))
            ++i;
        if (i == waiter.count)
            return false;
        waiter.events[i]->take(waiter);
        waiter.result = x86::reg32(i);
    }
    waiter.done = true;
    return true;
}

// Under s_lock: wakes the waiters the signalled object satisfies, in FIFO order, until it is taken.
void Event::release()
{
    for (size_t i = 0; i < m_waiters.size(); ++i)
    {
        EventWaiter& waiter = *m_waiters[i];
        if (!waiter.done && signaledFor(waiter) && tryComplete(waiter))
            wake(waiter);
    }
}

void Event::reset()
{
    pf::LwLockGuard guard(*s_lock);
    m_isSet = false;
}

// Releases the waiters a set would release, then resets the event.
x86::reg32 Event::pulse()
{
    pf::LwLockGuard guard(*s_lock);
    m_isSet = true;
    release();
    m_isSet = false;
    return 1;
}

x86::reg32 Event::wait(x86::CPU& cpu, const x86::reg32* handles, size_t count, bool waitAll, x86::sreg32 timeout,
                       x86::reg32 threadId)
{
    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS)
        return WAIT_FAILED;
    Event* events[MAXIMUM_WAIT_OBJECTS];
    Event** const begin = events;
    Event** const end = events + count;
    EventWaiter waiter = { events, count, waitAll, false, false, WAIT_TIMEOUT, threadId, nullptr };
    s_lock->lock();
    for (size_t i = 0; i < count; ++i)
    {
        std::unordered_map<x86::reg32, Event*>::const_iterator it = s_handles.find(handles[i]);
        if (it == s_handles.end())
        {
            s_lock->unlock();
            pf::logError("WaitFor*Object: 0x%x is not a waitable handle", handles[i]);
            return WAIT_FAILED;
        }
        events[i] = it->second;
    }
    if (tryComplete(waiter) || timeout == 0 || cpu.terminate)
    {
        s_lock->unlock();
        return waiter.done ? waiter.result : timeout == 0 ? WAIT_TIMEOUT : WAIT_FAILED;
    }
    if (s_semaphores.empty())
    {
        waiter.semaphore = new pf::Semaphore("event_wait", 0);
    }
    else
    {
        waiter.semaphore = s_semaphores.back();
        s_semaphores.pop_back();
    }
    for (Event** e = begin; e < end; ++e)
        (*e)->m_waiters.push_back(&waiter);
    s_waiters.push_back(&waiter);

    const uint64_t deadline = timeout > 0 ? pf::ticksUs() + uint64_t(timeout) * 1000 : 0;
    while (!waiter.done && !cpu.terminate)
    {
        bool woken;
        if (timeout < 0)
        {
            s_lock->unlock();
            waiter.semaphore->wait();
            woken = true;
        }
        else
        {
            const uint64_t now = pf::ticksUs();
            if (now >= deadline)
                break;
            const uint64_t remaining = deadline - now;
            s_lock->unlock();
            woken = waiter.semaphore->waitForUs(uint32_t(std::min<uint64_t>(remaining, 0x7fffffff)));
        }
        s_lock->lock();
        if (woken)
            waiter.posted = false;
    }

    for (Event** e = begin; e < end; ++e)
    {
        if (*e)
            unregister((*e)->m_waiters, &waiter);
    }
    unregister(s_waiters, &waiter);
    // A wake-up posted after the timeout is still pending: take it so the pooled semaphore is at 0.
    if (waiter.posted)
        waiter.semaphore->wait();
    s_semaphores.push_back(waiter.semaphore);
    s_lock->unlock();
    if (waiter.done)
        return waiter.result;
    return cpu.terminate ? WAIT_FAILED : WAIT_TIMEOUT;
}

}
